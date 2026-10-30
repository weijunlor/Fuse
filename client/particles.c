/* client/particles.c
Particle system in two layouts, "aos" (before) and "soa" (after #5, #6, #7);
see particles.h for what each one does and why.

Both layouts run exactly the same arithmetic on each particle in the same
order of operations, and draw the same random numbers in the same order, so
after the same ticks they hold the same particles (only their order in
memory differs). particles_checksum() checks this. */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>   // AVX2 + FMA intrinsics for soa_move_avx2()
#endif

#include "particles.h"
#include "atlas.h"
#include "config.h"
#include "worker_pool.h"

/* ----------------------------------------------------- */
/* Tuning (same for both layouts)                        */
/* ----------------------------------------------------- */

// Velocity is multiplied by this every second (strong drag: sparks shoot out
// and stop, as seen from above)
#define DRAG_PER_S 0.2f
// Spawn speed (pixels/s) and lifetime (s) ranges
#define SPEED_MIN 60.0f
#define SPEED_MAX 300.0f
#define LIFE_MIN 0.5f
#define LIFE_MAX 2.0f
// Particles per burst in particles_fill()
#define FILL_BURST 64
// Directions are taken from a table instead of calling cosf/sinf per spawn
// (spawning is serial, so it must stay cheap for the threaded update to scale)
#define DIRECTIONS 64
// Particles are drawn as atlas tiles; keep the whole tile inside the window
#define MAX_X ((float)(WIDTH - ATLAS_TILE))
#define MAX_Y ((float)(HEIGHT - ATLAS_TILE))
// Floats per 64-byte cache line: SoA arrays and thread chunks start on a
// cache line, so two threads never write the same line (no false sharing)
#define LINE_FLOATS 16

/* ----------------------------------------------------- */
/* Shared state                                          */
/* ----------------------------------------------------- */

static ParticleLayout layout;
static int int_capacity;
static int int_count;                   // live particles
static unsigned int u_rng;              // xorshift32 state
static float arr_dir_x[DIRECTIONS], arr_dir_y[DIRECTIONS];

/* ----------------------------------------------------- */
/* "aos" layout: the first, simple version (before)      */
/* ----------------------------------------------------- */

// One particle, 32 bytes, so two per cache line. The update reads and writes
// x..life and reads b_alive; life_max and sprite are only used when spawning
// and drawing, but are loaded into cache with the rest anyway.
typedef struct
{
    float x, y;         // top-left corner, pixels
    float vx, vy;       // pixels/s
    float life;         // seconds left
    float life_max;     // seconds at spawn (animation frame)
    int int_sprite;     // first animation frame (SPR_SPARK or SPR_EMBER)
    bool b_alive;
} ParticleAoS;

static ParticleAoS *arr_aos;
static int int_aos_cursor;  // where the search for a free slot starts

/* ----------------------------------------------------- */
/* "soa" layout (after)                                  */
/* ----------------------------------------------------- */

// Hot arrays: read and written by every update
static float *arr_x, *arr_y, *arr_vx, *arr_vy, *arr_life;
// Cold arrays: only touched when spawning, drawing and removing
static float *arr_life_max;
static int *arr_sprite;

// Live counts left at the start of each worker's chunk. The build-time #11
// option controls result padding; SIMD array/chunk alignment stays unchanged.
static PoolChunkResult arr_chunk[POOL_MAX_THREADS];
static WorkerPool pool;

// Arguments of one update, shared by all workers (read only)
typedef struct
{
    float dt;
    float drag;
} UpdateArgs;

/* ----------------------------------------------------- */
/* Helpers                                               */
/* ----------------------------------------------------- */

static const char *const arr_layout_names[] = { "aos", "soa", "soa-intrin", "soa-intrin-scan" };

int particles_layout_from_name(const char *a_str_name)
{
    for (int i = 0; i < (int)(sizeof(arr_layout_names) / sizeof(arr_layout_names[0])); i++)
    {
        if (strcmp(a_str_name, arr_layout_names[i]) == 0)
            return i;
    }
    return -1;
}

const char *particles_layout_name(ParticleLayout a_layout)
{
    return arr_layout_names[a_layout];
}

