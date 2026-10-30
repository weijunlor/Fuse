/* Include the real simulation/lobby to reach timer boundaries without
 * waiting minutes. Production code contains no test-only timing shortcuts. */
#include <assert.h>
#include <stdio.h>
#include "../server/game.c"
#include "../server/lobby.c"

static void reset_test(void)
{
	memset(arr_Players, 0, sizeof arr_Players);
	memset(arr_u64_slot_nonce, 0, sizeof arr_u64_slot_nonce);
	int_num_active_players = int_num_pending = int_num_results = int_num_leaving = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		bot_set_player(i, false);
		game_reset_player(i);
	}
	reset_match();
	u_tick = 0; u8_phase = PHASE_LOBBY; b_snapshot_ready = false;
	int_snapshot_every = 1;
	int_player_timeout_s = PLAYER_TIMEOUT_S;
	b_game_branchless = b_collision_branchless = b_ember_grid = b_reduce_accum = true;
	srand(1234);
}

static JoinResult join(uint64_t a_nonce)
{
	assert(lobby_signal_join(a_nonce));
	lobby_poll();
	JoinResult result;
	assert(lobby_take_result(&result));
	return result;
}

static void tick_many(int a_ticks)
{
	for (int i = 0; i < a_ticks; i++) game_tick();
}

static GameSnapshot take_snapshot(void)
{
	GameSnapshot out;
	assert(game_take_snapshot(&out));
	return out;
}

static void setup_players(void)
{
	reset_test();
	assert(join(100).u8_player_id == 1 && join(101).u8_player_id == 2);
	arr_Players[0].x = 100; arr_Players[0].y = 100;
	arr_Players[1].x = 600; arr_Players[1].y = 400;
}

int main(void)
{
	setup_players();
	tick_many(LOBBY_WAIT_TICKS);
	assert(u8_phase == PHASE_COUNTDOWN);
	assert(join(999).u8_status == JOIN_IN_GAME && join(100).u8_player_id == 1);
	tick_many(COUNTDOWN_TICKS);
	assert(u8_phase == PHASE_PLAYING);
	game_set_keys(0, KEY_RIGHT); game_set_keys(MAX_PLAYERS + 1, KEY_RIGHT);
	game_set_keys(1, KEY_RIGHT | KEY_DASH);
	game_tick();
	assert(take_snapshot().players[0].vx == 2 * PLAYER_SPEED);
	game_set_keys(1, 0);
	u_fuse_ticks = 1; arr_u8_lives[0] = 1;
	game_tick();
	assert(u8_phase == PHASE_GAME_OVER && arr_u8_lives[0] == 0);
	arr_u64_last_input_ns[1] = 123;
	tick_many(GAME_OVER_TICKS);
	assert(game_accepts_joins() && arr_u8_lives[0] == 3 && arr_u8_lives[1] == 3);
	assert(arr_u64_last_input_ns[1] == 123 && arr_explosions[0].tick == 0);
	puts("PASS phases, join gate/retries, input bounds, dash, winner and rematch");

	setup_players();
	u8_phase = PHASE_PLAYING; u8_bomb_holder = 1; u_fuse_ticks = FUSE_TICKS;
	arr_Players[1].x = 153; arr_Players[1].y = 100;
	game_tick(); assert(take_snapshot().bomb_holder == 2);
	tick_many(PASS_COOLDOWN_TICKS - 1); assert(u8_bomb_holder == 2);
	game_tick(); assert(u8_bomb_holder == 1);
	assert(join(999).u8_status == JOIN_IN_GAME);
	assert(lobby_signal_leave(2)); lobby_poll();
	u_fuse_ticks = 1;
	game_tick(); assert(u8_phase == PHASE_GAME_OVER && arr_u8_lives[0] == 3);
	puts("PASS same-tick bomb snapshot, pass-back timing and departure win");

	setup_players();
	assert(join(102).u8_status == JOIN_OK);
	u8_phase = PHASE_PLAYING; u8_bomb_holder = 1; u_fuse_ticks = FUSE_TICKS;
	arr_u8_lives[1] = 0;
	arr_Players[1].x = 153; arr_Players[1].y = 100;
	arr_Players[2].x = 600; arr_Players[2].y = 100;
	game_set_keys(1, KEY_RIGHT); game_tick();
	assert(arr_Players[0].x == 103 && !(take_snapshot().players[1].flags & PF_ALIVE));
	reset_test(); assert(lobby_add_bots(1));
	assert(join(100).u8_player_id == 2);
	arr_u64_last_input_ns[0] = arr_u64_last_input_ns[1] = 1;
	lobby_check_timeouts(timing_now_ns());
	assert(arr_Players[0].b_active && !arr_Players[1].b_active);
	puts("PASS spectator collision and bot timeout exemption");

	reset_test();
	for (int n = 0; n <= MAX_EMBERS / 32; n++)
		collision_embers_spawn((n * 37) % WIDTH, (n * 53) % HEIGHT, (uint16_t)n);
	for (int x = 0; x <= WIDTH - SQ_WIDTH; x += 17)
		for (int y = 0; y <= HEIGHT - SQ_WIDTH; y += 19)
			assert(collision_player_hits_ember(x,y) == collision_player_hits_ember_bruteforce(x,y));
	for (int i = 0; i < EMBER_LIFETIME_TICKS; i++) collision_embers_tick();
	assert(!collision_player_hits_ember(0,0));
	for (int keys = 0; keys < 256; keys++)
	{
		int ax, ay, bx, by;
		movement_direction((uint8_t)keys, &ax, &ay, true);
		movement_direction((uint8_t)keys, &bx, &by, false);
		assert(ax == bx && ay == by);
	}
	puts("PASS saturated grid/brute equivalence, expiry and all key masks");
	return 0;
}
