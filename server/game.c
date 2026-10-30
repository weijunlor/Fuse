/* server/game.c
Server simulation tick: movement, bomb/fuse, rounds, pickups, win state.

Fixed tick (with #12, snapshots): the baseline moved a player 1 pixel each
time an Input message arrived, so speed depended on the network. Now an Input
only says which keys are held (game_set_keys), and game_tick() moves every
player by PLAYER_SPEED / TICK_HZ pixels per tick, TICK_HZ times per second. */

#include <string.h>
#include <stdlib.h>

#include "game.h"
#include "collision.h"
#include "timing.h"
#include "log.h"

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int int_snapshot_every = TICK_HZ / SNAPSHOT_HZ;

// Pixels moved per tick while a key is held
#define TICK_STEP (PLAYER_SPEED / TICK_HZ)
#define PLAYER_START_LIVES 3
#define COUNTDOWN_TICKS (3 * TICK_HZ)
#define FUSE_TICKS (10 * TICK_HZ)
#define ROUND_OVER_TICKS (2 * TICK_HZ)
#define PASS_COOLDOWN_TICKS (TICK_HZ / 2)
#define DASH_TICKS (TICK_HZ / 5)
#define DASH_COOLDOWN_TICKS TICK_HZ
#define SPEED_PICKUP_TICKS (5 * TICK_HZ)
#define MAX_PICKUPS 4

// Keys each player holds (KEY_* bitmask); index = player ID - 1
static uint8_t arr_u8_keys[MAX_PLAYERS] = {0};
static uint8_t arr_u8_lives[MAX_PLAYERS] = {0};
static uint8_t arr_u8_shield[MAX_PLAYERS] = {0};
static uint16_t arr_u16_speed_ticks[MAX_PLAYERS] = {0};
static uint16_t arr_u16_dash_ticks[MAX_PLAYERS] = {0};
static uint16_t arr_u16_dash_cooldown[MAX_PLAYERS] = {0};
static bool arr_b_dash_was_down[MAX_PLAYERS] = {0};

// When each player's last Input arrived (timing_now_ns); index = player ID - 1
static uint64_t arr_u64_last_input_ns[MAX_PLAYERS] = {0};

// Ticks done since the server started
static uint32_t u_tick = 0;
static uint8_t u8_phase = PHASE_LOBBY;
static uint8_t u8_bomb_holder = 0;
static uint32_t u_countdown_ticks = 0;
static uint32_t u_fuse_ticks = 0;
static uint32_t u_round_over_ticks = 0;
static uint8_t u8_previous_holder = 0;
static uint32_t u_pass_cooldown_ticks = 0;
static Pickup arr_pickups[MAX_PICKUPS] = {0};
static Explosion arr_explosions[4] = {0};

// The latest snapshot, and whether it is waiting to be published
static GameSnapshot snapshot = {0};
static bool b_snapshot_ready = false;

static void clear_round_transients(void)
{
	memset(arr_pickups, 0, sizeof arr_pickups);
	memset(arr_explosions, 0, sizeof arr_explosions);
}

void game_set_keys(int a_int_player_id, uint8_t a_u8_keys)
{
	// The ID comes from the network: never index the table with an unchecked
	// value (the baseline wrote arr_Players[id-1] for any id)
	if (a_int_player_id < 1 || a_int_player_id > MAX_PLAYERS)
		return;

	uint64_t u64_now_ns = timing_now_ns();
	pthread_mutex_lock(&mutex);
	arr_u8_keys[a_int_player_id - 1] = a_u8_keys;
	arr_u64_last_input_ns[a_int_player_id - 1] = u64_now_ns;
	pthread_mutex_unlock(&mutex);
}