// xorshift32: fast, and the same sequence on every machine
static unsigned int rng_next(void)
{
    u_rng ^= u_rng << 13;
    u_rng ^= u_rng >> 17;
    u_rng ^= u_rng << 5;
    return u_rng;
}

// Random float in [a_lo, a_hi), from the top 24 bits
static float rng_range(float a_lo, float a_hi)
{
    return a_lo + (a_hi - a_lo) * (float)(rng_next() >> 8) * (1.0f / 16777216.0f);
}

// Cache-line-aligned array of a_int_n 4-byte items, size rounded up to whole lines
static void *alloc_lines(int a_int_n)
{
    size_t u_bytes = ((size_t)a_int_n + LINE_FLOATS - 1) / LINE_FLOATS * 64;
    void *p = aligned_alloc(64, u_bytes);
    if (p == NULL)
    {
        fprintf(stderr, "Out of memory for %d particles\n", a_int_n);
        exit(1);
    }
    return p;
}

/* ----------------------------------------------------- */
/* Set-up                                                */
/* ----------------------------------------------------- */

void particles_init(ParticleLayout a_layout, int a_int_capacity, int a_int_threads,
                    unsigned int a_u_seed)
{
#if !(defined(__AVX2__) && defined(__FMA__))
    if (a_layout == PARTICLES_SOA_INTRIN || a_layout == PARTICLES_SOA_INTRIN_SCAN)
    {
        fprintf(stderr, "soa-intrin and soa-intrin-scan need a build for AVX2 and FMA (e.g. -march=native)\n");
        exit(1);
    }
#endif
    layout = a_layout;
    int_capacity = a_int_capacity > 0 ? a_int_capacity : 1;
    int_count = 0;
    u_rng = a_u_seed != 0 ? a_u_seed : 1;   // xorshift must not start at 0

    for (int i = 0; i < DIRECTIONS; i++)
    {
        float f_angle = 2.0f * 3.14159265f * (float)i / DIRECTIONS;
        arr_dir_x[i] = cosf(f_angle);
        arr_dir_y[i] = sinf(f_angle);
    }

    if (layout == PARTICLES_AOS)
    {
        arr_aos = calloc((size_t)int_capacity, sizeof(ParticleAoS));
        if (arr_aos == NULL)
        {
            fprintf(stderr, "Out of memory for %d particles\n", int_capacity);
            exit(1);
        }
        int_aos_cursor = 0;
        a_int_threads = 1;      // the naive version is single-threaded
    }
    else
    {
        arr_x = alloc_lines(int_capacity);
        arr_y = alloc_lines(int_capacity);
        arr_vx = alloc_lines(int_capacity);
        arr_vy = alloc_lines(int_capacity);
        arr_life = alloc_lines(int_capacity);
        arr_life_max = alloc_lines(int_capacity);
        arr_sprite = alloc_lines(int_capacity);
    }

    if (!pool_init(&pool, a_int_threads))
    {
        fprintf(stderr, "Cannot start %d particle threads\n", a_int_threads);
        exit(1);
    }
}

void particles_free(void)
{
    pool_free(&pool);
    free(arr_aos);
    free(arr_x);
    free(arr_y);
    free(arr_vx);
    free(arr_vy);
    free(arr_life);
    free(arr_life_max);
    free(arr_sprite);
    arr_aos = NULL;
    arr_x = arr_y = arr_vx = arr_vy = arr_life = arr_life_max = NULL;
    arr_sprite = NULL;
    int_count = 0;
}

/* ----------------------------------------------------- */
/* Spawning                                              */
/* ----------------------------------------------------- */

// "aos": find a free slot by walking from the cursor (wrapping round).
// Returns -1 if every slot is in use.
static int aos_find_free(void)
{
    for (int n = 0; n < int_capacity; n++)
    {
        int i = int_aos_cursor;
        int_aos_cursor = (int_aos_cursor + 1 == int_capacity) ? 0 : int_aos_cursor + 1;
        if (!arr_aos[i].b_alive)
            return i;
    }
    return -1;
}

