/* server/lobby.c
Player slots: join (nonce), leave, liveliness timeout. */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lobby.h"
#include "game.h"
#include "collision.h"
#include "net.h"
#include "log.h"
#include "auth.h"
#include "timing.h"
#include "handoff.h"
#include "spsc_ring.h"

// Each queue has one writer and one reader. Only the main thread validates
// queued Input/Leave samples, so a stale queued MAC is checked against the
// CURRENT session before it can change the game or refresh a timeout.
SPSC_RING_DEFINE(JoinRing, JoinRequest, JOIN_QUEUE)
SPSC_RING_DEFINE(ResultRing, JoinResult, JOIN_QUEUE)
SPSC_RING_DEFINE(InputRing, Input, INPUT_QUEUE)
SPSC_RING_DEFINE(LeaveRing, Leave, JOIN_QUEUE)
static JoinRing join_ring;
static ResultRing result_ring;
static InputRing input_ring;
static LeaveRing leave_ring;

int int_player_timeout_s = PLAYER_TIMEOUT_S;

// Only written by the main thread
static int int_num_active_players = 0;

// Nonce of the JoinRequest that each slot was given to (0 = none, or the
// client sent no nonce). Only used by the main thread.
static uint64_t arr_u64_slot_nonce[MAX_PLAYERS] = {0};

// Bounded join requests waiting for authentication/admission, oldest first.
// Written by the DDS worker, emptied by the main thread, under the mutex.
static JoinRequest arr_pending[JOIN_QUEUE];
static int int_num_pending = 0;

// Player IDs from Leave messages waiting for the main thread.
// Written by the DDS worker, emptied by the main thread, under the mutex.
static int arr_int_leaving[JOIN_QUEUE];
static int int_num_leaving = 0;

// Answers waiting for the DDS worker (oldest first).
// Written by the main thread, emptied by the DDS worker, under the mutex.
static JoinResult arr_results[JOIN_QUEUE];
static int int_num_results = 0;

bool lobby_signal_join(const JoinRequest *a_request)
{
	if (handoff_spsc())
		return JoinRing_push(&join_ring, a_request);
	pthread_mutex_lock(&mutex);
	bool b_queued = int_num_pending < JOIN_QUEUE;
	if (b_queued)
		arr_pending[int_num_pending++] = *a_request;
	pthread_mutex_unlock(&mutex);
	return b_queued;
}

bool lobby_submit_input(const Input *a_input)
{
	return handoff_spsc() ? InputRing_push(&input_ring, a_input) :
	                       lobby_receive_input(a_input);
}

bool lobby_submit_leave(const Leave *a_leave)
{
	return handoff_spsc() ? LeaveRing_push(&leave_ring, a_leave) :
	                       lobby_receive_leave(a_leave);
}

bool lobby_receive_input(const Input *a_input)
{
	pthread_mutex_lock(&mutex);
	bool b_ok = a_input->player_id >= 1 && a_input->player_id <= MAX_PLAYERS &&
		arr_Players[a_input->player_id - 1].b_active && auth_input(a_input, timing_now_ns());
	if (b_ok)
		game_set_keys_locked(a_input->player_id, a_input->keys);
	pthread_mutex_unlock(&mutex);
	return b_ok;
}

bool lobby_receive_leave(const Leave *a_leave)
{
	pthread_mutex_lock(&mutex);
	bool b_ok = int_num_leaving < JOIN_QUEUE && a_leave->player_id >= 1 &&
		a_leave->player_id <= MAX_PLAYERS && arr_Players[a_leave->player_id - 1].b_active &&
		auth_leave(a_leave);
	if (b_ok)
		arr_int_leaving[int_num_leaving++] = a_leave->player_id;
	pthread_mutex_unlock(&mutex);
	return b_ok;
}

bool lobby_take_result(JoinResult *a_p_out)
{
	if (handoff_spsc())
		return ResultRing_pop(&result_ring, a_p_out);
	pthread_mutex_lock(&mutex);
	bool b_found = int_num_results > 0;
	if (b_found)
	{
		*a_p_out = arr_results[0];
		// Close the gap (at most JOIN_QUEUE entries, and joins are rare)
		int_num_results--;
		for (int i = 0; i < int_num_results; i++)
			arr_results[i] = arr_results[i + 1];
	}
	pthread_mutex_unlock(&mutex);
	return b_found;
}

// Find the slot already given to a_u64_nonce. Returns its index, or -1.
// Nonce 0 never matches: clients without a nonce cannot be told apart.
static int find_slot_by_nonce(uint64_t a_u64_nonce)
{
	if (a_u64_nonce == 0 || !net_join_nonce())
		return -1;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_Players[i].b_active && arr_u64_slot_nonce[i] == a_u64_nonce)
			return i;
	}
	return -1;
}

// Give a free slot to a new player at a random position where it does not
// overlap anyone. Returns the slot index, or -1 if the game is full.
// The caller holds the mutex.
static int admit_player(uint64_t a_u64_nonce)
{
	if (int_num_active_players == MAX_PLAYERS)
		return -1;

	// Loop through the players list
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_Players[i].b_active == false)
		{
			// Set this index to active
			arr_Players[i].b_active = true;
			// Increment the number of active players
			int_num_active_players++;
			arr_u64_slot_nonce[i] = a_u64_nonce;
			game_reset_player(i);

			// Calculate initial coords of the new player
			int int_rand_x = 0;
			int int_rand_y = 0;
			while (1)
			{
				int_rand_x = rand() % (WIDTH - SQ_WIDTH + 1);
				int_rand_y = rand() % (HEIGHT - SQ_WIDTH + 1);
				// Function don't have to lock/unlock mutex
				bool b_res = collision_detect(arr_Players,
					MAX_PLAYERS, i + 1, int_rand_x, int_rand_y);
				// If no collision detected
				if (b_res == false)
				{
					break;
				}
			}

			// Set the coords
			arr_Players[i].x = int_rand_x;
			arr_Players[i].y = int_rand_y;
			return i;
		}
	}
	return -1;
}