void game_reset_player(int a_int_index)
{
	// Keys left over from the slot's previous player would move the new one
	arr_u8_keys[a_int_index] = 0;
	arr_u8_lives[a_int_index] = PLAYER_START_LIVES;
	arr_u8_shield[a_int_index] = 0;
	arr_u16_speed_ticks[a_int_index] = 0;
	arr_u16_dash_ticks[a_int_index] = 0;
	arr_u16_dash_cooldown[a_int_index] = 0;
	arr_b_dash_was_down[a_int_index] = false;
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

static int count_active_players(void)
{
	int int_count = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
		int_count += arr_Players[i].b_active;
	return int_count;
}

static int count_alive_players(void)
{
	int int_count = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
		int_count += arr_Players[i].b_active && arr_u8_lives[i] > 0;
	return int_count;
}

static uint8_t choose_bomb_holder(void)
{
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_Players[i].b_active && arr_u8_lives[i] > 0)
			return (uint8_t)(i + 1);
	}
	return 0;
}

static uint16_t ticks_to_ms(uint32_t a_u_ticks)
{
	uint32_t u_ms = (a_u_ticks * 1000u) / TICK_HZ;
	return (uint16_t)(u_ms > UINT16_MAX ? UINT16_MAX : u_ms);
}

static bool players_touch(int a_int_left, int a_int_right)
{
	// Movement advances in TICK_STEP-sized increments, so a player can stop
	// one or two pixels short of an exact edge. Treat that distance as contact
	// for tagging without allowing the player rectangles to overlap.
	const int int_tolerance = TICK_STEP;
	return arr_Players[a_int_left].x <=
		       arr_Players[a_int_right].x + SQ_WIDTH + int_tolerance &&
	       arr_Players[a_int_left].x + SQ_WIDTH + int_tolerance >=
		       arr_Players[a_int_right].x &&
	       arr_Players[a_int_left].y <=
		       arr_Players[a_int_right].y + SQ_WIDTH + int_tolerance &&
	       arr_Players[a_int_left].y + SQ_WIDTH + int_tolerance >=
		       arr_Players[a_int_right].y;
}

static void add_explosion(int a_int_x, int a_int_y)
{
	Explosion explosion = {
		.tick = u_tick,
		.x = (int16_t)(a_int_x + SQ_WIDTH / 2),
		.y = (int16_t)(a_int_y + SQ_WIDTH / 2),
		.seed = (uint16_t)rand()
	};
	for (int i = 3; i > 0; i--)
		arr_explosions[i] = arr_explosions[i - 1];
	arr_explosions[0] = explosion;
}

static void spawn_pickup(int a_int_x, int a_int_y)
{
	for (int i = 0; i < MAX_PICKUPS; i++)
	{
		if (arr_pickups[i].type == PICKUP_NONE)
		{
			static const uint8_t arr_types[] = {
				PICKUP_SPEED, PICKUP_SHIELD, PICKUP_FUSE_RESET
			};
			arr_pickups[i].type = arr_types[rand() % 3];
			arr_pickups[i].x = (int16_t)clamp_coord(
				a_int_x + (rand() % 151) - 75, WIDTH - SQ_WIDTH);
			arr_pickups[i].y = (int16_t)clamp_coord(
				a_int_y + (rand() % 151) - 75, HEIGHT - SQ_WIDTH);
			return;
		}
	}
}

static void update_player_effects(int a_int_i)
{
	if (arr_u16_dash_cooldown[a_int_i] > 0)
		arr_u16_dash_cooldown[a_int_i]--;
	if (arr_u16_dash_ticks[a_int_i] > 0)
		arr_u16_dash_ticks[a_int_i]--;
	if (arr_u16_speed_ticks[a_int_i] > 0)
		arr_u16_speed_ticks[a_int_i]--;

	bool b_dash_down = (arr_u8_keys[a_int_i] & KEY_DASH) != 0;
	if (b_dash_down && !arr_b_dash_was_down[a_int_i] &&
	    arr_u16_dash_cooldown[a_int_i] == 0)
	{
		arr_u16_dash_ticks[a_int_i] = DASH_TICKS;
		arr_u16_dash_cooldown[a_int_i] = DASH_COOLDOWN_TICKS;
	}
	arr_b_dash_was_down[a_int_i] = b_dash_down;

	for (int i = 0; i < MAX_PICKUPS; i++)
	{
		if (arr_pickups[i].type == PICKUP_NONE)
			continue;
		if (arr_Players[a_int_i].x < arr_pickups[i].x + SQ_WIDTH &&
		    arr_Players[a_int_i].x + SQ_WIDTH > arr_pickups[i].x &&
		    arr_Players[a_int_i].y < arr_pickups[i].y + SQ_WIDTH &&
		    arr_Players[a_int_i].y + SQ_WIDTH > arr_pickups[i].y)
		{
			if (arr_pickups[i].type == PICKUP_SPEED)
				arr_u16_speed_ticks[a_int_i] = SPEED_PICKUP_TICKS;
			else if (arr_pickups[i].type == PICKUP_SHIELD)
				arr_u8_shield[a_int_i] = 1;
			else if (arr_pickups[i].type == PICKUP_FUSE_RESET &&
			         u8_phase == PHASE_PLAYING)
				u_fuse_ticks += 3 * TICK_HZ;
			arr_pickups[i].type = PICKUP_NONE;
		}
	}
}

