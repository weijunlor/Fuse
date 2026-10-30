/* #9: compare the actual overlap, clamp and held-key direction helpers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"
#include "movement.h"
#include "timing.h"

static uint64_t run_queries(int a_int_queries, bool a_b_branchless)
{
	uint32_t u32_state = 1234;
	uint64_t u64_sum = 0;
	const int int_max_x = WIDTH - SQ_WIDTH;
	const int int_max_y = HEIGHT - SQ_WIDTH;
	for (int i = 0; i < a_int_queries; i++)
	{
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_x = (int)(u32_state % 1000u) - 100;
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_y = (int)(u32_state % 800u) - 100;
		int int_dx, int_dy;
		movement_direction((uint8_t)(u32_state >> 16), &int_dx, &int_dy, a_b_branchless);
		int_x = movement_clamp(int_x + int_dx * 3, int_max_x, a_b_branchless);
		int_y = movement_clamp(int_y + int_dy * 3, int_max_y, a_b_branchless);
		bool b_hit = collision_overlap(int_x, int_y, SQ_WIDTH,
		                              375, 275, SQ_WIDTH, true, a_b_branchless);
		u64_sum += (unsigned int)(int_x + int_y + b_hit);
	}
	return u64_sum;
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3 ||
	    (strcmp(argv[1], "branchy") != 0 && strcmp(argv[1], "branchless") != 0))
	{
		fprintf(stderr, "Usage: %s <branchy|branchless> [queries]\n", argv[0]);
		return 1;
	}
	int int_queries = argc > 2 ? atoi(argv[2]) : 10000000;
	if (int_queries < 1) return 1;
	uint64_t u64_start = timing_now_ns();
	uint64_t u64_sum = run_queries(int_queries, strcmp(argv[1], "branchless") == 0);
	uint64_t u64_ns = timing_now_ns() - u64_start;
	printf("mode=%s queries=%d ns_per_query=%.3f checksum=%llu\n", argv[1],
	       int_queries, (double)u64_ns / int_queries, (unsigned long long)u64_sum);
	return 0;
}
