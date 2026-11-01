# Concurrency correctness evidence — 30 October 2026

Source base: `8713698` plus the local #3/#4 changes; source hashes are in
`source-sha256.json`. Ubuntu 22.04 WSL, GCC 11.4, Cyclone DDS 0.10.5.

| Check | Result | Evidence |
|---|---|---|
| Bounded FIFO, unsigned wrap, full/empty, pinning, coalescing | Pass | `unit.log` |
| 250,000 FIFO transfers + 250,000 latest-value publications | Pass | `unit.log` |
| Same primitive stress with ThreadSanitizer | Pass, no suppressions | `tsan-primitives.log` |
| AddressSanitizer + UndefinedBehaviorSanitizer | Pass: primitives and both lifecycle modes | `asan-ubsan.log` |
| Authentication/lifecycle including queued copies, overflow, stale sessions, response backpressure and snapshot divisors | Pass: 8,773 assertions SPSC; 8,387 mutex | `unit.log`, `spsc/unit.log`, `mutex/unit.log` |
| Real DDS acceptance, secure and plaintext, movement/Leave, replay/tampering, timeout/rejoin | Pass: 19 checks per handoff mode | `spsc/report.txt`, `mutex/report.txt`, `integration.log` |
| Two real WSLg clients, coherent players/IDs, normal shutdown and signed Leave | Pass | `gui-report.json`, `gui.log`, `gui-client-*.png`, `gui-server.log` |
| Invalid handoff mode | Rejected before startup | `gui-report.json` |
| Full-process ThreadSanitizer with instrumented DDS | Limited: DDS GC/discovery race, not a clean run | `production.log`, `production-report.json`, `tsan-server-spsc.log`, `tsan-worker-spsc.log` |

The production TSan diagnostic initially exposed an application race in lazy
network-option initialization; that code now uses `pthread_once`. The retained
full-process reports are subsequent dependency reports inside DDS 0.10.5
(`gc_buckets_cb`/`ddsrt_free` versus `ddsrt_chh_lookup`). They are not suppressed
or counted as passes. Building an isolated instrumented DDS under build/tsan-dds
did not eliminate that report. The standalone new queue/buffer stress is clean.

Both real clients completed 6,000 frames, sent signed Leave and had their
slots freed. Screenshots were inspected: each shows both players and the
correct local player banner. The OpenGL renderer was D3D12 (AMD Radeon Graphics).

No performance speedup or hardware-counter claim is made from these checks.
`FUSE_HANDOFF=mutex|spsc` supports subsequent native-machine measurement;
the snapshot path overlaps #1, as explained in `docs/CONCURRENCY.md`.
Cache-line padding (#11) is a separate experiment.

Reproduction: [docs/CONCURRENCY.md](../../../docs/CONCURRENCY.md).