static void pass_bomb_if_touched(void)
{
	if (u8_phase != PHASE_PLAYING || u8_bomb_holder == 0)
		return;
	if (u_pass_cooldown_ticks > 0)
		u_pass_cooldown_ticks--;

	int int_holder = u8_bomb_holder - 1;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (i == int_holder || !arr_Players[i].b_active ||
		    arr_u8_lives[i] == 0 || (uint8_t)(i + 1) == u8_previous_holder)
			continue;
		if (u_pass_cooldown_ticks != 0 || !players_touch(int_holder, i))
			continue;
		if (arr_u8_shield[i] != 0)
		{
			arr_u8_shield[i] = 0;
			continue;
		}
		u8_previous_holder = u8_bomb_holder;
		u8_bomb_holder = (uint8_t)(i + 1);
		u_pass_cooldown_ticks = PASS_COOLDOWN_TICKS;
		LOG_INFO("Bomb passed: player %d -> player %d\n",
		         int_holder + 1, i + 1);
		break;
	}
}

static void update_round_state(void)
{
	int int_active = count_active_players();

	if (int_active < 2)
	{
		u8_phase = PHASE_LOBBY;
		u8_bomb_holder = 0;
		u8_previous_holder = 0;
		u_pass_cooldown_ticks = 0;
		u_countdown_ticks = 0;
		u_fuse_ticks = 0;
		u_round_over_ticks = 0;
		clear_round_transients();
		return;
	}

	switch (u8_phase)
	{
		case PHASE_LOBBY:
			u8_phase = PHASE_COUNTDOWN;
			u_countdown_ticks = COUNTDOWN_TICKS;
			u8_bomb_holder = 0;
			clear_round_transients();
			break;

		case PHASE_COUNTDOWN:
			if (u_countdown_ticks > 0)
				u_countdown_ticks--;
			if (u_countdown_ticks == 0)
			{
				u8_phase = PHASE_PLAYING;
				u8_bomb_holder = choose_bomb_holder();
				u_fuse_ticks = FUSE_TICKS;
				u8_previous_holder = 0;
				u_pass_cooldown_ticks = 0;
			}
			break;

		case PHASE_PLAYING:
			if (u8_bomb_holder == 0 ||
			    !arr_Players[u8_bomb_holder - 1].b_active ||
			    arr_u8_lives[u8_bomb_holder - 1] == 0)
				u8_bomb_holder = choose_bomb_holder();

			if (u_fuse_ticks > 0)
				u_fuse_ticks--;
			if (u_fuse_ticks == 0 && u8_bomb_holder != 0)
			{
				int int_holder = u8_bomb_holder - 1;
				int int_x = arr_Players[int_holder].x;
				int int_y = arr_Players[int_holder].y;
				if (arr_u8_lives[int_holder] > 0)
					arr_u8_lives[int_holder]--;
				add_explosion(int_x, int_y);
				spawn_pickup(int_x, int_y);
				u8_bomb_holder = 0;
				if (count_alive_players() <= 1)
					u8_phase = PHASE_GAME_OVER;
				else
				{
					u8_phase = PHASE_ROUND_OVER;
					u_round_over_ticks = ROUND_OVER_TICKS;
				}
			}
			break;

		case PHASE_ROUND_OVER:
			if (u_round_over_ticks > 0)
				u_round_over_ticks--;
			if (u_round_over_ticks == 0)
			{
				u8_phase = PHASE_COUNTDOWN;
				u_countdown_ticks = COUNTDOWN_TICKS;
				clear_round_transients();
			}
			break;

		case PHASE_GAME_OVER:
			break;
	}
}

