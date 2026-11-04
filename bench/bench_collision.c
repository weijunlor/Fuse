/** @file bench/bench_collision.c
 * @brief Compare grid/brute ember queries and simulated tick workloads (#8).
 * @details Prints timings and hit checksums. Array/ring comparison and the
 * game workload include Weijun's later #8b extensions to this benchmark.
 */
/* bench_collision.c
Compare brute-force ember collision with the spatial-grid query (#8), and
the two ember layouts (#8b: fixed array of structs vs FIFO ring of SoA).

Modes: <grid|brute|game>[-array|-ring]. The layout defaults to array, the
layout the #8 measurements used, so "grid" and "brute" mean what they did.
- grid, brute: queries against a fixed set of embers (one tick per round).
- game: what the server does each tick: collision_embers_tick(), then one
  grid query per player, with an explosion (32 embers) every K ticks so that
  about <embers> embers are alive (K = 32 * lifetime / embers; 0 embers =
  no explosions). Reports ns per tick, including grid rebuilds. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "collision.h"
#include "timing.h"

#define SEED 1234u

/** @brief Populate a repeatable ember set in complete groups of 32.
 * @param a_int_count Requested ember count; a partial final group is omitted.
 */
static void fill_embers(int a_int_count)
{
	for (int i = 0; i < a_int_count / 32; i++)
		collision_embers_spawn((i * 37) % WIDTH, (i * 53) % HEIGHT,
		                       (uint16_t)(SEED + i));
}

// One server tick of the game workload; returns the number of hits
/** @brief Simulate ember expiry/spawn and one query per synthetic player.
 * @param a_int_t Workload tick used for positions and random seeds.
 * @param a_int_every Explosion interval in ticks; 0 disables spawning.
 * @param a_int_players Number of synthetic player queries.
 * @return Number of player queries that hit an ember this tick.
 */
static unsigned game_tick(int a_int_t, int a_int_every, int a_int_players)
{
	collision_embers_tick();
	if (a_int_every > 0 && a_int_t % a_int_every == 0)
		collision_embers_spawn((a_int_t * 37) % WIDTH, (a_int_t * 53) % HEIGHT,
		                       (uint16_t)(SEED + a_int_t));
	unsigned u_hits = 0;
	for (int p = 0; p < a_int_players; p++)
	{
		int int_x = (p * 157 + a_int_t * 3) % (WIDTH - SQ_WIDTH);
		int int_y = (p * 101 + a_int_t * 2) % (HEIGHT - SQ_WIDTH);
		u_hits += collision_player_hits_ember(int_x, int_y);
	}
	return u_hits;
}

/** @brief Run the selected collision benchmark and print timing/checksum output.
 * @param argc Number of command-line arguments.
 * @param argv Mode grid/brute/game with optional -array/-ring, then workload sizes.
 * @return 0 on completion; 1 on invalid arguments.
 */
int main(int argc, char **argv)
{
	char str_query[16] = "";
	const char *str_layout = argc > 1 ? strchr(argv[1], '-') : NULL;
	if (argc > 1)
		snprintf(str_query, sizeof str_query, "%.*s",
		         str_layout ? (int)(str_layout - argv[1]) : (int)strlen(argv[1]), argv[1]);
	bool b_layout_ok = !str_layout || strcmp(str_layout, "-array") == 0 ||
	                   strcmp(str_layout, "-ring") == 0;
	if (argc < 2 || !b_layout_ok ||
	    (strcmp(str_query, "grid") != 0 && strcmp(str_query, "brute") != 0 &&
	     strcmp(str_query, "game") != 0))
	{
		fprintf(stderr, "Usage: %s <grid|brute>[-array|-ring] [embers] [queries] [ticks]\n"
		                "       %s game[-array|-ring] [embers alive] [players] [ticks]\n",
		        argv[0], argv[0]);
		return 1;
	}
	b_ember_ring = str_layout && strcmp(str_layout, "-ring") == 0;
	bool b_game = strcmp(str_query, "game") == 0;
	int int_embers = argc > 2 ? atoi(argv[2]) : 2048;
	int int_queries = argc > 3 ? atoi(argv[3]) : (b_game ? 5 : 1000000);
	int int_ticks = argc > 4 ? atoi(argv[4]) : (b_game ? 1000000 : 100);
	if (int_embers < (b_game ? 0 : 32) || int_embers > MAX_EMBERS ||
	    int_queries < 1 || int_ticks < 1)
	{
		fprintf(stderr, "embers must be %d..%d; queries and ticks must be positive\n",
		        b_game ? 0 : 32, MAX_EMBERS);
		return 1;
	}

	collision_embers_clear();
	unsigned long long u64_hits = 0;
	uint64_t u64_ns;
	if (b_game)
	{
		int int_every = int_embers > 0 ? 32 * EMBER_LIFETIME_TICKS / int_embers : 0;
		// Warm up for one ember lifetime so the measured ticks see the
		// steady-state number of live embers
		for (int t = 0; t < EMBER_LIFETIME_TICKS; t++)
			game_tick(t, int_every, int_queries);
		uint64_t u64_start = timing_now_ns();
		for (int t = EMBER_LIFETIME_TICKS; t < EMBER_LIFETIME_TICKS + int_ticks; t++)
			u64_hits += game_tick(t, int_every, int_queries);
		u64_ns = timing_now_ns() - u64_start;
		printf("mode=%s embers=%d players=%d ticks=%d spawn_every=%d hits=%llu "
		       "ns_per_tick=%.3f checksum=%llu\n",
		       argv[1], int_embers, int_queries, int_ticks, int_every, u64_hits,
		       (double)u64_ns / int_ticks, u64_hits);
		return 0;
	}

	fill_embers(int_embers);
	bool b_grid = strcmp(str_query, "grid") == 0;
	uint64_t u64_start = timing_now_ns();
	for (int t = 0; t < int_ticks; t++)
	{
		for (int i = 0; i < int_queries; i++)
		{
			int int_x = (i * 17 + t * 11) % (WIDTH - SQ_WIDTH);
			int int_y = (i * 29 + t * 7) % (HEIGHT - SQ_WIDTH);
			u64_hits += b_grid
				? collision_player_hits_ember(int_x, int_y)
				: collision_player_hits_ember_bruteforce(int_x, int_y);
		}
		collision_embers_tick();
	}
	u64_ns = timing_now_ns() - u64_start;
	printf("mode=%s embers=%d queries=%d ticks=%d hits=%llu ns_per_query=%.3f checksum=%llu\n",
	       argv[1], int_embers, int_queries, int_ticks, u64_hits,
	       (double)u64_ns / ((double)int_queries * int_ticks), u64_hits);
	return 0;
}
