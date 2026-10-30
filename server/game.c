/* Server-authoritative simulation. All state below is guarded by mutex.
 * Held keys drive a fixed 60 Hz tick; snapshots describe the completed tick. */
#include <string.h>
#include <stdlib.h>
#include "game.h"
#include "collision.h"
#include "movement.h"
#include "bot.h"
#include "timing.h"
#include "log.h"

Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int int_snapshot_every = TICK_HZ / SNAPSHOT_HZ;
bool b_game_branchless = true;
bool b_ember_grid = true;
bool b_reduce_accum = true;

#define TICK_STEP (PLAYER_SPEED / TICK_HZ)
#define PLAYER_START_LIVES 3
#define LOBBY_WAIT_TICKS (5 * TICK_HZ)
#define COUNTDOWN_TICKS (3 * TICK_HZ)
#define FUSE_TICKS (20 * TICK_HZ)
#define ROUND_OVER_TICKS (2 * TICK_HZ)
#define GAME_OVER_TICKS (5 * TICK_HZ)
#define PASS_COOLDOWN_TICKS (TICK_HZ / 2)
#define DASH_TICKS (TICK_HZ / 5)
#define DASH_COOLDOWN_TICKS TICK_HZ
#define SPEED_PICKUP_TICKS (5 * TICK_HZ)
#define MAX_PICKUPS 4

static uint8_t arr_u8_keys[MAX_PLAYERS];
static uint8_t arr_u8_lives[MAX_PLAYERS];
static uint8_t arr_u8_shield[MAX_PLAYERS];
static uint16_t arr_u16_speed_ticks[MAX_PLAYERS];
static uint16_t arr_u16_dash_ticks[MAX_PLAYERS];
static uint16_t arr_u16_dash_cooldown[MAX_PLAYERS];
static uint16_t arr_u16_tag_guard_ticks[MAX_PLAYERS];
static bool arr_b_dash_was_down[MAX_PLAYERS];
static uint64_t arr_u64_last_input_ns[MAX_PLAYERS];
static uint32_t u_tick;
static uint8_t u8_phase = PHASE_LOBBY;
static uint8_t u8_bomb_holder;
static uint8_t u8_previous_holder;
static uint32_t u_lobby_ticks;
static uint32_t u_countdown_ticks;
static uint32_t u_fuse_ticks;
static uint32_t u_round_over_ticks;
static uint32_t u_game_over_ticks;
static uint32_t u_pass_cooldown_ticks;
static Pickup arr_pickups[MAX_PICKUPS];
static Explosion arr_explosions[4];
static GameSnapshot snapshot;
static bool b_snapshot_ready;

bool game_accepts_joins(void)
{
	return u8_phase == PHASE_LOBBY;
}

static bool player_alive(int a_int_index)
{
	return arr_Players[a_int_index].b_active && arr_u8_lives[a_int_index] > 0;
}

// Full lives and no power-ups for every slot, and no pickups or explosions
// left on the field: the start of a new game
static void reset_for_new_game(void)
{
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		arr_u8_lives[i] = PLAYER_START_LIVES;
		arr_u8_shield[i] = 0;
		arr_u16_speed_ticks[i] = 0;
	}
	clear_round_transients();
}

void game_set_keys(int a_int_player_id, uint8_t a_u8_keys)
{
	if (a_int_player_id < 1 || a_int_player_id > MAX_PLAYERS) return;
	pthread_mutex_lock(&mutex);
	int int_index = a_int_player_id - 1;
	if (arr_Players[int_index].b_active && !bot_is_player(int_index))
	{
		arr_u8_keys[int_index] = a_u8_keys &
			(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_DASH);
		arr_u64_last_input_ns[int_index] = timing_now_ns();
	}
	pthread_mutex_unlock(&mutex);
}

void game_reset_player(int a_int_index)
{
	arr_u8_keys[a_int_index] = 0;
	arr_u8_lives[a_int_index] = PLAYER_START_LIVES;
	arr_u8_shield[a_int_index] = 0;
	arr_u16_speed_ticks[a_int_index] = 0;
	arr_u16_dash_ticks[a_int_index] = 0;
	arr_u16_dash_cooldown[a_int_index] = 0;
	arr_u16_tag_guard_ticks[a_int_index] = 0;
	arr_b_dash_was_down[a_int_index] = false;
	arr_u64_last_input_ns[a_int_index] = timing_now_ns();
}

