/* bench/bench_particles.c
Particle update benchmark (optimisations #5, #6, #7) on the client's real
particle code (client/particles.c), without a window, so perf stat sees only
the particle work.

Usage: bench_particles <aos|soa|soa-intrin|soa-intrin-scan> [threads] [particles] [ticks] [capacity]
  defaults: 1 thread, 100000 particles, 600 ticks (10 s of game time at 60 Hz),
  capacity = particles. A game sizes the pool for its busiest moment (several
  explosions at once), so most of the time it is partly empty: capacity
  4 x particles = 25% full. "aos" walks the whole capacity every tick; "soa"
  only the live particles.

Every tick: update all particles (dt = 1/60 s), then spawn bursts until the
count is back to <particles>, as a busy game would. Before timing, 120 ticks
run untimed so particle ages are spread out (steady state: ~1 in 75 dies and
is replaced every tick). Prints one key=value line, parsed by
bench/particles.sh:
  ns_per_particle   update + respawn time / (ticks x particles)
  us_per_tick       the same per tick
  checksum          particles_checksum() at the end: must match between
                    layouts, thread counts and builds (up to rounding)

Built three ways by the Makefile from the same source (see #7 in PLAN.md):
  build/bench_particles_scalar  -O2 -fno-tree-vectorize (no SIMD at all)
  build/bench_particles_sse2    -O3 -fno-trapping-math (SSE2: 4 floats per instruction)
  build/bench_particles         -O3 -march=native -fno-trapping-math (AVX2: 8 floats) */

#include <stdio.h>
#include <stdlib.h>

#include "particles.h"
#include "timing.h"

#define DT (1.0f / 60.0f)
#define WARMUP_TICKS 120
#define SEED 12345u

int main(int argc, char **argv)
{
    if (argc < 2 || particles_layout_from_name(argv[1]) < 0)
    {
        fprintf(stderr, "Usage: %s <aos|soa|soa-intrin|soa-intrin-scan> [threads] [particles] [ticks]\n", argv[0]);
        return 1;
    }
    ParticleLayout layout_sel = (ParticleLayout)particles_layout_from_name(argv[1]);
    int int_threads = argc > 2 ? atoi(argv[2]) : 1;
    int int_particles = argc > 3 ? atoi(argv[3]) : 100000;
    int int_ticks = argc > 4 ? atoi(argv[4]) : 600;
    int int_capacity = argc > 5 ? atoi(argv[5]) : int_particles;
    if (int_threads < 1 || int_particles < 1 || int_ticks < 1 || int_capacity < int_particles)
    {
        fprintf(stderr, "threads, particles and ticks must be positive, capacity >= particles\n");
        return 1;
    }

    particles_init(layout_sel, int_capacity, int_threads, SEED);
    particles_fill(int_particles);
    for (int t = 0; t < WARMUP_TICKS; t++)
    {
        particles_update(DT);
        particles_fill(int_particles);
    }

    uint64_t u_t0 = timing_now_ns();
    for (int t = 0; t < int_ticks; t++)
    {
        particles_update(DT);
        particles_fill(int_particles);
    }
    uint64_t u_ns = timing_now_ns() - u_t0;

    printf("layout=%s threads=%d particles=%d capacity=%d ticks=%d ns_per_particle=%.4f"
           " us_per_tick=%.2f checksum=%.6e\n",
           particles_layout_name(layout_sel), particles_threads(), int_particles, int_capacity,
           int_ticks,
           (double)u_ns / ((double)int_ticks * int_particles),
           (double)u_ns / int_ticks / 1000.0,
           particles_checksum());

    particles_free();
    return 0;
}
