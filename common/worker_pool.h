/* common/worker_pool.h
Fixed pool of worker threads that run one job together and wait for each
other (optimisation #5, thread-level parallelism).

The threads are created once and sleep between jobs, so a job costs two
barrier waits instead of creating and joining threads every tick. The caller
is worker 0 and does a share of the work itself, so a pool of N threads
creates only N - 1 extra threads, and a pool of 1 runs jobs directly with no
synchronisation at all (the single-thread "before"). */

#ifndef COMMON_WORKER_POOL_H
#define COMMON_WORKER_POOL_H

#include <pthread.h>
#include <stdbool.h>

// Upper limit on threads per pool (the test laptop has 24 hardware threads)
#define POOL_MAX_THREADS 32

// A job: called once on every worker with that worker's index (0 = caller)
// and the number of workers, so each one can pick its own share of the data
typedef void (*PoolJob)(void *a_arg, int a_int_worker, int a_int_workers);

typedef struct WorkerPool WorkerPool;

// What a pool thread is started with (its index is fixed for its whole life)
typedef struct
{
    WorkerPool *p_pool;
    int int_worker;
} WorkerStart;

struct WorkerPool
{
    int int_threads;            // workers including the caller
    pthread_t arr_threads[POOL_MAX_THREADS];
    WorkerStart arr_starts[POOL_MAX_THREADS];
    // start: everyone waits here until the caller has posted a job;
    // done: the caller waits here until every worker has finished it.
    // A barrier wait is also a full memory barrier, so the job's data
    // written before "start" and results written before "done" are visible.
    pthread_barrier_t barrier_start;
    pthread_barrier_t barrier_done;
    PoolJob job;                // current job; NULL tells workers to exit
    void *p_job_arg;
};

// Start a_int_threads - 1 threads (a_int_threads is clamped to 1..POOL_MAX_THREADS).
// Returns false if a thread could not be created.
bool pool_init(WorkerPool *a_pool, int a_int_threads);

// Run a_job on every worker and return when all of them have finished
void pool_run(WorkerPool *a_pool, PoolJob a_job, void *a_arg);

// Stop and join the threads
void pool_free(WorkerPool *a_pool);

// Split a_int_n items into a_int_workers ranges whose starts are multiples
// of a_int_align items (e.g. 16 floats = one 64-byte cache line, so two
// workers never write the same line: no false sharing at the edges).
// Gives [*a_p_begin, *a_p_end) for worker a_int_worker; may be empty.
void pool_chunk(int a_int_n, int a_int_worker, int a_int_workers, int a_int_align,
                int *a_p_begin, int *a_p_end);

#endif /* COMMON_WORKER_POOL_H */