uint64_t game_last_input_ns(int a_int_index)
{
	return arr_u64_last_input_ns[a_int_index];
}

static void clear_round_transients(void)
{
	memset(arr_pickups, 0, sizeof arr_pickups);
	memset(arr_explosions, 0, sizeof arr_explosions);
	collision_embers_clear();
}

// Reset connected slots without refreshing their heartbeat: a rematch must
// not keep a crashed client alive indefinitely. Fixed spawns never overlap.
static void reset_match(void)
{
	static const int arr_x[MAX_PLAYERS] = {100, 650, 100, 650, 375};
	static const int arr_y[MAX_PLAYERS] = {100, 100, 450, 450, 275};
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_Players[i].b_active) continue;
		uint64_t u64_last = arr_u64_last_input_ns[i];
		game_reset_player(i);
		arr_u64_last_input_ns[i] = u64_last;
		arr_Players[i].x = arr_x[i];
		arr_Players[i].y = arr_y[i];
	}
	u8_bomb_holder = 0;
	u8_previous_holder = 0;
	u_pass_cooldown_ticks = 0;
	u_lobby_ticks = 0;
	u_countdown_ticks = 0;
	u_fuse_ticks = 0;
	u_round_over_ticks = 0;
	u_game_over_ticks = 0;
	clear_round_transients();
}

// #10: independent accumulators in the SAME loop, with an odd tail.
// --reduction serial retains the dependent path for an isolated comparison.
static void count_players(int *a_p_active, int *a_p_alive)
{
	int int_active_a = 0, int_active_b = 0;
	int int_alive_a = 0, int_alive_b = 0;
	int i = 0;
	const int int_pair_end = MAX_PLAYERS - MAX_PLAYERS % 2;
	if (b_reduce_accum)
	{
		for (; i < int_pair_end; i += 2)
		{
			int_active_a += arr_Players[i].b_active;
			int_active_b += arr_Players[i + 1].b_active;
			int_alive_a += player_alive(i);
			int_alive_b += player_alive(i + 1);
		}
	}
	for (; i < MAX_PLAYERS; i++)
	{
		int_active_a += arr_Players[i].b_active;
		int_alive_a += player_alive(i);
	}
	*a_p_active = int_active_a + int_active_b;
	*a_p_alive = int_alive_a + int_alive_b;
}

static uint8_t choose_bomb_holder(void)
{
	for (int i = 0; i < MAX_PLAYERS; i++)
		if (player_alive(i)) return (uint8_t)(i + 1);
	return 0;
}

static void finish_match(void)
{
	u8_phase = PHASE_GAME_OVER;
	u_game_over_ticks = GAME_OVER_TICKS;
	u8_bomb_holder = 0;
	u8_previous_holder = 0;
	u_pass_cooldown_ticks = 0;
	u_fuse_ticks = 0;
	LOG_INFO("Match over: winner player %u (0 = draw)\n", (unsigned)choose_bomb_holder());
}

static uint16_t ticks_to_ms(uint32_t a_u_ticks)
{
	uint32_t u_ms = (a_u_ticks * 1000u) / TICK_HZ;
	return (uint16_t)(u_ms > UINT16_MAX ? UINT16_MAX : u_ms);
}

static void add_explosion(int a_int_x, int a_int_y)
{
	Explosion explosion = {
		.tick = u_tick, .x = (int16_t)(a_int_x + SQ_WIDTH / 2),
		.y = (int16_t)(a_int_y + SQ_WIDTH / 2), .seed = (uint16_t)rand()
	};
	for (int i = 3; i > 0; i--) arr_explosions[i] = arr_explosions[i - 1];
	arr_explosions[0] = explosion;
	collision_embers_spawn(explosion.x, explosion.y, explosion.seed);
}

