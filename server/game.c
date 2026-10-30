/* server/game.c
Server simulation tick: movement, bomb/fuse, rounds, pickups, win state.

Fixed tick (with #12, snapshots): the baseline moved a player 1 pixel each
time an Input message arrived, so speed depended on the network. Now an Input
only says which keys are held (game_set_keys), and game_tick() moves every
player by PLAYER_SPEED / TICK_HZ pixels per tick, TICK_HZ times per second. */

#include <string.h>

#include "game.h"
#include "collision.h"
#include "timing.h"

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int int_snapshot_every = TICK_HZ / SNAPSHOT_HZ;

// Pixels moved per tick while a key is held
#define TICK_STEP (PLAYER_SPEED / TICK_HZ)

// Keys each player holds (KEY_* bitmask); index = player ID - 1
static uint8_t arr_u8_keys[MAX_PLAYERS] = {0};

// When each player's last Input arrived (timing_now_ns); index = player ID - 1
static uint64_t arr_u64_last_input_ns[MAX_PLAYERS] = {0};

// Ticks done since the server started
static uint32_t u_tick = 0;

// The latest snapshot, and whether it is waiting to be published
static GameSnapshot snapshot = {0};
static bool b_snapshot_ready = false;

void game_set_keys(int a_int_player_id, uint8_t a_u8_keys)
{
	// The ID comes from the network: never index the table with an unchecked
	// value (the baseline wrote arr_Players[id-1] for any id)
	if (a_int_player_id < 1 || a_int_player_id > MAX_PLAYERS)
		return;

	pthread_mutex_lock(&mutex);
	game_set_keys_locked(a_int_player_id, a_u8_keys);
	pthread_mutex_unlock(&mutex);
}

void game_set_keys_locked(int a_int_player_id, uint8_t a_u8_keys)
{
	if (a_int_player_id < 1 || a_int_player_id > MAX_PLAYERS ||
	    !arr_Players[a_int_player_id - 1].b_active)
		return;
	arr_u8_keys[a_int_player_id - 1] = a_u8_keys;
	arr_u64_last_input_ns[a_int_player_id - 1] = timing_now_ns();
}

void game_reset_player(int a_int_index)
{
	// Keys left over from the slot's previous player would move the new one
	arr_u8_keys[a_int_index] = 0;
	arr_u64_last_input_ns[a_int_index] = timing_now_ns();
}

uint64_t game_last_input_ns(int a_int_index)
{
	return arr_u64_last_input_ns[a_int_index];
}

// Clamp a coordinate to [0, a_int_max]
static int clamp_coord(int a_int_v, int a_int_max)
{
	return a_int_v < 0 ? 0 : (a_int_v > a_int_max ? a_int_max : a_int_v);
}

/* Move player index a_int_i one tick by its held keys. Each axis is tried on
its own, so a player blocked sideways by another player can still move up or
down. A move into another player is cancelled (as in the baseline); a move
past the window edge stops at the edge. Fills the player's velocity in
*a_p_state. The caller holds the mutex. */
static void move_player(int a_int_i, PlayerState *a_p_state)
{
	uint8_t u8_keys = arr_u8_keys[a_int_i];
	// -1, 0 or +1 per axis; opposite keys cancel out
	int int_dx = ((u8_keys & KEY_RIGHT) != 0) - ((u8_keys & KEY_LEFT) != 0);
	int int_dy = ((u8_keys & KEY_DOWN) != 0) - ((u8_keys & KEY_UP) != 0);

	int int_x = arr_Players[a_int_i].x;
	int int_y = arr_Players[a_int_i].y;

	if (int_dx != 0)
	{
		int int_new_x = clamp_coord(int_x + int_dx * TICK_STEP, WIDTH - SQ_WIDTH);
		if (!collision_detect(arr_Players, MAX_PLAYERS, a_int_i + 1, int_new_x, int_y))
			int_x = int_new_x;
	}
	if (int_dy != 0)
	{
		int int_new_y = clamp_coord(int_y + int_dy * TICK_STEP, HEIGHT - SQ_WIDTH);
		if (!collision_detect(arr_Players, MAX_PLAYERS, a_int_i + 1, int_x, int_new_y))
			int_y = int_new_y;
	}

	// Velocity actually moved, in pixels per second (0 when blocked):
	// clients use it to draw the player between snapshots (#15)
	a_p_state->vx = (int16_t)((int_x - arr_Players[a_int_i].x) * TICK_HZ);
	a_p_state->vy = (int16_t)((int_y - arr_Players[a_int_i].y) * TICK_HZ);

	arr_Players[a_int_i].x = int_x;
	arr_Players[a_int_i].y = int_y;
}

void game_tick(void)
{
	pthread_mutex_lock(&mutex);
	u_tick++;

	// Build the snapshot while moving: one pass over the players
	memset(&snapshot, 0, sizeof snapshot);
	snapshot.tick = u_tick;
	snapshot.phase = PHASE_LOBBY;   // no rounds yet: everyone moves freely
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_Players[i].b_active)
			continue;   // inactive slot: flags 0, the rest unused
		PlayerState *p_state = &snapshot.players[i];
		move_player(i, p_state);
		p_state->flags = PF_ACTIVE | PF_ALIVE;
		p_state->x = (int16_t)arr_Players[i].x;
		p_state->y = (int16_t)arr_Players[i].y;
	}

	// Publish every int_snapshot_every-th tick (all of them at 60 Hz)
	if (u_tick % (uint32_t)int_snapshot_every == 0)
		b_snapshot_ready = true;
	pthread_mutex_unlock(&mutex);
}

bool game_take_snapshot(GameSnapshot *a_p_out)
{
	if (!b_snapshot_ready)
		return false;
	*a_p_out = snapshot;
	b_snapshot_ready = false;
	return true;
}