// Free slot a_int_index: the player disappears from the next snapshot, and
// its nonce is forgotten. The caller holds the mutex.
static void free_slot(int a_int_index)
{
	auth_forget(a_int_index + 1);
	arr_Players[a_int_index].b_active = false;
	arr_u64_slot_nonce[a_int_index] = 0;
	game_reset_player(a_int_index);
	int_num_active_players--;
}

void lobby_poll(void)
{
	if (handoff_spsc())
	{
		// Bound work per pass even if a peer continually supplies samples.
		Input input;
		for (unsigned i = 0; i < INPUT_QUEUE && InputRing_pop(&input_ring, &input); i++)
			lobby_receive_input(&input);
		Leave leave;
		for (unsigned i = 0; i < JOIN_QUEUE && LeaveRing_pop(&leave_ring, &leave); i++)
			lobby_receive_leave(&leave);
		while (int_num_pending < JOIN_QUEUE &&
		       JoinRing_pop(&join_ring, &arr_pending[int_num_pending]))
			int_num_pending++;
		// The spinning main loop must not acquire the state mutex when there
		// is no work; the DDS thread no longer accesses this state directly.
		if (int_num_pending == 0 && int_num_leaving == 0)
			return;
	}
	pthread_mutex_lock(&mutex);
	// Leaves first, so a slot freed now can go to a request in the same pass.
	// The ID comes from the network: ignore IDs out of range or not playing.
	// Network leaves have already passed MAC/sequence validation while locked.
	for (int i = 0; i < int_num_leaving; i++)
	{
		int int_index = arr_int_leaving[i] - 1;
		if (int_index >= 0 && int_index < MAX_PLAYERS && arr_Players[int_index].b_active)
		{
			free_slot(int_index);
			LOG_INFO("Player %d left\n", int_index + 1);
		}
	}
	int_num_leaving = 0;

	// Every waiting request, oldest first, while there is room for the answer
	// (the worker empties the answers every pass, so there almost always is)
	int int_done = 0;
	while (int_done < int_num_pending &&
	       (handoff_spsc() ? ResultRing_space(&result_ring) > 0 : int_num_results < JOIN_QUEUE))
	{
		const JoinRequest *p_request = &arr_pending[int_done++];
		uint64_t u64_nonce = p_request->nonce;
		JoinResult result = { .nonce = u64_nonce, .status = JOIN_OK };
		if (auth_enabled())
		{
			int int_free_id = 0;
			for (int i = 0; i < MAX_PLAYERS; i++)
				if (!arr_Players[i].b_active) { int_free_id = i + 1; break; }
			auth_join(p_request, int_free_id, timing_now_ns(), &result);
			if (result.status == JOIN_OK && !arr_Players[result.player_id - 1].b_active)
				admit_player(u64_nonce);
			if (handoff_spsc())
				ResultRing_push(&result_ring, &result);
			else
				arr_results[int_num_results++] = result;
			continue;
		}

		// A request with a nonce that already holds a slot: the client did
		// not get (or not yet get) our answer and asked again. Answer with the
		// same slot instead of filling a second one.
		int int_slot = find_slot_by_nonce(u64_nonce);
		if (int_slot >= 0)
			LOG_INFO("Repeated join request, player %d again\n", int_slot + 1);
		else
			int_slot = admit_player(u64_nonce);

		if (int_slot >= 0)
			result.player_id = (uint8_t)(int_slot + 1);   // ID = index + 1
		else
			result.status = JOIN_FULL;
		if (handoff_spsc())
			ResultRing_push(&result_ring, &result);
		else
			arr_results[int_num_results++] = result;
	}
	// Keep the requests that did not fit for the next call
	int_num_pending -= int_done;
	for (int i = 0; i < int_num_pending; i++)
		arr_pending[i] = arr_pending[i + int_done];
	// Do not retain request proofs beyond the pending queue.
	memset(arr_pending + int_num_pending, 0, (size_t)int_done * sizeof arr_pending[0]);
	pthread_mutex_unlock(&mutex);
}

void lobby_check_timeouts(uint64_t a_u64_now_ns)
{
	if (int_player_timeout_s <= 0 || !net_leave())
		return;
	uint64_t u64_timeout_ns = (uint64_t)int_player_timeout_s * 1000000000ull;

	pthread_mutex_lock(&mutex);
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		// The worker may record an Input just after a_u64_now_ns was read, so
		// the last input can be later than "now": not timed out
		uint64_t u64_last_ns = game_last_input_ns(i);
		if (arr_Players[i].b_active && a_u64_now_ns > u64_last_ns &&
		    a_u64_now_ns - u64_last_ns > u64_timeout_ns)
		{
			free_slot(i);
			LOG_INFO("Player %d timed out (no input for %d s)\n", i + 1, int_player_timeout_s);
		}
	}
	pthread_mutex_unlock(&mutex);
}