static void spawn_pickup(int a_int_x, int a_int_y)
{
	static const uint8_t arr_types[] = {PICKUP_SPEED, PICKUP_SHIELD, PICKUP_FUSE_RESET};
	for (int i = 0; i < MAX_PICKUPS; i++)
	{
		if (arr_pickups[i].type != PICKUP_NONE) continue;
		arr_pickups[i].type = arr_types[rand() % 3];
		arr_pickups[i].x = (int16_t)movement_clamp(
			a_int_x + (rand() % 151) - 75, WIDTH - SQ_WIDTH, b_game_branchless);
		arr_pickups[i].y = (int16_t)movement_clamp(
			a_int_y + (rand() % 151) - 75, HEIGHT - SQ_WIDTH, b_game_branchless);
		return;
	}
}

static void update_player_effects(int a_int_i)
{
	if (arr_u16_dash_cooldown[a_int_i] > 0) arr_u16_dash_cooldown[a_int_i]--;
	if (arr_u16_dash_ticks[a_int_i] > 0) arr_u16_dash_ticks[a_int_i]--;
	if (arr_u16_speed_ticks[a_int_i] > 0) arr_u16_speed_ticks[a_int_i]--;
	if (arr_u16_tag_guard_ticks[a_int_i] > 0) arr_u16_tag_guard_ticks[a_int_i]--;
	bool b_dash_down = (arr_u8_keys[a_int_i] & KEY_DASH) != 0;
	if (b_dash_down && !arr_b_dash_was_down[a_int_i] && arr_u16_dash_cooldown[a_int_i] == 0)
	{
		arr_u16_dash_ticks[a_int_i] = DASH_TICKS;
		arr_u16_dash_cooldown[a_int_i] = DASH_COOLDOWN_TICKS;
	}
	arr_b_dash_was_down[a_int_i] = b_dash_down;
	if (u8_phase != PHASE_PLAYING) return;
	for (int i = 0; i < MAX_PICKUPS; i++)
	{
		if (arr_pickups[i].type == PICKUP_NONE ||
		    !collision_overlap(arr_Players[a_int_i].x, arr_Players[a_int_i].y,
		                       SQ_WIDTH, arr_pickups[i].x, arr_pickups[i].y,
		                       SQ_WIDTH, false, b_game_branchless)) continue;
		if (arr_pickups[i].type == PICKUP_SPEED) arr_u16_speed_ticks[a_int_i] = SPEED_PICKUP_TICKS;
		else if (arr_pickups[i].type == PICKUP_SHIELD) arr_u8_shield[a_int_i] = 1;
		else if (arr_pickups[i].type == PICKUP_FUSE_RESET)
			// Reset to the configured fuse; never extend the compact timer to overflow.
			u_fuse_ticks = FUSE_TICKS;
		arr_pickups[i].type = PICKUP_NONE;
	}
}

static void pass_bomb_if_touched(void)
{
	if (u8_phase != PHASE_PLAYING || u8_bomb_holder == 0) return;
	if (u_pass_cooldown_ticks > 0 && --u_pass_cooldown_ticks == 0) u8_previous_holder = 0;
	int int_holder = u8_bomb_holder - 1;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (i == int_holder || !player_alive(i) ||
		    (uint8_t)(i + 1) == u8_previous_holder || arr_u16_tag_guard_ticks[i] > 0) continue;
		// One normal step bridges gaps left by discrete collision blocking.
		// Only pass-back is delayed: a third player can receive it immediately.
		if (!collision_overlap(arr_Players[int_holder].x, arr_Players[int_holder].y,
		                       SQ_WIDTH + TICK_STEP, arr_Players[i].x, arr_Players[i].y,
		                       SQ_WIDTH + TICK_STEP, true, b_game_branchless)) continue;
		if (arr_u8_shield[i] != 0)
		{
			// The shield blocks this pass and is used up. Start the cooldown
			// too: the players are still touching on the next tick, and
			// without it the bomb would pass then, so the shield would only
			// have lasted one tick (1/60 s).
			arr_u8_shield[i] = 0;
			arr_u16_tag_guard_ticks[i] = PASS_COOLDOWN_TICKS;
			continue;
		}
		u8_previous_holder = u8_bomb_holder;
		u8_bomb_holder = (uint8_t)(i + 1);
		u_pass_cooldown_ticks = PASS_COOLDOWN_TICKS;
		LOG_INFO("Bomb passed: player %d -> player %d\n", int_holder + 1, i + 1);
		break;
	}
}

