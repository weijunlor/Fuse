# Concurrency handoff (#3 and #4)

The default `FUSE_HANDOFF=spsc` uses bounded single-producer/single-consumer
rings between the server's network and main threads. `FUSE_HANDOFF=mutex`
retains the earlier queue/state-mutex path for comparisons. Choose the same
mode for the server and clients. Invalid values fail startup.

## Ownership and authentication

The DDS worker copies JoinRequest, Input and Leave samples before DDS reuses
their storage. The main thread consumes them, authenticates against the
current session and changes the player table. Join responses travel back on
a separate ring. The state mutex is retained around lifecycle/validation
operations, but the network thread does not acquire it in SPSC mode.

Queue capacities: 256 inputs, 16 joins, 16 leaves and 16 join responses.
The main thread consumes a bounded batch per pass so an input flood cannot
turn a drain into an infinite loop. A full response ring leaves pending joins
queued. Input overflow discards the newest attempted update without changing
authentication's sequence watermark; the existing 100 ms repeat recovers it.
Joins retry after 1 s. A Leave discarded at capacity is recovered by the
existing player timeout. If timeouts are explicitly disabled, that recovery
is disabled too. DDS reliable delivery only acknowledges arrival at DDS;
it is not an application-level queue acknowledgement.

Input and Leave queues contain untrusted samples. A successful submit means
only that a copy was queued. MAC, sequence, player/session and rate-limit
checks happen at consumption, including if a slot has timed out or been
reused since the sample arrived. The existing direct receive functions are
retained for the mutex path and lifecycle tests.

