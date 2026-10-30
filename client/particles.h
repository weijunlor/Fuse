/* client/particles.h
Particle system (explosion sparks and embers), written twice so each
optimisation can be measured against the version before it:

- aos (before): array of structs with an alive flag. Dead particles leave
  holes; every tick walks the whole array and tests the flag; spawning
  searches for a hole. One thread.
- soa (#6, after): struct of arrays, live particles packed in [0, count).
  A dead particle is removed by moving a live one from the end into its slot
  (swap-remove), so the update loop has no holes and no alive test, and
  touches only the hot fields. Contiguous float arrays are what the compiler
  needs to vectorise the update (#7, DLP), and they split into
  cache-line-aligned chunks for the worker pool (#5, TLP).

- soa-intrin (#7 comparison): as soa, but the update loop is written by hand
  with AVX2 intrinsics instead of left to the compiler's auto-vectoriser.
- soa-intrin-scan: as soa-intrin, and the search for dead particles (a loop
  the compiler cannot vectorise) also uses AVX2: 8 lives tested per compare.

The layout is chosen at start-up (--particle-layout <name> in the client,
first argument of bench_particles), so both stay runnable. */

#ifndef CLIENT_PARTICLES_H
#define CLIENT_PARTICLES_H

// Room the client keeps for explosion particles on top of --particles N
#define PARTICLES_SPARE 1024

typedef enum
{
    PARTICLES_AOS,            // "aos": array of structs, alive flags, single thread
    PARTICLES_SOA,            // "soa": struct of arrays, packed, worker pool (default)
    PARTICLES_SOA_INTRIN,     // "soa-intrin": as soa, update loop hand-written
                              // with AVX2 intrinsics (only in AVX2 builds)
    PARTICLES_SOA_INTRIN_SCAN // "soa-intrin-scan": as soa-intrin, plus the dead
                              // particle scan hand-written (8 at a time)
} ParticleLayout;

// Layout from its name ("aos", "soa", "soa-intrin", "soa-intrin-scan"); -1 if unknown
int particles_layout_from_name(const char *a_str_name);
const char *particles_layout_name(ParticleLayout a_layout);

// Allocate room for a_int_capacity particles and start the worker pool with
// a_int_threads threads (the caller counts as one; "aos" always uses one).
// a_u_seed seeds the random numbers, so runs with the same seed spawn the
// same particles in the same order. Exits on failure.
void particles_init(ParticleLayout a_layout, int a_int_capacity, int a_int_threads,
                    unsigned int a_u_seed);
void particles_free(void);

// Remove all live particles immediately, for a new round or server session.
void particles_clear(void);

// Spawn a_int_count particles flying out of (a_x, a_y) in random directions
// (an explosion). Particles that do not fit are dropped.
void particles_burst(float a_x, float a_y, int a_int_count);

// Spawn bursts at random places until a_int_target particles are alive
// (stress test and benchmark: keeps the particle count steady)
void particles_fill(int a_int_target);

// Advance every particle by a_dt seconds: move, slow down, stay inside the
// window, age; remove the ones whose life ran out
void particles_update(float a_dt);

// Call a_fn(sprite, x, y) for every live particle (drawing)
void particles_draw(void (*a_fn)(int a_int_sprite, float a_x, float a_y));

int particles_count(void);
int particles_capacity(void);
int particles_threads(void);

// Sum of every live particle's position and life (in double, so the order of
// the particles hardly matters): two layouts given the same seed and ticks
// must agree, which checks that the optimised versions compute the same thing
double particles_checksum(void);

#endif /* CLIENT_PARTICLES_H */
