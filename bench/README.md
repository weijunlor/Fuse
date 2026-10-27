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

Use one label per optimisation step (matching the git tag) so the final report can compare every step.
Run the setup in `docs/PLAN.md` section 7 first (perf_event_paranoid, kptr_restrict, governor); the script checks it.
