/* #11: identical two-thread workloads compiled with/without cache padding.
Usage: bench_false_share <counters|ring> [iterations [producer_cpu consumer_cpu]]
Counters are a synthetic false-sharing demonstration; ring uses the actual
game queue. Neither measures frame time. Pin workers to distinct physical
cores for native measurements. Thread creation/affinity is outside timing. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "spsc_ring.h"

typedef struct { FUSE_CACHE_ALIGN atomic_uint_fast64_t u64_value; } Counter;
SPSC_RING_DEFINE(BenchRing, uint64_t, 1024)

// Align the base in BOTH builds, so only the selected member/element spacing
// differs. In the compact build both counters definitely share one line.
static _Alignas(FUSE_CACHE_LINE_SIZE) Counter arr_counters[2];
static _Alignas(FUSE_CACHE_LINE_SIZE) BenchRing ring;
static pthread_barrier_t barrier;
static uint64_t u64_iterations;
static bool b_ring;

typedef struct { int int_index, int_cpu; uint64_t u64_checksum; } ThreadArgs;

static void check_thread(int a_error, const char *a_operation)
{
    if (a_error != 0)
    {
        fprintf(stderr, "%s: %s\n", a_operation, strerror(a_error));
        exit(EXIT_FAILURE);
    }
}

static void wait_barrier(void)
{
    int int_rc = pthread_barrier_wait(&barrier);
    if (int_rc != PTHREAD_BARRIER_SERIAL_THREAD)
        check_thread(int_rc, "pthread_barrier_wait");
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void *worker(void *a_arg)
{
    ThreadArgs *p_args = a_arg;
    if (p_args->int_cpu >= 0)
    {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(p_args->int_cpu, &cpus);
        check_thread(pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus),
                     "pthread_setaffinity_np");
    }
    wait_barrier();
    uint64_t u64_sum = 0;
    if (!b_ring)
        for (uint64_t i = 0; i < u64_iterations; i++)
            atomic_fetch_add_explicit(&arr_counters[p_args->int_index].u64_value,
                                      1, memory_order_relaxed);
    else if (p_args->int_index == 0)
        for (uint64_t i = 1; i <= u64_iterations; i++)
            while (!BenchRing_push(&ring, &i)) { }
    else
        for (uint64_t i = 1; i <= u64_iterations; i++)
        {
            uint64_t u64_value;
            while (!BenchRing_pop(&ring, &u64_value)) { }
            if (u64_value != i)
            {
                fprintf(stderr, "ring FIFO mismatch at %" PRIu64 "\n", i);
                exit(EXIT_FAILURE);
            }
            u64_sum += u64_value;
        }
    p_args->u64_checksum = u64_sum;
    wait_barrier();
    return NULL;
}

static uint64_t parse_number(const char *a_text, uint64_t a_max)
{
    char *p_end;
    errno = 0;
    unsigned long long u_value = strtoull(a_text, &p_end, 10);
    if (errno || *a_text < '0' || *a_text > '9' || *p_end || u_value > a_max)
    {
        fprintf(stderr, "Invalid numeric argument: %s\n", a_text);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)u_value;
}

int main(int argc, char **argv)
{
    if ((argc != 2 && argc != 3 && argc != 5) ||
        (strcmp(argv[1], "counters") != 0 && strcmp(argv[1], "ring") != 0))
    {
        fprintf(stderr, "Usage: %s <counters|ring> [iterations [producer_cpu consumer_cpu]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    b_ring = strcmp(argv[1], "ring") == 0;
    u64_iterations = argc >= 3 ? parse_number(argv[2], UINT64_C(1000000000)) : UINT64_C(10000000);
    ThreadArgs arr_args[2] = {{ .int_index = 0, .int_cpu = -1 },
                              { .int_index = 1, .int_cpu = -1 }};
    if (argc == 5)
    {
        arr_args[0].int_cpu = (int)parse_number(argv[3], CPU_SETSIZE - 1);
        arr_args[1].int_cpu = (int)parse_number(argv[4], CPU_SETSIZE - 1);
    }
    if (u64_iterations == 0 || (argc == 5 && arr_args[0].int_cpu == arr_args[1].int_cpu))
    {
        fprintf(stderr, "Iterations must be positive; choose two distinct CPUs\n");
        return EXIT_FAILURE;
    }
    atomic_init(&arr_counters[0].u64_value, 0);
    atomic_init(&arr_counters[1].u64_value, 0);
    BenchRing_init(&ring);
    if (!atomic_is_lock_free(&arr_counters[0].u64_value) ||
        !atomic_is_lock_free(&ring.u_head) || !atomic_is_lock_free(&ring.u_tail))
    {
        fprintf(stderr, "Benchmark requires lock-free atomic counters\n");
        return EXIT_FAILURE;
    }
    check_thread(pthread_barrier_init(&barrier, NULL, 3), "pthread_barrier_init");
    pthread_t arr_threads[2];
    for (int i = 0; i < 2; i++)
        check_thread(pthread_create(&arr_threads[i], NULL, worker, &arr_args[i]), "pthread_create");
    uint64_t u64_start = now_ns();
    wait_barrier();
    wait_barrier();
    uint64_t u64_elapsed = now_ns() - u64_start;
    for (int i = 0; i < 2; i++)
        check_thread(pthread_join(arr_threads[i], NULL), "pthread_join");
    check_thread(pthread_barrier_destroy(&barrier), "pthread_barrier_destroy");

    uint64_t u64_sum, u64_expected;
    if (b_ring)
    {
        u64_sum = arr_args[1].u64_checksum;
        u64_expected = u64_iterations * (u64_iterations + 1) / 2;
    }
    else
    {
        uint64_t u64_a = atomic_load(&arr_counters[0].u64_value);
        uint64_t u64_b = atomic_load(&arr_counters[1].u64_value);
        if (u64_a != u64_iterations || u64_b != u64_iterations)
            return EXIT_FAILURE;
        u64_sum = u64_a + u64_b;
        u64_expected = 2 * u64_iterations;
    }
    if (u64_sum != u64_expected)
        return EXIT_FAILURE;
    printf("mode=%s padding=%d cache_line=%d iterations=%" PRIu64
           " cpu0=%d cpu1=%d elapsed_ns=%" PRIu64 " ns_per_iteration=%.4f"
           " checksum=%" PRIu64 " counter_stride=%zu ring_bytes=%zu"
           " head_offset=%zu tail_offset=%zu values_offset=%zu\n",
           argv[1], FUSE_CACHE_PADDING, FUSE_CACHE_LINE_SIZE, u64_iterations,
           arr_args[0].int_cpu, arr_args[1].int_cpu, u64_elapsed,
           (double)u64_elapsed / u64_iterations, u64_sum, sizeof(Counter), sizeof(ring),
           offsetof(BenchRing, u_head), offsetof(BenchRing, u_tail), offsetof(BenchRing, arr_values));
    return EXIT_SUCCESS;
}
