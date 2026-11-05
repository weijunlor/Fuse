/** @file client/particles.h
 * @brief Client particle system (explosion sparks and embers) in four
 * selectable layouts, so each optimisation can be measured against the
 * version before it (#5, #6, #7).
 * @details
 * - aos (before): array of structs with an alive flag. Dead particles leave
 *   holes; every tick walks the whole array and tests the flag; spawning
 *   searches for a hole. One thread.
 * - soa (#6, after): struct of arrays, live particles packed in [0, count).
 *   A dead particle is removed by moving a live one from the end into its slot
 *   (swap-remove), so the update loop has no holes and no alive test, and
 *   touches only the hot fields. Contiguous float arrays are what the compiler
 *   needs to vectorise the update (#7, DLP), and they split into
 *   cache-line-aligned chunks for the worker pool (#5, TLP).
 * - soa-intrin (#7 comparison): as soa, but the update loop is written by hand
 *   with AVX2 intrinsics instead of left to the compiler's auto-vectoriser.
 * - soa-intrin-scan: as soa-intrin, and the search for dead particles (a loop
 *   the compiler cannot vectorise) also uses AVX2: 8 lives tested per compare.
 *
 * The layout is chosen at start-up (--particle-layout <name> in the client,
 * first argument of bench_particles), so both stay runnable.
 *
 * Threading: all functions are called from one thread (the client's render
 * thread, or the benchmark's main thread). particles_update() may hand chunks
 * to the worker pool internally and returns only when every chunk is done.
 */

#ifndef CLIENT_PARTICLES_H
#define CLIENT_PARTICLES_H

/** @brief Room the client keeps for explosion particles on top of --particles N. */
#define PARTICLES_SPARE 1024

/** @brief Memory layout / update strategy of the particle pool. */
typedef enum
{
    PARTICLES_AOS,            ///< "aos": array of structs, alive flags, single thread
    PARTICLES_SOA,            ///< "soa": struct of arrays, packed, worker pool (default)
    PARTICLES_SOA_INTRIN,     ///< "soa-intrin": as soa, update loop hand-written
                              ///< with AVX2 intrinsics (only in AVX2 builds)
    PARTICLES_SOA_INTRIN_SCAN ///< "soa-intrin-scan": as soa-intrin, plus the dead
                              ///< particle scan hand-written (8 at a time)
} ParticleLayout;

/** @brief Look up a layout by its command-line name.
 * @param a_str_name "aos", "soa", "soa-intrin" or "soa-intrin-scan".
 * @return The matching ParticleLayout, or -1 if the name is unknown.
 */
int particles_layout_from_name(const char *a_str_name);

/** @brief Command-line name of a layout (inverse of particles_layout_from_name()).
 * @param a_layout Layout to name.
 * @return Static string, e.g. "soa".
 */
const char *particles_layout_name(ParticleLayout a_layout);

/** @brief Allocate the particle pool and start the worker pool.
 * @param a_layout Layout to use for the whole run.
 * @param a_int_capacity Maximum number of live particles.
 * @param a_int_threads Threads for particles_update(), the caller counting as
 *        one; "aos" always uses one.
 * @param a_u_seed Random seed: runs with the same seed spawn the same
 *        particles in the same order.
 * @note Exits the process on allocation or thread-creation failure.
 */
void particles_init(ParticleLayout a_layout, int a_int_capacity, int a_int_threads,
                    unsigned int a_u_seed);

/** @brief Stop the worker pool and free the particle arrays. */
void particles_free(void);

/** @brief Remove all live particles immediately, for a new round or server session. */
void particles_clear(void);

/** @brief Spawn an explosion: particles flying out of a point in random directions.
 * @param a_x Centre x in window pixels.
 * @param a_y Centre y in window pixels.
 * @param a_int_count Number of particles; those that do not fit are dropped.
 */
void particles_burst(float a_x, float a_y, int a_int_count);

/** @brief Spawn bursts at random places until a target number is alive.
 * @details Stress test and benchmark helper: keeps the particle count steady.
 * @param a_int_target Number of live particles wanted.
 */
void particles_fill(int a_int_target);

/** @brief Advance every particle by one time step.
 * @details Move, slow down, stay inside the window, age, and remove the
 * particles whose life ran out.
 * @param a_dt Time step in seconds.
 */
void particles_update(float a_dt);

/** @brief Call a draw callback once for every live particle.
 * @param a_fn Callback receiving the atlas sprite index and the position in
 *        window pixels.
 */
void particles_draw(void (*a_fn)(int a_int_sprite, float a_x, float a_y));

/** @brief Number of live particles. */
int particles_count(void);

/** @brief Capacity given to particles_init(). */
int particles_capacity(void);

/** @brief Number of threads used by particles_update() (including the caller). */
int particles_threads(void);

/** @brief Checksum of the live particles, to compare layouts.
 * @details Sum of every live particle's position and life, in double so the
 * order of the particles hardly matters: two layouts given the same seed and
 * ticks must agree, which checks that the optimised versions compute the same
 * thing.
 * @return The sum.
 */
double particles_checksum(void);

#endif /* CLIENT_PARTICLES_H */