void particles_burst(float a_x, float a_y, int a_int_count)
{
    for (int k = 0; k < a_int_count; k++)
    {
        // Same random numbers in the same order for both layouts
        unsigned int u_dir = rng_next() % DIRECTIONS;
        float f_speed = rng_range(SPEED_MIN, SPEED_MAX);
        float f_life = rng_range(LIFE_MIN, LIFE_MAX);
        int int_sprite = (k & 1) ? SPR_EMBER : SPR_SPARK;
        float vx = arr_dir_x[u_dir] * f_speed;
        float vy = arr_dir_y[u_dir] * f_speed;

        if (layout == PARTICLES_AOS)
        {
            int i = aos_find_free();
            if (i < 0)
                return;
            arr_aos[i] = (ParticleAoS){ a_x, a_y, vx, vy, f_life, f_life, int_sprite, true };
        }
        else
        {
            if (int_count == int_capacity)
                return;
            int i = int_count;      // append after the last live particle
            arr_x[i] = a_x;
            arr_y[i] = a_y;
            arr_vx[i] = vx;
            arr_vy[i] = vy;
            arr_life[i] = f_life;
            arr_life_max[i] = f_life;
            arr_sprite[i] = int_sprite;
        }
        int_count++;
    }
}

void particles_fill(int a_int_target)
{
    if (a_int_target > int_capacity)
        a_int_target = int_capacity;
    while (int_count < a_int_target)
    {
        int int_n = a_int_target - int_count;
        float f_x = rng_range(0.0f, MAX_X);
        float f_y = rng_range(0.0f, MAX_Y);
        particles_burst(f_x, f_y, int_n < FILL_BURST ? int_n : FILL_BURST);
    }
}

/* ----------------------------------------------------- */
/* Update: "aos"                                         */
/* ----------------------------------------------------- */

// Walk every slot, skip holes, move and age live particles, free dead ones.
// The alive test is a branch that is hard to predict once the array is full
// of holes, and each particle drags its cold fields into cache with it.
static void aos_update(float a_dt, float a_drag)
{
    for (int i = 0; i < int_capacity; i++)
    {
        ParticleAoS *p = &arr_aos[i];
        if (!p->b_alive)
            continue;

        float vx = p->vx * a_drag;
        float vy = p->vy * a_drag;
        p->vx = vx;
        p->vy = vy;
        float x = p->x + vx * a_dt;
        float y = p->y + vy * a_dt;
        if (x < 0.0f)  x = 0.0f;
        if (x > MAX_X) x = MAX_X;
        if (y < 0.0f)  y = 0.0f;
        if (y > MAX_Y) y = MAX_Y;
        p->x = x;
        p->y = y;
        p->life -= a_dt;
        if (p->life <= 0.0f)
        {
            p->b_alive = false;
            int_count--;
        }
    }
}

/* ----------------------------------------------------- */
/* Update: "soa"                                         */
/* ----------------------------------------------------- */

/* The hot loop (#7). Separate float arrays, restrict (the arrays never
overlap) and no branches (the clamps are selects) let the compiler process 4
(SSE) or 8 (AVX2) particles per instruction. Same arithmetic as aos_update.
Needs -fno-trapping-math (in SIMD_FLAGS): otherwise GCC keeps the clamps as
branches, in case a comparison raises a floating-point exception flag, and
the loop stays scalar. Check with -fopt-info-vec: "loop vectorized".
noinline (also on soa_move_avx2): keeps each loop a separate function, so
perf and the disassembly can compare the two versions instruction by
instruction; one call per chunk costs nothing measurable. */
__attribute__((noinline))
static void soa_move(float *restrict a_x, float *restrict a_y,
                     float *restrict a_vx, float *restrict a_vy,
                     float *restrict a_life, int a_int_n, float a_dt, float a_drag)
{
    for (int i = 0; i < a_int_n; i++)
    {
        float vx = a_vx[i] * a_drag;
        float vy = a_vy[i] * a_drag;
        a_vx[i] = vx;
        a_vy[i] = vy;
        float x = a_x[i] + vx * a_dt;
        float y = a_y[i] + vy * a_dt;
        x = x < 0.0f ? 0.0f : x;
        x = x > MAX_X ? MAX_X : x;
        y = y < 0.0f ? 0.0f : y;
        y = y > MAX_Y ? MAX_Y : y;
        a_x[i] = x;
        a_y[i] = y;
        a_life[i] -= a_dt;
    }
}