/* Move player index a_int_i one tick by its held keys. Each axis is tried on
its own, so a player blocked sideways by another player can still move up or
down. A move into another player is cancelled (as in the baseline); a move
past the window edge stops at the edge. Fills the player's velocity in
*a_p_state. The caller holds the mutex. */
static void move_player(int a_int_i, PlayerState *a_p_state)
{
	uint8_t u8_keys = arr_u8_keys[a_int_i];
	int int_speed = TICK_STEP;
	if (arr_u16_speed_ticks[a_int_i] > 0)
		int_speed *= 2;
	if (arr_u16_dash_ticks[a_int_i] > 0)
		int_speed *= 2;
	// -1, 0 or +1 per axis; opposite keys cancel out
	int int_dx = ((u8_keys & KEY_RIGHT) != 0) - ((u8_keys & KEY_LEFT) != 0);
	int int_dy = ((u8_keys & KEY_DOWN) != 0) - ((u8_keys & KEY_UP) != 0);

	int int_x = arr_Players[a_int_i].x;
	int int_y = arr_Players[a_int_i].y;

	if (int_dx != 0)
	{
		int int_new_x = clamp_coord(int_x + int_dx * int_speed, WIDTH - SQ_WIDTH);
		if (!collision_detect(arr_Players, MAX_PLAYERS, a_int_i + 1, int_new_x, int_y))
			int_x = int_new_x;
	}
	if (int_dy != 0)
	{
		int int_new_y = clamp_coord(int_y + int_dy * int_speed, HEIGHT - SQ_WIDTH);
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
	update_round_state();
	static uint8_t u8_previous_phase = PHASE_LOBBY;
	if (u8_phase != u8_previous_phase)
	{
		LOG_INFO("Game phase changed: %u -> %u\n",
		         (unsigned)u8_previous_phase, (unsigned)u8_phase);
		u8_previous_phase = u8_phase;
	}
	snapshot.phase = u8_phase;
	snapshot.bomb_holder = u8_bomb_holder;
	if (u8_phase == PHASE_COUNTDOWN)
		snapshot.timer_ms = ticks_to_ms(u_countdown_ticks);
	else if (u8_phase == PHASE_PLAYING)
		snapshot.timer_ms = ticks_to_ms(u_fuse_ticks);
	else if (u8_phase == PHASE_ROUND_OVER)
		snapshot.timer_ms = ticks_to_ms(u_round_over_ticks);
	memcpy(snapshot.pickups, arr_pickups, sizeof arr_pickups);
	memcpy(snapshot.explosions, arr_explosions, sizeof arr_explosions);
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_Players[i].b_active)
			continue;   // inactive slot: flags 0, the rest unused
		PlayerState *p_state = &snapshot.players[i];
		if (arr_u8_lives[i] > 0)
			move_player(i, p_state);
		update_player_effects(i);
		p_state->flags = PF_ACTIVE;
		if (arr_u8_lives[i] > 0)
			p_state->flags |= PF_ALIVE;
		if (arr_u8_shield[i] != 0)
			p_state->flags |= PF_SHIELD;
		if (arr_u16_speed_ticks[i] > 0)
			p_state->flags |= PF_SPEED;
		if (arr_u16_dash_ticks[i] > 0)
			p_state->flags |= PF_DASHING;
		p_state->lives = arr_u8_lives[i];
		p_state->x = (int16_t)arr_Players[i].x;
		p_state->y = (int16_t)arr_Players[i].y;
	}
	pass_bomb_if_touched();

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
