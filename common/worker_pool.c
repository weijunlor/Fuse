/* common/worker_pool.c
Fixed pool of worker threads synchronised by two barriers per job (see
worker_pool.h). */

#include <stdlib.h>

#include "worker_pool.h"

// Body of threads 1..N-1: sleep at the start barrier, run the posted job on
// this worker's share, report at the done barrier, repeat until job == NULL
static void *worker_main(void *a_arg)
{
    WorkerStart *p_start = a_arg;
    WorkerPool *p_pool = p_start->p_pool;
    for (;;)
    {
        pthread_barrier_wait(&p_pool->barrier_start);
        PoolJob job = p_pool->job;
        if (job == NULL)
            break;
        job(p_pool->p_job_arg, p_start->int_worker, p_pool->int_threads);
        pthread_barrier_wait(&p_pool->barrier_done);
    }
    return NULL;
}

bool pool_init(WorkerPool *a_pool, int a_int_threads)
{
    if (a_int_threads < 1)
        a_int_threads = 1;
    if (a_int_threads > POOL_MAX_THREADS)
        a_int_threads = POOL_MAX_THREADS;
    a_pool->int_threads = a_int_threads;
    a_pool->job = NULL;
    a_pool->p_job_arg = NULL;
    if (a_int_threads == 1)
        return true;    // jobs run directly in pool_run(), no threads or barriers

    pthread_barrier_init(&a_pool->barrier_start, NULL, (unsigned)a_int_threads);
    pthread_barrier_init(&a_pool->barrier_done, NULL, (unsigned)a_int_threads);
    for (int i = 1; i < a_int_threads; i++)
    {
        a_pool->arr_starts[i] = (WorkerStart){ a_pool, i };
        if (pthread_create(&a_pool->arr_threads[i], NULL, worker_main, &a_pool->arr_starts[i]) != 0)
            return false;
    }
    return true;
}

void pool_run(WorkerPool *a_pool, PoolJob a_job, void *a_arg)
{
    if (a_pool->int_threads == 1)
    {
        a_job(a_arg, 0, 1);
        return;
    }
    a_pool->job = a_job;
    a_pool->p_job_arg = a_arg;
    pthread_barrier_wait(&a_pool->barrier_start);   // wake the workers
    a_job(a_arg, 0, a_pool->int_threads);           // the caller is worker 0
    pthread_barrier_wait(&a_pool->barrier_done);    // wait for the slowest
}

void pool_free(WorkerPool *a_pool)
{
    if (a_pool->int_threads == 1)
        return;
    a_pool->job = NULL;                              // NULL job = exit
    pthread_barrier_wait(&a_pool->barrier_start);
    for (int i = 1; i < a_pool->int_threads; i++)
        pthread_join(a_pool->arr_threads[i], NULL);
    pthread_barrier_destroy(&a_pool->barrier_start);
    pthread_barrier_destroy(&a_pool->barrier_done);
    a_pool->int_threads = 1;
}

void pool_chunk(int a_int_n, int a_int_worker, int a_int_workers, int a_int_align,
                int *a_p_begin, int *a_p_end)
{
    // Equal shares, each rounded up to whole alignment units
    int int_units = (a_int_n + a_int_align - 1) / a_int_align;
    int int_per = (int_units + a_int_workers - 1) / a_int_workers;
    int int_begin = a_int_worker * int_per * a_int_align;
    int int_end = int_begin + int_per * a_int_align;
    if (int_begin > a_int_n)
        int_begin = a_int_n;
    if (int_end > a_int_n)
        int_end = a_int_n;
    *a_p_begin = int_begin;
    *a_p_end = int_end;
}