#if defined(__AVX2__) && defined(__FMA__)
/* "soa-intrin": the same loop written by hand with AVX2 intrinsics, to
compare with what the compiler generates from soa_move. Every _mm256_*
call is one instruction working on 8 floats (one 32-byte ymm register).
Gives exactly the same results as soa_move built with -march=native:
- x + vx * dt: GCC contracts this to a fused multiply-add there, so FMA is
  used here too (_mm256_fmadd_ps)
- clamps: vmaxps/vminps return their second operand when the first is not
  larger/smaller, so max(0, x) = (0 > x ? 0 : x), the same as soa_move's
  select, including for -0.0
- aligned loads/stores (_mm256_load_ps): every chunk starts on a 64-byte
  cache line (pool_chunk, alloc_lines), so a_x etc. are 32-byte aligned
Only built when the compiler targets AVX2 and FMA (SIMD_FLAGS). */
__attribute__((noinline))
static void soa_move_avx2(float *restrict a_x, float *restrict a_y,
                          float *restrict a_vx, float *restrict a_vy,
                          float *restrict a_life, int a_int_n, float a_dt, float a_drag)
{
    // Loop constants, copied into all 8 lanes once
    const __m256 v_dt = _mm256_set1_ps(a_dt);
    const __m256 v_drag = _mm256_set1_ps(a_drag);
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_max_x = _mm256_set1_ps(MAX_X);
    const __m256 v_max_y = _mm256_set1_ps(MAX_Y);

    int i = 0;
    for (; i + 8 <= a_int_n; i += 8)     // 8 particles per iteration
    {
        __m256 vx = _mm256_mul_ps(_mm256_load_ps(a_vx + i), v_drag);
        __m256 vy = _mm256_mul_ps(_mm256_load_ps(a_vy + i), v_drag);
        _mm256_store_ps(a_vx + i, vx);
        _mm256_store_ps(a_vy + i, vy);

        __m256 x = _mm256_fmadd_ps(vx, v_dt, _mm256_load_ps(a_x + i));   // x + vx * dt
        __m256 y = _mm256_fmadd_ps(vy, v_dt, _mm256_load_ps(a_y + i));
        x = _mm256_min_ps(v_max_x, _mm256_max_ps(v_zero, x));           // clamp, no branch
        y = _mm256_min_ps(v_max_y, _mm256_max_ps(v_zero, y));
        _mm256_store_ps(a_x + i, x);
        _mm256_store_ps(a_y + i, y);

        _mm256_store_ps(a_life + i, _mm256_sub_ps(_mm256_load_ps(a_life + i), v_dt));
    }

    // Last 0-7 particles: same arithmetic, one at a time
    soa_move(a_x + i, a_y + i, a_vx + i, a_vy + i, a_life + i, a_int_n - i, a_dt, a_drag);
}
#endif

// Copy particle a_int_src over particle a_int_dst (every array)
static void soa_copy(int a_int_dst, int a_int_src)
{
    arr_x[a_int_dst] = arr_x[a_int_src];
    arr_y[a_int_dst] = arr_y[a_int_src];
    arr_vx[a_int_dst] = arr_vx[a_int_src];
    arr_vy[a_int_dst] = arr_vy[a_int_src];
    arr_life[a_int_dst] = arr_life[a_int_src];
    arr_life_max[a_int_dst] = arr_life_max[a_int_src];
    arr_sprite[a_int_dst] = arr_sprite[a_int_src];
}

/* Remove the dead particles of chunk [a_int_begin, a_int_end) inside the
chunk, by moving the chunk's last live particle into each hole. About 1 in
75 particles dies per tick, so the "if" is almost always false and well
predicted. Returns the end of the live particles. GCC cannot vectorise this
loop (its length changes as particles are removed), so after #7 it is the
slowest part of the update. noinline: shows up separately in perf. */
__attribute__((noinline))
static int soa_remove_chunk(int a_int_begin, int a_int_end)
{
    int int_last = a_int_end;   // live particles are in [a_int_begin, int_last)
    for (int i = a_int_begin; i < int_last; )
    {
        if (arr_life[i] <= 0.0f)
            soa_copy(i, --int_last);    // check slot i again: the moved one may be dead too
        else
            i++;
    }
    return int_last;
}