static void update_round_state(void)
{
	int int_active, int_alive;
	count_players(&int_active, &int_alive);
	if (u8_phase == PHASE_GAME_OVER)
	{
		if (u_game_over_ticks > 0) u_game_over_ticks--;
		if (u_game_over_ticks == 0)
		{
			reset_match();
			u8_phase = PHASE_LOBBY;
		}
		return;
	}
	if (u8_phase == PHASE_LOBBY)
	{
		// Leave a real admission window before each match, including rematches.
		if (int_active < 2) u_lobby_ticks = 0;
		else u_lobby_ticks++;
		if (u_lobby_ticks >= LOBBY_WAIT_TICKS)
		{
			reset_match();
			u8_phase = PHASE_COUNTDOWN;
			u_countdown_ticks = COUNTDOWN_TICKS;
		}
		return;
	}
	if (u8_phase == PHASE_COUNTDOWN && int_active < 2)
	{
		reset_match();
		u8_phase = PHASE_LOBBY;
		return;
	}
	// Survival is checked BEFORE the fuse. Disconnects must award the sole
	// survivor a win even with eliminated spectators still connected.
	if (u8_phase != PHASE_COUNTDOWN && int_alive <= 1)
	{
		finish_match();
		return;
	}
	switch (u8_phase)
	{
		case PHASE_COUNTDOWN:
			if (u_countdown_ticks > 0) u_countdown_ticks--;
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
			if (u8_bomb_holder == 0 || !player_alive(u8_bomb_holder - 1))
			{
				u8_bomb_holder = choose_bomb_holder();
				u8_previous_holder = 0;
				u_pass_cooldown_ticks = 0;
			}
			if (u_fuse_ticks > 0) u_fuse_ticks--;
			if (u_tick % (4 * TICK_HZ) == 0) spawn_pickup(rand() % WIDTH, rand() % HEIGHT);
			if (u_fuse_ticks == 0 && u8_bomb_holder != 0)
			{
				int int_holder = u8_bomb_holder - 1;
				arr_u8_lives[int_holder]--;
				add_explosion(arr_Players[int_holder].x, arr_Players[int_holder].y);
				spawn_pickup(arr_Players[int_holder].x, arr_Players[int_holder].y);
				u8_bomb_holder = 0;
				count_players(&int_active, &int_alive);
				if (int_alive <= 1) finish_match();
				else
				{
					u8_phase = PHASE_ROUND_OVER;
					u_round_over_ticks = ROUND_OVER_TICKS;
				}
			}
			break;
		case PHASE_ROUND_OVER:
			if (u_round_over_ticks > 0) u_round_over_ticks--;
			if (u_round_over_ticks == 0)
			{
				u8_phase = PHASE_COUNTDOWN;
				u_countdown_ticks = COUNTDOWN_TICKS;
			}
			break;
		default: break;
	}
}

static void move_player(int a_int_i, PlayerState *a_p_state)
{
	int int_speed = TICK_STEP;
	if (arr_u16_speed_ticks[a_int_i] > 0) int_speed *= 2;
	if (arr_u16_dash_ticks[a_int_i] > 0) int_speed *= 2;
	bool b_slow = b_ember_grid
		? collision_player_hits_ember(arr_Players[a_int_i].x, arr_Players[a_int_i].y)
		: collision_player_hits_ember_bruteforce(arr_Players[a_int_i].x, arr_Players[a_int_i].y);
	if (b_slow) int_speed = (int_speed + 1) / 2;
	int int_dx, int_dy;
	movement_direction(arr_u8_keys[a_int_i], &int_dx, &int_dy, b_game_branchless);

	// Spectators retain their connection slots, but never block movement.
	// Copy current positions so earlier moves in this tick remain visible.
	Player arr_blockers[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		arr_blockers[i] = arr_Players[i];
		arr_blockers[i].b_active = player_alive(i);
	}
	int int_x = arr_Players[a_int_i].x, int_y = arr_Players[a_int_i].y;
	if (int_dx != 0)
	{
		int int_new_x = movement_clamp(int_x + int_dx * int_speed, WIDTH - SQ_WIDTH, b_game_branchless);
		if (!collision_detect(arr_blockers, MAX_PLAYERS, a_int_i + 1, int_new_x, int_y)) int_x = int_new_x;
	}
	if (int_dy != 0)
	{
		int int_new_y = movement_clamp(int_y + int_dy * int_speed, HEIGHT - SQ_WIDTH, b_game_branchless);
		if (!collision_detect(arr_blockers, MAX_PLAYERS, a_int_i + 1, int_x, int_new_y)) int_y = int_new_y;
	}
	a_p_state->vx = (int16_t)((int_x - arr_Players[a_int_i].x) * TICK_HZ);
	a_p_state->vy = (int16_t)((int_y - arr_Players[a_int_i].y) * TICK_HZ);
	arr_Players[a_int_i].x = int_x;
	arr_Players[a_int_i].y = int_y;
}

