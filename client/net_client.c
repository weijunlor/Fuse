/* client/net_client.c
Client DDS worker thread: sends input, receives snapshots.

Since #12 the client receives one GameSnapshot per published server tick
(every player at once) instead of one Position message per player, and sends
the keys it holds instead of single steps: every 10 ms while any key is held
(the baseline's rate), plus once when the last key is released so the server
stops the player.

#13 (input on change, the default): an Input is sent only when the held keys
change, and repeated every INPUT_REPEAT_MS in case one was lost (Input will be
best-effort, #14 in common/net.h). A held arrow key then costs ~10 messages per second instead
of ~100. --input-send every-10ms keeps the old behaviour for measurements.

Join nonce (common/net.h): the client sends the same random nonce in every
JoinRequest and accepts only the JoinResponse carrying it; responses to other
clients are taken and dropped. FUSE_JOIN_NONCE=off: nonce 0 and the first
response is taken, as in the baseline.

Leave (common/net.h): when the window closes, the worker sends a Leave so the
server frees the slot at once. If the server dropped this client (no Input
for its timeout, e.g. the network was cut), the client sees its own slot
missing from the snapshots and joins again with a new nonce.

#2 on the client (wait instead of polling, the default): the worker blocks in
a DDS waitset on the join-response and snapshot readers and on a guard
condition that the main thread sets when the held keys change
(net_client_wake()) or the window closes. The wait ends at the latest when
the next timed message is due (join request every 1 s, Input repeat every
INPUT_REPEAT_MS). The baseline loop busy-polled while no key was held (one
core per idle client) and slept 10 ms per pass while one was, so a key press
could wait up to 10 ms before it was sent. FUSE_CLIENT_WAIT=spin keeps that
loop for measurements.

#15: every snapshot taken is passed to interp_push() (client/interp.h) with
the time it arrived; the render thread draws interpolated positions. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_client.h"
#include "net.h"
#include "fuse.h"
#include "input.h"
#include "interp.h"
#include "timing.h"
#include "log.h"

int int_player_id = -1;

bool b_input_on_change = true;

// #13: repeat the current keys this often even if they did not change, so a
// lost Input is corrected within this time
#define INPUT_REPEAT_MS 100

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

bool b_client_wait = true;

// Set by net_client_stop() (main thread), read by the worker, under the mutex
static bool b_quit = false;

// Set by net_client_wake() (main thread), waited on by the worker. Created
// before the worker starts and never changed, and a guard condition is
// thread-safe, so no mutex is needed around it.
static dds_entity_t guard_wake = 0;

void net_client_init(void)
{
	const char *str_wait = getenv("FUSE_CLIENT_WAIT");
	b_client_wait = !(str_wait != NULL && strcmp(str_wait, "spin") == 0);
	LOG_INFO("Client network wait: %s\n",
	         b_client_wait ? "waitset" : "poll (as the baseline)");
	if (b_client_wait)
	{
		// Owned by the library: the worker creates the participant later
		guard_wake = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE);
		if (guard_wake < 0)
			DDS_FATAL("dds_create_guardcondition: %s\n", dds_strretcode(-guard_wake));
	}
}

void net_client_wake(void)
{
	if (guard_wake > 0)
		dds_set_guardcondition(guard_wake, true);
}

void net_client_stop(void)
{
	pthread_mutex_lock(&mutex);
	b_quit = true;
	pthread_mutex_unlock(&mutex);
	// A waiting worker would otherwise only notice at its next timeout
	net_client_wake();
}

void *net_client_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t response_reader;
	dds_entity_t snapshot_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;
	dds_entity_t leave_writer;

	//Pointer to unknown type
	void *samples_response[JOIN_TAKE];
	dds_sample_info_t infos_response[JOIN_TAKE];
	void *samples_snapshot[MAX_SAMPLES];
	dds_sample_info_t infos_snapshot[MAX_SAMPLES];
	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net);

	/* ----------------------------------------------------- */
	/* Create writers                                        */
	/* ----------------------------------------------------- */
	join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
	input_writer = net_create_writer(&net, net.input_topic, NET_BEST_EFFORT);
	// Reliable: a lost Leave would keep the player in the game until the timeout
	leave_writer = net_create_writer(&net, net.leave_topic, NET_RELIABLE_ALL);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);

	/* #2: wake on a join response, a snapshot or the guard condition. A read
	condition on DDS_ANY_STATE stays triggered while samples are in the
	reader, so both readers are emptied on every pass (in wait mode the
	responses meant for other clients are taken and dropped even after we
	have joined). */
	dds_entity_t waitset = 0;
	if (b_client_wait)
	{
		waitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
		if (waitset < 0)
			DDS_FATAL("dds_create_waitset: %s\n", dds_strretcode(-waitset));
		dds_entity_t arr_readers[2] = { response_reader, snapshot_reader };
		for (int i = 0; i < 2; i++)
		{
			dds_entity_t condition = dds_create_readcondition(arr_readers[i], DDS_ANY_STATE);
			if (condition < 0)
				DDS_FATAL("dds_create_readcondition: %s\n", dds_strretcode(-condition));
			rc = dds_waitset_attach(waitset, condition, arr_readers[i]);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-rc));
		}
		rc = dds_waitset_attach(waitset, guard_wake, 0);
		if (rc != DDS_RETCODE_OK)
			DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-rc));
	}

	// One nonce for this client, the same in every request (so the server
	// knows a repeated request); name and proof stay empty until the login is
	// added
	JoinRequest request = {0};
	bool b_join_nonce = net_join_nonce();
	if (b_join_nonce)
		request.nonce = net_random_nonce();

	// Input message: sequence number counts every input sent
	Input input = {0};
	// Keys in the last input sent, to send one more input when they are released
	uint8_t u8_sent_keys = 0;
	// When the last input was sent (#13 repeats it every INPUT_REPEAT_MS)
	dds_time_t last_input_time = 0;
	// True once our own player has appeared in a snapshot; if it disappears
	// after that, the server has dropped us (snapshots taken before the
	// server admitted us do not show us yet, so they must not count)
	bool b_seen_self = false;

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	for (int i = 0; i < JOIN_TAKE; i++)
		samples_response[i] = JoinResponse__alloc();

	samples_snapshot[0] = GameSnapshot__alloc();

	// Record the time now
	uint64_t last_join_req_time = dds_time();
	while (1)
	{
		// If not in a game yet
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		bool b_quit_temp = b_quit;
		pthread_mutex_unlock(&mutex);
		// The window was closed
		if (b_quit_temp)
			break;
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			// Every waiting response (every client receives all of them)
			rc = dds_take(response_reader, samples_response, infos_response, JOIN_TAKE, JOIN_TAKE);

			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			int int_resp_player_id = -1;
			for (int i = 0; i < rc && int_resp_player_id == -1; i++)
			{
				JoinResponse *response = (JoinResponse *) samples_response[i];
				// Only the response to our own request (any one without nonce)
				if (!infos_response[i].valid_data ||
				    (b_join_nonce && response->nonce != request.nonce))
					continue;
				LOG_INFO("Join response received\n");
				// If server responded with an invalid player ID
				if (response->status != JOIN_OK || response->player_id == 0)
				{
					LOG_WARN("Unable to join\n");
					int_resp_player_id = 0;
				}
				else
					int_resp_player_id = response->player_id;
			}

			if (int_resp_player_id > 0)
			{
				pthread_mutex_lock(&mutex);
				// Update player ID
				int_player_id = int_resp_player_id;
				pthread_mutex_unlock(&mutex);
			}
			else if (rc == 0 && !b_client_wait)
			{
				/* Polling sleep (the waitset replaces it). */
				dds_sleepfor (DDS_MSECS (20));
			}

			// Send request every 1s
			// Get the current time
			uint64_t curr_time = dds_time();
			// Get the time difference
			if (curr_time - last_join_req_time > DDS_SECS(1))
			{
				// Update the last req time
				last_join_req_time = curr_time;

				// Send join request
				rc = dds_write(join_writer, &request);
				LOG_INFO("Join request sent\n");
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
			}
		}

		// Client is an active player
		else
		{
			pthread_mutex_lock(&mutex);
			uint8_t u8_keys_temp = u8_keys;
			pthread_mutex_unlock(&mutex);

			bool b_send;
			if (b_input_on_change)
			{
				// #13: when the keys changed, or INPUT_REPEAT_MS after the
				// last input (held or not, so a lost release is resent too)
				dds_time_t now = dds_time();
				b_send = u8_keys_temp != u8_sent_keys ||
				         now - last_input_time >= DDS_MSECS(INPUT_REPEAT_MS);
				if (b_send)
					last_input_time = now;
			}
			else
			{
				// Every 10 ms while keys are held, and once more when released
				b_send = u8_keys_temp != 0 || u8_sent_keys != 0;
			}

			if (b_send)
			{
				input.player_id = (uint8_t)int_player_id;
				input.seq++;
				input.keys = u8_keys_temp;
				// Send input
				rc = dds_write(input_writer, &input);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				LOG_DEBUG("Input sent: keys 0x%02x\n", (unsigned)u8_keys_temp);
				u8_sent_keys = u8_keys_temp;
			}

			// Poll mode (FUSE_CLIENT_WAIT=spin): while a key is held the loop
			// waits 10 ms per pass in both input modes, as the baseline did
			// after each input; otherwise it busy-polls, as the baseline did.
			// Wait mode: the waitset at the end of the loop replaces both.
			if (!b_client_wait && (u8_keys_temp != 0 || (b_send && !b_input_on_change)))
				dds_sleepfor(DDS_MSECS(10));

			// Wait mode: responses to other clients' joins keep arriving;
			// drop them, or the read condition would never let us sleep
			if (b_client_wait)
			{
				rc = dds_take(response_reader, samples_response, infos_response,
				              JOIN_TAKE, JOIN_TAKE);
				if (rc < 0)
					DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
			}
		}

		// Listen for snapshots
		rc = dds_take(snapshot_reader, samples_snapshot, infos_snapshot, MAX_SAMPLES, MAX_SAMPLES);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_snapshot[0].valid_data))
		{
			GameSnapshot *snapshot = (GameSnapshot *) samples_snapshot[0];
			LOG_DEBUG("Snapshot %u received\n", (unsigned)snapshot->tick);
			uint64_t u_received_ns = timing_now_ns();
			// Copy every player at once, so a frame never shows a mix of
			// two ticks (the baseline updated one player per message)
			pthread_mutex_lock(&mutex);
			interp_push(snapshot, u_received_ns);
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				const PlayerState *p_state = &snapshot->players[i];
				arr_Players[i].b_active = (p_state->flags & PF_ACTIVE) != 0;
				arr_Players[i].x = p_state->x;
				arr_Players[i].y = p_state->y;
			}

			// Our own slot (if we are playing): still there?
			if (int_player_id_temp >= 1 && int_player_id_temp <= MAX_PLAYERS && net_leave())
			{
				if (arr_Players[int_player_id_temp - 1].b_active)
					b_seen_self = true;
				else if (b_seen_self)
				{
					// Dropped by the server: join again. A new nonce, so a
					// late response to the old one cannot be taken by mistake.
					LOG_WARN("Removed by the server (timed out), joining again\n");
					int_player_id = -1;
					b_seen_self = false;
					if (b_join_nonce)
						request.nonce = net_random_nonce();
				}
			}
			pthread_mutex_unlock(&mutex);
		}

		/* #2: sleep until a snapshot or join response arrives, the keys
		change or the window closes (guard condition), or the next timed
		message is due, whichever comes first. */
		if (b_client_wait)
		{
			dds_time_t now = dds_time();
			dds_duration_t timeout;
			if (int_player_id_temp == -1)
				// Next join request (sent every 1 s until answered)
				timeout = (dds_time_t)last_join_req_time + DDS_SECS(1) - now;
			else if (b_input_on_change)
				// Next Input repeat (#13)
				timeout = last_input_time + DDS_MSECS(INPUT_REPEAT_MS) - now;
			else
				// Old input rate: every 10 ms while keys are held (and once
				// after release); with no keys only a key press wakes us
				timeout = u8_sent_keys != 0 ? DDS_MSECS(10) : DDS_INFINITY;
			if (timeout > 0)
			{
				dds_attach_t arr_triggered[3];
				rc = dds_waitset_wait(waitset, arr_triggered, 3, timeout);
				if (rc < 0)
					DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
			}
			// Reset the guard before the next pass reads the keys, so a key
			// change during that pass is not lost (it wakes the next wait)
			bool b_woken;
			dds_take_guardcondition(guard_wake, &b_woken);
		}
	}

	// Tell the server we are leaving, so it frees the slot now instead of
	// after the timeout, and wait (briefly) until it has the message: the
	// process exits right after this
	if (int_player_id > 0 && net_leave())
	{
		Leave leave = {0};
		leave.player_id = (uint8_t)int_player_id;
		leave.seq = ++input.seq;   // continues the Input sequence (see the IDL)
		rc = dds_write(leave_writer, &leave);
		if (rc != DDS_RETCODE_OK)
			LOG_WARN("Leave not sent: %s\n", dds_strretcode(-rc));
		else if (dds_wait_for_acks(leave_writer, DDS_MSECS(500)) != DDS_RETCODE_OK)
			LOG_WARN("Leave not acknowledged (server gone?)\n");
		else
			LOG_INFO("Left the game\n");
	}
	dds_delete(net.participant);
	return NULL;
}