#if defined(__AVX2__) && defined(__FMA__)
/* "soa-intrin-scan": soa_remove_chunk with a hand-written AVX2 test that
the compiler cannot produce. One compare (vcmpps) checks 8 lives against 0
and movemask (vmovmskps) packs the 8 results into the low bits of an int.
If none is dead (about 90% of blocks: (74/75)^8), all 8 are skipped at once
instead of with 8 loads, compares and branches. A block with a dead particle
falls back to the scalar step for one particle, so the particles are
removed in exactly the same order as soa_remove_chunk. */
__attribute__((noinline))
static int soa_remove_chunk_avx2(int a_int_begin, int a_int_end)
{
    const __m256 v_zero = _mm256_setzero_ps();
    int int_last = a_int_end;
    int i = a_int_begin;
    while (i < int_last)
    {
        if (i + 8 <= int_last)
        {
            // Unaligned load: after a scalar step i is no longer a multiple of 8
            __m256 v_life = _mm256_loadu_ps(arr_life + i);
            int int_dead_bits = _mm256_movemask_ps(_mm256_cmp_ps(v_life, v_zero, _CMP_LE_OQ));
            if (int_dead_bits == 0)
            {
                i += 8;         // all 8 alive
                continue;
            }
        }
        // Same scalar step as soa_remove_chunk
        if (arr_life[i] <= 0.0f)
            soa_copy(i, --int_last);
        else
            i++;
    }
    return int_last;
}
#endif

// One worker's share of an update: move its chunk (SIMD), then remove its
// dead particles inside the chunk. The chunk was just updated by this core,
// so the removal works on data already in this core's cache.
static void soa_update_job(void *a_arg, int a_int_worker, int a_int_workers)
{
    const UpdateArgs *p_args = a_arg;
    int int_begin, int_end;
    pool_chunk(int_count, a_int_worker, a_int_workers, LINE_FLOATS, &int_begin, &int_end);
    int int_last;

#if defined(__AVX2__) && defined(__FMA__)
    if (layout == PARTICLES_SOA_INTRIN || layout == PARTICLES_SOA_INTRIN_SCAN)
        soa_move_avx2(arr_x + int_begin, arr_y + int_begin, arr_vx + int_begin, arr_vy + int_begin,
                      arr_life + int_begin, int_end - int_begin, p_args->dt, p_args->drag);
    else
#endif
    soa_move(arr_x + int_begin, arr_y + int_begin, arr_vx + int_begin, arr_vy + int_begin,
             arr_life + int_begin, int_end - int_begin, p_args->dt, p_args->drag);

#if defined(__AVX2__) && defined(__FMA__)
    if (layout == PARTICLES_SOA_INTRIN_SCAN)
        int_last = soa_remove_chunk_avx2(int_begin, int_end);
    else
#endif
    int_last = soa_remove_chunk(int_begin, int_end);

    arr_chunk[a_int_worker].int_live = int_last - int_begin;
}

