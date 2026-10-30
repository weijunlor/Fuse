/* bench_collision.c
Compare brute-force ember collision with the spatial-grid query (#8). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "collision.h"
#include "timing.h"

#define SEED 1234u

static void fill_embers(int a_int_count)
{
	for (int i = 0; i < a_int_count / 32; i++)
		collision_embers_spawn((i * 37) % WIDTH, (i * 53) % HEIGHT,
		                       (uint16_t)(SEED + i));
}

int main(int argc, char **argv)
{
	if (argc < 2 ||
	    (strcmp(argv[1], "grid") != 0 && strcmp(argv[1], "brute") != 0))
	{
		fprintf(stderr, "Usage: %s <grid|brute> [embers] [queries] [ticks]\n",
		        argv[0]);
		return 1;
	}
	int int_embers = argc > 2 ? atoi(argv[2]) : 2048;
	int int_queries = argc > 3 ? atoi(argv[3]) : 1000000;
	int int_ticks = argc > 4 ? atoi(argv[4]) : 100;
	if (int_embers < 32 || int_embers > MAX_EMBERS ||
	    int_queries < 1 || int_ticks < 1)
	{
		fprintf(stderr, "embers must be 32..%d; queries and ticks must be positive\n",
		        MAX_EMBERS);
		return 1;
	}

	collision_embers_clear();
	fill_embers(int_embers);
	bool b_grid = strcmp(argv[1], "grid") == 0;
	unsigned long long u64_hits = 0;
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
	uint64_t u64_ns = timing_now_ns() - u64_start;
	printf("mode=%s embers=%d queries=%d ticks=%d hits=%llu ns_per_query=%.3f checksum=%llu\n",
	       argv[1], int_embers, int_queries, int_ticks, u64_hits,
	       (double)u64_ns / ((double)int_queries * int_ticks), u64_hits);
	return 0;
}
