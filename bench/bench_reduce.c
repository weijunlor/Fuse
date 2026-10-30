/* bench_reduce.c
Compare a dependent reduction with multiple independent accumulators (#10). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timing.h"

#define SEED 0x9e3779b9u

static unsigned int reduce_serial(const unsigned int *a_arr_values,
                                  int a_int_count, int a_int_rounds)
{
	unsigned int u32_sum = 0;
	for (int r = 0; r < a_int_rounds; r++)
		for (int i = 0; i < a_int_count; i++)
			u32_sum += a_arr_values[i] + (unsigned int)r;
	return u32_sum;
}

static unsigned int reduce_accumulators(const unsigned int *a_arr_values,
                                        int a_int_count, int a_int_rounds)
{
	unsigned int u32_sum_0 = 0;
	unsigned int u32_sum_1 = 0;
	unsigned int u32_sum_2 = 0;
	unsigned int u32_sum_3 = 0;
	// Hoist the rounded loop bound: it is independent of every round (#10).
	const int int_end = a_int_count - a_int_count % 4;
	for (int r = 0; r < a_int_rounds; r++)
	{
		int i = 0;
		for (; i < int_end; i += 4)
		{
			u32_sum_0 += a_arr_values[i] + (unsigned int)r;
			u32_sum_1 += a_arr_values[i + 1] + (unsigned int)r;
			u32_sum_2 += a_arr_values[i + 2] + (unsigned int)r;
			u32_sum_3 += a_arr_values[i + 3] + (unsigned int)r;
		}
		for (; i < a_int_count; i++)
			u32_sum_0 += a_arr_values[i] + (unsigned int)r;
	}
	return u32_sum_0 + u32_sum_1 + u32_sum_2 + u32_sum_3;
}

int main(int argc, char **argv)
{
	if (argc < 2 ||
	    (strcmp(argv[1], "serial") != 0 && strcmp(argv[1], "accum") != 0))
	{
		fprintf(stderr, "Usage: %s <serial|accum> [values] [rounds]\n", argv[0]);
		return 1;
	}
	int int_count = argc > 2 ? atoi(argv[2]) : 1000000;
	int int_rounds = argc > 3 ? atoi(argv[3]) : 100;
	if (int_count < 4 || int_rounds < 1)
	{
		fprintf(stderr, "values must be >= 4 and rounds must be positive\n");
		return 1;
	}

	unsigned int *arr_values = malloc((size_t)int_count * sizeof *arr_values);
	if (arr_values == NULL)
	{
		perror("malloc");
		return 1;
	}
	unsigned int u32_state = SEED;
	for (int i = 0; i < int_count; i++)
	{
		u32_state = u32_state * 1664525u + 1013904223u;
		arr_values[i] = u32_state;
	}

	uint64_t u64_start = timing_now_ns();
	unsigned int u32_sum = strcmp(argv[1], "accum") == 0
		? reduce_accumulators(arr_values, int_count, int_rounds)
		: reduce_serial(arr_values, int_count, int_rounds);
	uint64_t u64_ns = timing_now_ns() - u64_start;
	printf("mode=%s values=%d rounds=%d ns_per_value=%.3f checksum=%u\n",
	       argv[1], int_count, int_rounds,
	       (double)u64_ns / ((double)int_count * int_rounds), u32_sum);
	free(arr_values);
	return 0;
}