void game_tick(void)
{
	pthread_mutex_lock(&mutex);
	u_tick++;
	collision_embers_tick();
	update_round_state();
	memset(&snapshot, 0, sizeof snapshot);
	bool arr_b_alive[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; i++) arr_b_alive[i] = player_alive(i);
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_b_alive[i] || u8_phase == PHASE_GAME_OVER || u8_phase == PHASE_ROUND_OVER) continue;
		if (bot_is_player(i))
			arr_u8_keys[i] = u8_phase == PHASE_PLAYING
				? bot_choose_keys(arr_Players, arr_b_alive, i, u8_bomb_holder, u_tick) : 0;
		update_player_effects(i);
		move_player(i, &snapshot.players[i]);
	}
	pass_bomb_if_touched();

	// Fill state AFTER movement, pickups and bomb passing to avoid stale fields.
	snapshot.tick = u_tick;
	snapshot.phase = u8_phase;
	snapshot.bomb_holder = u8_bomb_holder;
	if (u8_phase == PHASE_LOBBY && u_lobby_ticks > 0) snapshot.timer_ms = ticks_to_ms(LOBBY_WAIT_TICKS - u_lobby_ticks);
	else if (u8_phase == PHASE_COUNTDOWN) snapshot.timer_ms = ticks_to_ms(u_countdown_ticks);
	else if (u8_phase == PHASE_PLAYING) snapshot.timer_ms = ticks_to_ms(u_fuse_ticks);
	else if (u8_phase == PHASE_ROUND_OVER) snapshot.timer_ms = ticks_to_ms(u_round_over_ticks);
	else if (u8_phase == PHASE_GAME_OVER) snapshot.timer_ms = ticks_to_ms(u_game_over_ticks);
	memcpy(snapshot.pickups, arr_pickups, sizeof arr_pickups);
	memcpy(snapshot.explosions, arr_explosions, sizeof arr_explosions);
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (!arr_Players[i].b_active) continue;
		PlayerState *p = &snapshot.players[i];
		p->flags = PF_ACTIVE;
		if (player_alive(i)) p->flags |= PF_ALIVE;
		if (arr_u8_shield[i] != 0) p->flags |= PF_SHIELD;
		if (arr_u16_speed_ticks[i] > 0) p->flags |= PF_SPEED;
		if (arr_u16_dash_ticks[i] > 0) p->flags |= PF_DASHING;
		if (bot_is_player(i)) p->flags |= PF_BOT;
		p->lives = arr_u8_lives[i];
		p->x = (int16_t)arr_Players[i].x;
		p->y = (int16_t)arr_Players[i].y;
	}
	static uint8_t u8_previous_phase = PHASE_LOBBY;
	if (u8_phase != u8_previous_phase)
	{
		LOG_INFO("Game phase changed: %u -> %u\n", u8_previous_phase, u8_phase);
		u8_previous_phase = u8_phase;
	}
	if (u_tick % (uint32_t)int_snapshot_every == 0) b_snapshot_ready = true;
	pthread_mutex_unlock(&mutex);
}

bool game_take_snapshot(GameSnapshot *a_p_out)
{
	if (!b_snapshot_ready) return false;
	*a_p_out = snapshot;
	b_snapshot_ready = false;
	return true;
}
