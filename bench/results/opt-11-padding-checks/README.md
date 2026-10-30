# Compile-time padding #11: correctness evidence

Checked 30 October 2026 on this PC's Ubuntu 22.04 WSL, GCC 11.4.0,
Cyclone DDS 0.10.5 and OpenSSL 3.0.2. The game was left built with `PADDING=1`.
Both layouts use identical handoff algorithms and atomic memory orders.

| Check | `PADDING=0` | `PADDING=1` |
|---|---|---|
| Primitive FIFO/wrap/full/empty and pinned/coherent buffer tests | Pass | Pass |
| Concurrent FIFO transfers / snapshot publications | 250,000 / 250,000 | 250,000 / 250,000 |
| Primitive ThreadSanitizer, no suppressions | Pass | Pass |
| Primitive ASan/UBSan | Pass | Pass |
| Authentication/lifecycle assertions, SPSC / mutex | 8,773 / 8,387 | 8,773 / 8,387 |
| Authentication/lifecycle ASan/UBSan, both handoffs | Pass | Pass |
| Real-DDS acceptance checks, SPSC handoff | 19 / 19 | 19 / 19 |
| Particle checksum, 10,003 particles, 60 ticks, 1 and 4 workers | `1.234420e+07` | `1.234420e+07` |
| No recompilation when repeating the same setting | Pass | Pass |

`primitives-*.log`, `tsan-primitives-*.log` and `asan-primitives-*.log` cover
the standalone queues/buffers. TSan uses this WSL's per-process `setarch -R`
workaround. These passes do not change the previously documented DDS
whole-process TSan limitation in `docs/CONCURRENCY.md`.

`auth-*` and `asan-auth-*` cover lifecycle, overflow/backpressure, delayed
old-session samples, input validation and snapshot rates. `dds-0/` and `dds-1/`
contain the real worker/server acceptance logs and binary hashes: secure and
plaintext DDS, authenticated movement/Leave, tampering/replay rejection,
wrong passwords and timeout/rejoin. No baseline source was changed.

`rebuild-back-*.log` demonstrates switching 1 -> 0 -> 1, including rebuilding
`particles.o`. `server-marker-*` and `particles-marker-*` show the actual
compiled setting. `noop-*.log` is intentionally empty. `invalid-padding.log`
records rejection of `PADDING=2`. Profile/debug/custom-CFLAGS command previews
also retain the selected macro.

`bench-*.log` checks one million increments per worker or one million FIFO
transfers in both fixed-layout binaries, pinned to virtual CPUs 0 and 1.
The counter checksums are 2,000,000 and ring checksums 500,000,500,000.
`measurement-script.log` and the sibling `opt-11-padding-smoke/` folder
verify two interleaved runs per layout/workload (8 total) with `PERF=0`.
ShellCheck passes for `bench/padding.sh` and `bench/setup_info.sh`.

These WSL timings are smoke checks, not native performance evidence. Native
perf-stat, cache-to-cache and full-game comparison commands are documented in
`docs/CONCURRENCY.md` and `bench/README.md`.

To reproduce the main checks (run each build sequentially):

```bash
source security/scripts/env.sh
make -j4 PADDING=0 all test-concurrency build/test_concurrency_tsan build/test_concurrency_asan build/test_auth_asan
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 ./build/test_concurrency_tsan
./build/test_concurrency_asan
FUSE_HANDOFF=spsc ./build/test_auth_asan
FUSE_HANDOFF=mutex ./build/test_auth_asan
# Repeat these commands with PADDING=1 before running either sanitizer.
make bench-padding
RUNS=2 ITERATIONS=100000 PERF=0 bash bench/padding.sh opt-11-padding-smoke 0 1
```