On the client, only the network worker accesses the live player ID, session
and current snapshot. It publishes a `ClientFrame` containing both ID and
snapshot. The renderer takes one immutable copy for sprites, labels and the
banner; it keeps the previous complete frame if nothing new is available.
The main thread sends held keys and shutdown through independent atomic
scalars. There is no publicly shared, unprotected `int_player_id` (#4).
Network options now use `pthread_once`: the production TSan check exposed
a separate first-access race in the former lazy `net_leave()` cache.

## Two-buffer protocol

Both server snapshots and client frames use `common/double_buffer.h`.
Bit 0 of an atomic state selects the published slot; bits 1/2 pin a slot
while the single consumer copies it. The producer writes only the other,
unpinned slot, then publishes with a release RMW. The consumer acquires a pin
with one CAS and releases it after copying. An acquire on the producer's next
pass orders reuse after that release. All state modifications are RMWs,
preserving the C11 release sequence. Per-slot versions distinguish new data
from repeated reads.

A writer that laps a pinned reader skips that publication. A reader whose
pin CAS collides keeps its previous frame. Neither spins inside the handoff;
intermediate snapshots may coalesce, as with DDS keep-last 1. Pinning prevents
the C data race that would occur if a producer merely flipped an index and
then reused a slot still being copied. This is not a seqlock over plain data.
The SPSC ring similarly uses release/acquire on publication and slot reuse.
See [GCC's memory-order descriptions](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html).

Initialise automatic instances before starting threads; production server
instances have static zero initialisation. Each ring/buffer must have exactly
one producer and one consumer. Do not reset or change modes while live.
Cache-line separation is independently selectable at compile time (#11),
as described below. Both layouts use the same handoff algorithms/memory orders.

## Build and checks

Run from Ubuntu/WSL, with DDS security provisioned as described in
`security/README.md`:

```bash
source security/scripts/env.sh
make -j4 all baseline
make test-concurrency
make test-concurrency-integration
make build/test_concurrency_tsan
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 ./build/test_concurrency_tsan
make build/test_concurrency_asan build/test_auth_asan
./build/test_concurrency_asan
FUSE_HANDOFF=spsc ./build/test_auth_asan
FUSE_HANDOFF=mutex ./build/test_auth_asan
make build/concurrency-tsan/server build/concurrency-tsan/auth_worker
make concurrency-tsan-dds
python3 bench/check_concurrency.py --gui
# Optional full-process diagnostic; see the DDS limitation below:
python3 bench/check_concurrency.py --tsan --tsan-dds-lib build/tsan-dds/lib
```

`setarch -R` is a per-process workaround for GCC 11 TSan's unexpected-memory-
mapping startup failure on this WSL installation. It changes no system setting.
The production harness owns and stops only its own child PIDs and refuses to
run alongside an existing server. GUI checks require WSLg and use the client's
normal frame-limited exit, including worker stop/join and signed Leave.
The optional production TSan run uses an isolated instrumented DDS library
and security plugins under build/tsan-dds; the normal installation is unchanged.
Full-process TSan is NOT clean: even with that dependency instrumented, the
pinned DDS 0.10.5 reports a race between `gc_buckets_cb`/`ddsrt_free` and
`ddsrt_chh_lookup` during discovery. Earlier runs with the uninstrumented
release library also reported DDS receive-buffer/GC accesses. No suppressions
are used, and these reports are retained in `tsan-server-spsc.log` and
`tsan-worker-spsc.log`. The optional diagnostic exits unsuccessfully at that
dependency report; it must not be described as a passing whole-game TSan run.
The standalone queue/buffer TSan stress test passes with no dependency or
suppression. AddressSanitizer/UBSan and functional DDS checks run separately.

The primitive checks cover full/empty and counter wrap, FIFO ordering across
250,000 transfers, a pinned reader, coalescing and coherent/monotonic delivery
across 250,000 publications. Lifecycle regressions cover queue overflow,
copied DDS storage, delayed samples after timeout/slot reuse, queued Leave,
response backpressure and snapshot-rate divisors. Real-DDS acceptance covers
authentication, movement, tampering/replay, Leave, timeout/rejoin and explicit
plaintext comparisons in each mode.

Evidence is saved in `bench/results/concurrency-tests/`. Sanitizer tests are
correctness evidence, not performance measurements.

## Profiling handoff

Use the same security settings, machine and scenario for each mode:

```bash
FUSE_HANDOFF=mutex MOVE=1 bench/profile.sh opt-03-mutex build
FUSE_HANDOFF=spsc MOVE=1 bench/profile.sh opt-03-spsc build
```

`bench/setup_info.sh` records the handoff mode. The comparison mode is a
race-free frame-copy implementation, not the original historical client:
ID ownership, atomic keys/shutdown and rendering one consistent frame apply
to both modes. Use the previous commit or unchanged baseline for historical
comparisons, and never label its known race as an intentional live option.

SPSC snapshot publication necessarily removes the server's state mutex from
that path; comparison mode retains publish-under-mutex. A direct mode
comparison therefore overlaps the snapshot portion of #1. Coordinate the
order of #1/#3 with Weijun before claiming isolated speedups. Waitsets/sleep
(#2) remain separate: these application loops still busy-poll. No hardware
counter or speedup claim is made from the WSL correctness runs.

## Cache-line padding (#11)

`make PADDING=1` (default) defines `FUSE_CACHE_PADDING=1`. `PADDING=0` keeps
compact fields. `common/cache_padding.h` applies `_Alignas(64)` only in the
padded build; no runtime branch is added. The flag covers:

- SPSC head, tail and payload, each starting on its own line.
- Double-buffer shared state, producer/consumer version counters and slots.
  Slot sizes round up to whole lines so the two slots cannot share an edge.
- Particle worker live-count results, one line per worker instead of adjacent
  ints. Particle arrays/chunk boundaries keep their existing SIMD alignment.

The 64-byte size targets the team's x86-64 machines. Padding increases memory
use and does not remove true sharing: both threads still read queue counters,
and both modify the double-buffer state. Measure the effect on the workload.
Static/automatic instances honour the type alignment. Future heap instances
must use an aligned allocator. Read-only worker-start records are unchanged.

The Makefile tracks the setting in `build/.cache-padding`. Switching 0/1
rebuilds affected game, benchmark, test and sanitizer targets, including
`particles.o`; repeating the same setting does not recompile. Do not run
two separate make processes with different settings against the same build
directory. Invalid values are rejected.

```bash
source security/scripts/env.sh
make -j4 PADDING=0 all test-concurrency
make -j4 PADDING=1 all test-concurrency
make bench-padding
# Dedicated compact/padded binaries coexist regardless of the game's setting:
./build/bench_false_share_0 counters 10000000
./build/bench_false_share_1 counters 10000000
./build/bench_false_share_0 ring 10000000
./build/bench_false_share_1 ring 10000000
```

The primitive tests check alignment, offsets and array stride, as well as the
same FIFO/wrap/coherent-snapshot checks in both builds. The benchmark checks
the final counters or every FIFO value/checksum, and reports the compiled flag,
layout sizes, offsets and elapsed time. `counters` is a synthetic pair of
independent relaxed atomic increments; `ring` uses the production queue.
One counters iteration is one increment per worker; one ring iteration is
one complete producer/consumer transfer. Do not compare rates across modes.
Thread creation and CPU-affinity setup are outside the timing window: both
workers reach a readiness barrier before the main thread records the start
timestamp. A separate start phase releases the workers; start and completion
barrier overhead remains inside the measured interval.

For native measurements, choose CPUs on different physical cores using
`lscpu -e=CPU,CORE,SOCKET`, then run (replace `0 2` with those CPU IDs):

```bash
RUNS=10 ITERATIONS=10000000 bash bench/padding.sh opt-11-padding 0 2
# Optional cache-to-cache evidence; use the same CPU IDs and iterations:
perf c2c record -o build/padding-0.data -- ./build/bench_false_share_0 counters 10000000 0 2
perf c2c report -i build/padding-0.data
perf c2c record -o build/padding-1.data -- ./build/bench_false_share_1 counters 10000000 0 2
perf c2c report -i build/padding-1.data
```

The script warms both binaries and interleaves 10 runs per layout/workload,
alternating which layout runs first. `bench/results/<label>/` contains setup,
binary hashes, commands, per-run timing/checksums in `results.csv`, and raw
perf-stat CSV. `PERF=0` permits a timing-only WSL smoke run; WSL timings do not
establish native cache/false-sharing speedups. `bench/setup_info.sh` reads the
compiled padding marker from each binary rather than the environment.

For the real game, keep security, handoff mode and all scenario settings equal:

```bash
make PADDING=0 profile
FUSE_HANDOFF=spsc MOVE=1 bash bench/profile.sh opt-11-game-compact build
make PADDING=1 profile
FUSE_HANDOFF=spsc MOVE=1 bash bench/profile.sh opt-11-game-padded build
```

The earlier particle implementation already padded worker results; the new
flag makes that choice measurable too. For an isolated historical #3
comparison, use the same padding flag in both handoff modes and account for
that pre-existing particle layout. #11 comparisons keep `FUSE_HANDOFF` fixed.
