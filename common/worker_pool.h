/** @file common/worker_pool.h
 * @brief Fixed pool of worker threads that run one job together and wait for
 * each other (optimisation #5, thread-level parallelism).
 * @details The threads are created once and sleep between jobs, so a job costs two
 * barrier waits instead of creating and joining threads every tick. The caller
 * is worker 0 and does a share of the work itself, so a pool of N threads
 * creates only N - 1 extra threads, and a pool of 1 runs jobs directly with no
 * synchronisation at all (the single-thread "before").
 *
 * A pool is driven from one thread only (the one that called pool_init()).
 */

#ifndef COMMON_WORKER_POOL_H
#define COMMON_WORKER_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include "cache_padding.h"

/** @brief Upper limit on threads per pool (the test laptop has 24 hardware threads). */
#define POOL_MAX_THREADS 32

/** @brief One worker's per-tick result (live particles left in its chunk).
 * @details Each particle worker writes one result per tick. PADDING=1 (#11)
 * gives each element its own cache line; PADDING=0 keeps adjacent ints for
 * comparison.
 */
typedef struct
{
    FUSE_CACHE_ALIGN int int_live;  ///< Live items at the start of the chunk
} PoolChunkResult;

/** @brief A job, called once on every worker.
 * @param a_arg Argument given to pool_run(), shared by all workers.
 * @param a_int_worker This worker's index (0 = the calling thread).
 * @param a_int_workers Number of workers, so each one can pick its own share
 *        of the data (see pool_chunk()).
 */
typedef void (*PoolJob)(void *a_arg, int a_int_worker, int a_int_workers);

typedef struct WorkerPool WorkerPool;

/** @brief What a pool thread is started with (its index is fixed for its whole life). */
typedef struct
{
    WorkerPool *p_pool;     ///< Owning pool
    int int_worker;         ///< Worker index, 1..int_threads - 1
} WorkerStart;

/** @brief A fixed set of threads that run each job together. */
struct WorkerPool
{
    int int_threads;            ///< Workers including the caller
    pthread_t arr_threads[POOL_MAX_THREADS];    ///< Threads 1..int_threads - 1
    WorkerStart arr_starts[POOL_MAX_THREADS];   ///< Start arguments of those threads
    /** @brief Everyone waits here until the caller has posted a job.
     * @details A barrier wait is also a full memory barrier, so the job's
     * data written before "start" and results written before "done" are
     * visible. */
    pthread_barrier_t barrier_start;
    /** @brief The caller waits here until every worker has finished the job. */
    pthread_barrier_t barrier_done;
    PoolJob job;                ///< Current job; NULL tells workers to exit
    void *p_job_arg;            ///< Argument of the current job
};

/** @brief Start the pool's extra threads.
 * @param a_pool Pool to initialise.
 * @param a_int_threads Workers including the caller, clamped to
 *        1..POOL_MAX_THREADS; a_int_threads - 1 threads are created (none for 1).
 * @return False if a thread could not be created.
 */
bool pool_init(WorkerPool *a_pool, int a_int_threads);

/** @brief Run a job on every worker and return when all of them have finished.
 * @details The caller runs worker 0's share itself. With one thread the job
 * is called directly, with no synchronisation.
 * @param a_pool Initialised pool.
 * @param a_job Job to run.
 * @param a_arg Argument passed to every call of the job.
 */
void pool_run(WorkerPool *a_pool, PoolJob a_job, void *a_arg);

/** @brief Stop and join the threads, and destroy the barriers.
 * @param a_pool Pool from pool_init(); left as a 1-thread pool.
 */
void pool_free(WorkerPool *a_pool);

/** @brief One worker's share of a_int_n items, aligned for no false sharing.
 * @details Splits the items into a_int_workers ranges whose starts are
 * multiples of a_int_align items (e.g. 16 floats = one 64-byte cache line, so
 * two workers never write the same line: no false sharing at the edges).
 * @param a_int_n Number of items.
 * @param a_int_worker Worker index.
 * @param a_int_workers Number of workers.
 * @param a_int_align Alignment unit in items.
 * @param[out] a_p_begin First item of the share.
 * @param[out] a_p_end One past the last item; the share may be empty.
 */
void pool_chunk(int a_int_n, int a_int_worker, int a_int_workers, int a_int_align,
                int *a_p_begin, int *a_p_end);

#endif /* COMMON_WORKER_POOL_H */
