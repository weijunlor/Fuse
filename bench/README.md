# Benchmarks

Each `bench_<name>.c` is a standalone program built to `build/bench_<name>` by `make bench`.
Each one implements the naive and optimised version of one technique, selected by a command-line argument,
so both can be measured on the same machine with the same build flags.

Planned: bench_particles (AoS vs SoA, scalar vs SIMD, 1..N threads), bench_collision (branchy vs branchless),
bench_false_share (padded vs unpadded), bench_reduce (1 vs 4 accumulators).

Results go in `results/` as CSV (`perf stat -x, -r 10 -o results/<name>.csv ...`).

## Profiling the game: `profile.sh`

`bench/profile.sh <label> <bin_dir>` runs 1 server + 4 clients from `<bin_dir>` (pinned to P-cores,
10 × 30 s `perf stat`, plus `perf lock`, `perf trace -s` and `perf record`) and writes one folder per run:

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
```

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
