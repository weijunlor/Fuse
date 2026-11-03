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
	b_ember_ring = true;
	srand(1234);
}

/* #8b: record ember hits (grid and brute force, which must agree) at
 * checkpoints of one scripted sequence: overfill past MAX_EMBERS, expiry on
 * the exact tick, a partly expired ring refilled so it wraps. The ring and
 * array layouts must give identical records. */
#define EMBER_CHECKPOINTS 7
#define EMBER_POINTS ((WIDTH - SQ_WIDTH) / 23 + 1) * ((HEIGHT - SQ_WIDTH) / 29 + 1)
static int record_ember_hits(bool *a_p_out)
{
	int int_hits = 0;
	for (int x = 0; x <= WIDTH - SQ_WIDTH; x += 23)
		for (int y = 0; y <= HEIGHT - SQ_WIDTH; y += 29)
		{
			bool b_hit = collision_player_hits_ember(x, y);
			assert(b_hit == collision_player_hits_ember_bruteforce(x, y));
			*a_p_out++ = b_hit;
			int_hits += b_hit;
		}
	return int_hits;
}

static void ember_ticks(int a_int_n)
{
	for (int i = 0; i < a_int_n; i++) collision_embers_tick();
}

static void ember_spawns(int a_int_first, int a_int_batches)
{
	for (int n = a_int_first; n < a_int_first + a_int_batches; n++)
		collision_embers_spawn((n * 37) % WIDTH, (n * 53) % HEIGHT, (uint16_t)n);
}

static void run_ember_script(bool a_b_ring, bool a_arr_out[EMBER_CHECKPOINTS][EMBER_POINTS])
{
	b_ember_ring = a_b_ring;
	collision_embers_clear();
	int arr_int_hits[EMBER_CHECKPOINTS];
	ember_spawns(0, 100);                      // 3200 embers at tick 0
	arr_int_hits[0] = record_ember_hits(a_arr_out[0]);
	ember_ticks(240);
	ember_spawns(100, 40);                     // 896 fit, the rest dropped
	arr_int_hits[1] = record_ember_hits(a_arr_out[1]);
	ember_ticks(EMBER_LIFETIME_TICKS - 240 - 1);
	arr_int_hits[2] = record_ember_hits(a_arr_out[2]);  // all still alive
	ember_ticks(1);                            // first 3200 expire now
	arr_int_hits[3] = record_ember_hits(a_arr_out[3]);
	ember_spawns(140, 100);                    // ring wraps past the end
	arr_int_hits[4] = record_ember_hits(a_arr_out[4]);
	ember_ticks(240);                          // the 896 expire
	arr_int_hits[5] = record_ember_hits(a_arr_out[5]);
	ember_ticks(EMBER_LIFETIME_TICKS);         // everything expired
	arr_int_hits[6] = record_ember_hits(a_arr_out[6]);
	assert(arr_int_hits[0] > 0 && arr_int_hits[2] > arr_int_hits[3]);
	assert(arr_int_hits[3] > 0 && arr_int_hits[6] == 0);
}

static JoinResult join(uint64_t a_nonce)
{
	JoinRequest request = { .nonce = a_nonce };
	assert(lobby_signal_join(&request));
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
	assert(join(100).player_id == 1 && join(101).player_id == 2);
	arr_Players[0].x = 100; arr_Players[0].y = 100;
	arr_Players[1].x = 600; arr_Players[1].y = 400;
}

static void test_random_bomb_holder(void)
{
	// Cover every set of eligible IDs, including gaps, eliminated players,
	// inactive slots, and the zero/one-player boundaries. The seed is fixed
	// so checking that all eligible players can be picked is repeatable.
	for (unsigned int u_mask = 0; u_mask < (1u << MAX_PLAYERS); u_mask++)
	{
		reset_test();
		bool arr_seen[MAX_PLAYERS] = {false};
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			bool b_eligible = (u_mask & (1u << i)) != 0;
			arr_Players[i].b_active = b_eligible || i % 2 == 0;
			arr_u8_lives[i] = b_eligible || !arr_Players[i].b_active ? 3 : 0;
		}
		for (int n = 0; n < 512; n++)
		{
			uint8_t u8_holder = choose_bomb_holder();
			if (u_mask == 0) { assert(u8_holder == 0); continue; }
			assert(u8_holder >= 1 && u8_holder <= MAX_PLAYERS);
			assert((u_mask & (1u << (u8_holder - 1))) != 0);
			arr_seen[u8_holder - 1] = true;
		}
		for (int i = 0; i < MAX_PLAYERS; i++)
			assert(arr_seen[i] == ((u_mask & (1u << i)) != 0));
	}
	puts("PASS random bomb holder across all eligible player sets (0-5 players)");
}

int main(void)
{
	// FUSE_HANDOFF=spsc (default) or mutex: make test runs both. Login is off
	// (auth_init() not called), so joins take the nonce-only path.
	assert(handoff_init());
	test_random_bomb_holder();
	setup_players();
	tick_many(LOBBY_WAIT_TICKS);
	assert(u8_phase == PHASE_COUNTDOWN);
	assert(join(999).status == JOIN_IN_GAME && join(100).player_id == 1);
	tick_many(COUNTDOWN_TICKS);
	assert(u8_phase == PHASE_PLAYING);
	game_set_keys(0, KEY_RIGHT); game_set_keys(MAX_PLAYERS + 1, KEY_RIGHT);
	game_set_keys(1, KEY_RIGHT | KEY_DASH);
	game_tick();
	assert(take_snapshot().players[0].vx == 2 * PLAYER_SPEED);
	game_set_keys(1, 0);
	// Force player 1 only for this elimination scenario; round starts above
	// now choose a random eligible holder instead of always choosing player 1.
	u8_bomb_holder = 1; u_fuse_ticks = 1; arr_u8_lives[0] = 1;
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
	assert(join(999).status == JOIN_IN_GAME);
	assert(lobby_submit_leave(&(Leave){ .player_id = 2 })); lobby_poll();
	u_fuse_ticks = 1;
	game_tick(); assert(u8_phase == PHASE_GAME_OVER && arr_u8_lives[0] == 3);
	puts("PASS same-tick bomb snapshot, pass-back timing and departure win");

	setup_players();
	assert(join(102).status == JOIN_OK);
	u8_phase = PHASE_PLAYING; u8_bomb_holder = 1; u_fuse_ticks = FUSE_TICKS;
	arr_u8_lives[1] = 0;
	arr_Players[1].x = 153; arr_Players[1].y = 100;
	arr_Players[2].x = 600; arr_Players[2].y = 100;
	game_set_keys(1, KEY_RIGHT); game_tick();
	assert(arr_Players[0].x == 103 && !(take_snapshot().players[1].flags & PF_ALIVE));
	reset_test(); assert(lobby_add_bots(1));
	assert(join(100).player_id == 2);
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
	static bool arr_ring_hits[EMBER_CHECKPOINTS][EMBER_POINTS];
	static bool arr_array_hits[EMBER_CHECKPOINTS][EMBER_POINTS];
	for (int branchless = 0; branchless < 2; branchless++)
	{
		b_collision_branchless = branchless;
		run_ember_script(true, arr_ring_hits);
		run_ember_script(false, arr_array_hits);
		assert(memcmp(arr_ring_hits, arr_array_hits, sizeof arr_ring_hits) == 0);
	}
	b_ember_ring = b_collision_branchless = true;
	collision_embers_clear();
	puts("PASS ember ring/SoA and array layouts agree (overfill, expiry tick, wrap)");
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
