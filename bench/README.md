# Benchmarks

Each `bench_<name>.c` is a standalone program built to `build/bench_<name>` by `make bench`.
Each one implements the naive and optimised version of one technique, selected by a command-line argument,
so both can be measured on the same machine with the same build flags.

Done: bench_particles (AoS vs SoA, scalar vs SIMD, 1..N threads; see below).
Planned: bench_collision (branchy vs branchless), bench_false_share (padded vs unpadded), bench_reduce (1 vs 4 accumulators).

Results go in `results/` as CSV (`perf stat -x, -r 10 -o results/<name>.csv ...`).

## Profiling the game: `profile.sh`

`bench/profile.sh <label> <bin_dir>` runs 1 server + 4 clients from `<bin_dir>` (pinned to P-cores,
10 × 30 s `perf stat`, plus `perf lock`, `perf trace -s`, `perf record` and a `perf trace -e sendmsg,sendto`
log of every network send with its size) and writes one folder per run:

```
results/<label>/
  setup.txt    machine, OS, tool versions, build flags and optimisation level, git commit
  report.txt   how the profiling was done and a summary (per-run perf stat table, futex counts, hotspots)
  raw/         untouched perf output; raw/commands.txt lists every perf command that ran
```

```bash
make baseline-O0 baseline-O2
bench/profile.sh baseline-O0 build/baseline-O0
bench/profile.sh baseline-O2 build/baseline-O2
make profile && bench/profile.sh opt-01-unlock-before-publish build
MOVE=1 bench/profile.sh opt-12-snapshots build                       # clients move (FUSE_AUTO_MOVE=1)
MOVE=1 SERVER_ARGS="--snapshot-hz 10" bench/profile.sh opt-12-snapshots-10hz build
```

Options (environment): `RUNS`, `DURATION`, `CLIENTS`, `WARMUP`, `STEPS` (any of `stat lock trace record net`),
`MOVE=1` (moving clients), `SERVER_ARGS` (extra server options), `SOURCE_REF` (binaries built from another commit,
recorded in setup.txt). The `net` step (added 2026-10-28 for #12) reports UDP sends/s, bytes/s and bytes per send
for the server and client #1; earlier result folders do not have it.

perf stat output is saved exactly as `perf stat` prints it on screen (`raw/stat_*.txt`, one block per run,
each starting with `# started on`). Runs before 2026-10-27 16:30 (`baseline-O0`, `baseline-O2`, `split-modules`,
`opt-16a-*`, `opt-16b-*`) saved CSV instead (`raw/stat_*.csv`, `perf stat -x,`); `stat_normalize` in `setup_info.sh`
reads both.

Use one label per optimisation step (matching the git tag) so the final report can compare every step.
Run the setup in `docs/PLAN.md` section 7 first (perf_event_paranoid, kptr_restrict, governor); the script checks it.

## Rendering cost: `render.sh`

`bench/render.sh <label> [bin_dir]` measures the client's drawing code on its own (no server), for #16 and later
rendering optimisations. The client has test options for this:

| Option | Effect |
|---|---|
| `--stress N` | draw N extra animated sprites (fixed pseudo-random positions, same every run) |
| `--frames F` | vsync on, draw F frames, print one `frames= ... render_us_mean/p50/p99/max ...` line, exit |
| `--render MODE` | drawing path (#16 steps): `immediate` (one `glBegin`/`glEnd` per sprite), `float` (one `glDrawArrays`, 16-byte vertices), `short` (8-byte vertices, default) |

The script runs every mode in `MODES` (default `immediate float short`) at each N in `STRESS` (default `0 1000 10000`), 5 interleaved runs each, under
`perf stat` (from 3 s after start, so zink/Vulkan start-up is excluded), then one `perf record` per mode.
It writes the same `setup.txt` / `report.txt` / `raw/` layout as `profile.sh` (`setup.txt` also gives the GL renderer).
Both scripts write `setup.txt` with `bench/setup_info.sh`.

```bash
make profile && bench/render.sh opt-16b-short-vertices build
```

## Particle system: `particles.sh`

`bench/particles.sh <label> <step>` measures `client/particles.c` (optimisations #5, #6, #7) with `bench_particles`,
which runs the client's particle code without a window:
`bench_particles <aos|soa> [threads] [particles] [ticks] [capacity]` updates every particle each tick (dt = 1/60 s),
respawns until the count is back to `particles`, and prints `ns_per_particle`, `us_per_tick` and a `checksum`
(must be equal for every layout, build and thread count with the same arguments: the optimised code computes the same particles).

`make bench` builds it three ways from the same source, so each step of #7 is a build, not a code change:

| Binary | Flags | Vectors |
|---|---|---|
| `build/bench_particles_scalar` | `-O2 -fno-tree-vectorize` | none |
| `build/bench_particles_sse2` | `-O3 -fno-trapping-math` | SSE2, 4 floats |
| `build/bench_particles` | `-O3 -march=native -fno-trapping-math` (= `SIMD_FLAGS`, also used for the client's `particles.o`) | AVX2, 8 floats |

Steps (one results folder each; 10 interleaved runs per configuration, sizes 1000 to 1000000):

| Step | Optimisation | Compares |
|---|---|---|
| `layout` | #6 SoA + swap-remove | aos vs soa in the scalar build, 1 thread, pool full and 25% full |
| `simd` | #7 vectorisation | soa scalar vs SSE2 vs AVX2 (+ aos in the AVX2 build), and hand-written AVX2 intrinsics (`soa-intrin`: update loop; `soa-intrin-scan`: also the dead-particle scan); vectoriser report, disassembly (compiler vs hand-written loop), perf record |
| `threads` | #5 worker pool | soa AVX2 with 1, 2, 4, 6, 8 threads, one per physical P-core |

```bash
make bench
bench/particles.sh opt-06-particle-soa layout
bench/particles.sh opt-07-particle-simd simd
bench/particles.sh opt-07b-particle-avx2-intrinsics simd   # same step, with the intrinsic versions
bench/particles.sh opt-05-particle-threads threads
```

The client uses the same code: `--particles N` keeps N particles alive on screen, `--particle-layout aos|soa|soa-intrin|soa-intrin-scan`,
`--particle-threads T`.
