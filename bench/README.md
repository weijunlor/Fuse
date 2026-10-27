# Benchmarks

Each `bench_<name>.c` is a standalone program built to `build/bench_<name>` by `make bench`.
Each one implements the naive and optimised version of one technique, selected by a command-line argument,
so both can be measured on the same machine with the same build flags.

Planned: bench_particles (AoS vs SoA, scalar vs SIMD, 1..N threads), bench_collision (branchy vs branchless),
bench_false_share (padded vs unpadded), bench_reduce (1 vs 4 accumulators).

Results go in `results/` as CSV (`perf stat -x, -r 10 -o results/<name>.csv ...`).