/* Serial step after the workers. Each chunk now has its live particles at its
start and a hole at its end. Fill the holes below the new count with the live
particles above it, taken from the top end of the array. Every move is a
contiguous run (one memcpy per array) and only about as many particles move
as died this tick, so this stays small next to the parallel part. */
static void soa_close_gaps(int a_int_workers, int a_int_count_before)
{
    int arr_begin[POOL_MAX_THREADS];
    int int_total = 0;
    for (int w = 0; w < a_int_workers; w++)
    {
        int int_end;
        pool_chunk(a_int_count_before, w, a_int_workers, LINE_FLOATS, &arr_begin[w], &int_end);
        int_total += arr_chunk[w].int_live;
    }

    // Holes: [begin + live, next chunk's begin) for chunks in ascending order,
    // only the part below int_total. Sources: live particles at or above
    // int_total, from the highest chunk down. Both run out together.
    int w_dst = 0, w_src = a_int_workers - 1;
    int int_dst = 0, int_dst_end = 0;   // current hole
    int int_src_lo = 0, int_src = 0;    // current source run [int_src_lo, int_src)
    for (;;)
    {
        while (int_dst == int_dst_end && w_dst < a_int_workers)
        {
            int_dst = arr_begin[w_dst] + arr_chunk[w_dst].int_live;
            int_dst_end = (w_dst + 1 < a_int_workers) ? arr_begin[w_dst + 1] : a_int_count_before;
            if (int_dst_end > int_total)
                int_dst_end = int_total;
            if (int_dst > int_dst_end)
                int_dst = int_dst_end;
            w_dst++;
        }
        while (int_src == int_src_lo && w_src >= 0)
        {
            int_src = arr_begin[w_src] + arr_chunk[w_src].int_live;
            int_src_lo = arr_begin[w_src] > int_total ? arr_begin[w_src] : int_total;
            if (int_src < int_src_lo)
                int_src = int_src_lo;
            w_src--;
        }
        if (int_dst == int_dst_end || int_src == int_src_lo)
            break;

        // Move the top k particles of the source run into the bottom of the hole
        int k = int_dst_end - int_dst;
        if (int_src - int_src_lo < k)
            k = int_src - int_src_lo;
        int_src -= k;
        size_t u_bytes = (size_t)k * 4;
        memcpy(arr_x + int_dst, arr_x + int_src, u_bytes);
        memcpy(arr_y + int_dst, arr_y + int_src, u_bytes);
        memcpy(arr_vx + int_dst, arr_vx + int_src, u_bytes);
        memcpy(arr_vy + int_dst, arr_vy + int_src, u_bytes);
        memcpy(arr_life + int_dst, arr_life + int_src, u_bytes);
        memcpy(arr_life_max + int_dst, arr_life_max + int_src, u_bytes);
        memcpy(arr_sprite + int_dst, arr_sprite + int_src, u_bytes);
        int_dst += k;
    }
    int_count = int_total;
}

void particles_update(float a_dt)
{
    // Constant for the whole update: computed once, not per particle
    float f_drag = powf(DRAG_PER_S, a_dt);

    if (layout == PARTICLES_AOS)
    {
        aos_update(a_dt, f_drag);
        return;
    }

    UpdateArgs args = { a_dt, f_drag };
    int int_count_before = int_count;
    pool_run(&pool, soa_update_job, &args);
    soa_close_gaps(pool.int_threads, int_count_before);
}

/* ----------------------------------------------------- */
/* Queries                                               */
/* ----------------------------------------------------- */

// Animation: 2 frames, the second one for the last half of the life
static int frame_sprite(int a_int_sprite, float a_life, float a_life_max)
{
    return a_int_sprite + (a_life * 2.0f < a_life_max ? 1 : 0);
}

void particles_draw(void (*a_fn)(int a_int_sprite, float a_x, float a_y))
{
    if (layout == PARTICLES_AOS)
    {
        for (int i = 0; i < int_capacity; i++)
        {
            const ParticleAoS *p = &arr_aos[i];
            if (p->b_alive)
                a_fn(frame_sprite(p->int_sprite, p->life, p->life_max), p->x, p->y);
        }
    }
    else
    {
        for (int i = 0; i < int_count; i++)
            a_fn(frame_sprite(arr_sprite[i], arr_life[i], arr_life_max[i]), arr_x[i], arr_y[i]);
    }
}

int particles_count(void)
{
    return int_count;
}

int particles_capacity(void)
{
    return int_capacity;
}

int particles_threads(void)
{
    return pool.int_threads;
}

double particles_checksum(void)
{
    double d_sum = 0.0;
    if (layout == PARTICLES_AOS)
    {
        for (int i = 0; i < int_capacity; i++)
        {
            if (arr_aos[i].b_alive)
                d_sum += (double)arr_aos[i].x + 3.0 * arr_aos[i].y + 7.0 * arr_aos[i].life;
        }
    }
    else
    {
        for (int i = 0; i < int_count; i++)
            d_sum += (double)arr_x[i] + 3.0 * arr_y[i] + 7.0 * arr_life[i];
    }
    return d_sum;
}
