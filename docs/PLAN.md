# FUSE — Project Plan

CSD3116 Low-Level Programming team assignment: a real-time, interactive, distributed and secure game built on WSL, OpenGL, GLFW and Cyclone DDS.

**Deadline:** Friday 6 November 2026 (Moodle) — source zip, slides (.pdf), video (.mp4, ≤ 5 min, narrated by all members).

---

## 1. Game concept

A 2–5 player bomb-passing arena game. One player holds a lit bomb with a burning fuse; touching another player passes it. When the fuse runs out, the holder loses a life and a new round starts. The last player with lives wins.

### Mechanics

| Mechanic | Description |
|---|---|
| Movement | Velocity-based, `pos += vel * dt` on a fixed 60 Hz server tick |
| Bomb pass | Touching another player passes the bomb; 0.5 s no-pass-back window |
| Dash | Short speed burst with a cooldown, to escape or to tag |
| Pickups | *+speed*, *shield* (blocks one tag), *fuse-reset* |
| Embers | Explosions leave burning embers on the floor that slow players (gives the server a large workload to optimise) |
| Rounds | Lobby → countdown → rounds → win screen → rematch |
| Bots (stretch) | State machine: flee when not holding the bomb, chase the nearest player when holding it |

### Animation

Bomb pulses faster as the fuse burns down, explosion particle bursts, spark trail from the bomb, dash trails, screen shake, smooth movement through client-side interpolation.

---

## 2. Improvements over the baseline programs

| Area | Baseline | FUSE |
|---|---|---|
| Leaving | Not supported | `Leave` message + DDS liveliness timeout for crashed clients |
| Join | `JoinResponse` broadcast to all; two clients joining together can take the same ID | Client sends a random nonce and only accepts a response carrying it |
| Movement | 1 px per input message; speed depends on network rate | Fixed tick, input is held-key state, velocity × dt |
| State sync | One `Position` message per player | One `GameSnapshot` per tick |
| Input safety | `arr_Players[id-1]` written without a bounds check (out-of-bounds write) | Player ID validated and tied to the session token |
| Game flow | None | Lobby, rounds, win screen |

### Message definitions (draft)

```idl
struct JoinRequest  { unsigned long long nonce; string<16> name; sequence<octet,64> proof; };
struct JoinResponse { unsigned long long nonce; long player_id; unsigned long long token; };
struct Input        { @key long player_id; unsigned long seq; octet keys; /* bitmask */ sequence<octet,32> mac; };
struct PlayerState  { octet id; octet flags; short x, y, vx, vy; octet lives; };
struct GameSnapshot { unsigned long tick; octet bomb_holder; unsigned short fuse_ms; PlayerState players[5]; };
struct Leave        { long player_id; unsigned long long token; };
```

### QoS per topic

| Topic | QoS | Reason |
|---|---|---|
| `JoinRequest`, `JoinResponse`, `Leave` | Reliable | Must arrive |
| `Input`, `GameSnapshot` | Best-effort, keep-last 1 | Old state is useless; never resend it |

---

## 3. Directory structure

```
Fuse/
├── Makefile              # all / baseline / profile / debug (TSan) / bench / clean
├── README.md             # build, run, controls, team, AI declaration
├── idl/messages.idl      # all DDS types
├── generated/            # idlc output — never edit, gitignored
├── common/               # shared by server and client
│   ├── config.h          #   constants: window size, MAX_PLAYERS, tick rate
│   ├── types.h           #   PlayerState, snapshot layout, key bitmask
│   ├── log.h             #   log macros, compiled out of hot paths
│   ├── spsc_ring.h       #   lock-free single-producer/single-consumer ring buffer
│   ├── net.c/.h          #   DDS participant/topics/readers/writers, QoS, waitsets
│   ├── crypto.c/.h       #   HMAC, tokens, password hashing
│   └── timing.c/.h       #   monotonic clock, fixed-timestep accumulator
├── server/
│   ├── main.c            #   startup, threads, shutdown
│   ├── game.c/.h         #   tick: movement, bomb/fuse, rounds, pickups
│   ├── collision.c/.h    #   AABB tests, spatial grid for embers
│   ├── lobby.c/.h        #   join (nonce), leave, liveliness
│   ├── auth.c/.h         #   login, tokens, input validation, rate limit
│   ├── bot.c/.h          #   AI bots (stretch)
│   └── net_server.c/.h   #   DDS worker: take inputs, publish snapshots
├── client/
│   ├── main.c            #   GLFW window, main loop
│   ├── input.c/.h        #   keys → bitmask, send on change
│   ├── render.c/.h       #   batched drawing of players, bomb, HUD
│   ├── particles.c/.h    #   SoA particle pool, SIMD/threaded update
│   ├── interp.c/.h       #   snapshot buffer + interpolation
│   ├── ui.c/.h           #   lobby, login, win screen, cached text
│   └── net_client.c/.h   #   DDS worker: send input, receive snapshots
├── third_party/          # stb_easy_font.h
├── security/
│   ├── config/           #   cyclonedds.xml, governance.xml, permissions_*.xml
│   ├── scripts/          #   make_ca.sh, make_certs.sh, run_secure.sh
│   └── certs/            #   generated certs and keys — gitignored, never committed
├── bench/                # microbenchmarks (naive vs optimised in one program) + results/*.csv
├── baseline/             # original lecturer programs, untouched, for "before" measurements
└── docs/                 # PLAN.md, ai_log.md, diagrams/, slides/
```

**Principles**

- `common/`, `server/` and `client/` are separate, so shared constants live in one place and neither program compiles the other's code.
- One module per system, matching how the optimisations are presented.
- `generated/` and `build/` are disposable (`make clean`).
- `baseline/` is never modified, so "before" numbers can always be re-measured.
- Private keys are never committed; only the scripts that generate them.

---

## 4. Threading design

| Thread | Server | Client |
|---|---|---|
| **Main** | Fixed 60 Hz tick: physics → collision → game logic, in order | GLFW input + rendering (owns the GL context) |
| **Network** | DDS worker blocking on a waitset; hands inputs to main via SPSC ring | DDS worker; hands snapshots to main via double buffer |
| **Worker pool** (cores − 2) | Ember and bot updates, split into chunks | Particle updates, split into chunks |

**Not one thread per system.** The systems run in a fixed order and each needs the previous one's results, the work is very uneven (collision for 5 players is nanoseconds, particles are milliseconds), and every handoff costs synchronisation. Long-lived threads separate work that waits on different things (network I/O, vsync); the worker pool splits one heavy system's data across cores.

**SPSC instead of a mutex for the network → main handoff:** there is exactly one producer and one consumer, so neither side ever blocks, there is no lock/unlock pair to get wrong, and worst-case frame time improves. A mutex is still used where several fields must change together (e.g. the player table).

---

## 5. Optimisations: what, where, and how to prove it

| # | Optimisation | Where | Type | Proof |
|---|---|---|---|---|
| 1 | Don't sleep or publish while holding the mutex: copy state, unlock, then publish | `server/net_server.c` | Synchronisation | `perf lock contention`, `perf sched timehist` |
| 2 | Replace busy loops with DDS waitsets and `pthread_cond` | `net_*.c`, `server/main.c` | Synchronisation / CPU | `perf stat` "CPUs utilized" |
| 3 | SPSC ring for inputs, double-buffered snapshots | `common/spsc_ring.h`, `net_*.c` | TLP, lock-free | 99th-percentile frame time, `perf trace -s` (futex count) |
| 4 | Fix the data race on `int_player_id` | `client/main.c` | Correctness | Code review (TSan does not catch it at runtime: main locks the same mutex every frame, so the single write is ordered). Use TSan on the new lock-free code instead |
| 5 | Worker pool for particles and embers | `client/particles.c`, `server/collision.c` | **TLP** | Speedup vs thread count; N where threads start paying off |
| 6 | Particles as struct-of-arrays; dead particles removed by swapping with the last | `client/particles.c` | **Cache**, removes branches | `L1-dcache-load-misses`, AoS vs SoA |
| 7 | SIMD auto-vectorisation (`restrict`, `-O3 -march=native`) | `client/particles.c` | **DLP** | `-fopt-info-vec`, `perf annotate` (`vmulps`), instruction count |
| 8 | Spatial grid, sorted by cell in a flat array; power-of-two cell size (shift, not divide) | `server/collision.c` | Cache + ILP | Time and cache misses vs brute force |
| 9 | Branchless overlap test (`&`), `fminf`/`fmaxf` clamps, key → velocity lookup table | `server/collision.c`, `server/game.c` | **Branch** / ILP | `branch-misses` % |
| 10 | Multiple accumulators in reductions; constants hoisted out of loops | `server/game.c`, `bench/bench_reduce.c` | **ILP** / data dependency | IPC (instructions ÷ cycles) |
| 11 | Pad variables written by different threads onto separate cache lines (`_Alignas(64)`) | `common/spsc_ring.h`, worker pool | Cache (false sharing) | `perf c2c` HITM count |
| 12 | One `GameSnapshot` instead of N `Position` messages; compact `short` fields and bitflags | `idl/messages.idl`, `server/net_server.c` | Network + cache | Messages/s, bytes/s |
| 13 | Send input only when keys change, as a bitmask | `client/input.c` | Network | Input messages/s |
| 14 | Per-topic QoS; take several samples per `dds_take` | `common/net.c` | Network / latency | Latency, message counters |
| 15 | Client interpolation so snapshots can drop to 20–30 Hz | `client/interp.c` | Network | Bandwidth vs smoothness |
| 16 | vsync, cached text vertices, all quads in one draw call | `client/render.c`, `client/ui.c` | Rendering | Frame time, `perf record` |
| 17 | Logging compiled out of hot paths; compare `-O0` vs `-O2` | `common/log.h`, `Makefile` | General | `perf stat` time |

### By parallelism level

| Level | What it is | FUSE systems |
|---|---|---|
| **ILP** (instruction-level) | The CPU runs independent instructions in the same cycle | Collision tests, key → velocity table, reductions with several accumulators |
| **DLP** (data-level) | One instruction on many data items (SIMD, GPU) | Particle update, ember broad-phase, batched rendering |
| **TLP** (thread-level) | Separate threads on separate cores | Network threads, worker pool for particles/embers, render separate from simulation |

### Priority

- **Must do:** #1–4, #6, #12–14
- **Strong extras for the slides:** #5, #7, #9, #11
- **If time allows:** the rest

### Stress mode

5 players is too small for cache, branch or SIMD optimisations to show up. Add `--stress N --ticks T` (spawns N particles/embers/bots, runs T ticks, then exits) so runs are heavy and repeatable.

---

## 6. Security

### Layer A — DDS Security (encryption of DDS traffic)

- **Authentication:** each participant has an X.509 certificate signed by the team CA (created with `openssl`).
- **Access control:** `governance.xml` turns on encryption; `permissions.xml` restricts publishing (e.g. only the server may publish `GameSnapshot` and `JoinResponse`).
- **Encryption:** AES-GCM, configured through the XML that `CYCLONEDDS_URI` points to.
- **Check first:** `ls /usr/lib/x86_64-linux-gnu | grep dds_security`. If the plugins are missing, build Cyclone from source with `-DENABLE_SECURITY=ON` and OpenSSL.
- **Demo:** Wireshark showing readable RTPS packets before and encrypted ones after; a client without a certificate being rejected.

### Layer B — application level (authentication of clients)

- Username + password checked against a salted hash on the server; server issues a session token.
- Every `Input` carries a sequence number and an HMAC computed with the token (stops replay and spoofing).
- Player ID range-checked and matched to the token (fixes the baseline out-of-bounds write).
- Server authority: clients send key presses, never positions. Inputs rate-limited per player.

---

## 7. Measuring and reporting

### Where "before" comes from

| Kind | Before | After |
|---|---|---|
| Fixes to the lecturer's code (#1–4, #12–14) | `build/baseline_*` | FUSE running the same scenario |
| New systems (particles, grid, worker pool, SIMD) | The first simple version: AoS, single thread, brute force | The optimised version |
| Micro-optimisations (branchless, accumulators, false sharing) | `bench/bench_*.c`, naive argument | Same program, optimised argument |

### Rules

1. Change one thing at a time; measure after each optimisation, not at the end.
2. Keep the slow version runnable (compile flag such as `-DUSE_SOA=0/1`, or a bench argument).
3. Same scenario, same machine, locked CPU clock, same cores (`taskset`).
4. `perf stat -r 10 -x,` for mean ± variance, saved as CSV in `bench/results/`.
5. Tag each step in git (`git tag opt-01-unlock-before-publish`).
6. Record the baseline readings before changing any code.

### Report format (one row per optimisation in the slides)

| Optimisation | Metric | Before | After | Change | How measured |
|---|---|---|---|---|---|
| *example:* Waitset instead of busy loop | CPUs utilized (server) | … | … | … | `perf stat -r 10`, 4 clients, 30 s |

Below each table: machine (CPU, kernel, compiler flags), a graph or flame graph for the biggest wins, and analysis of *why* it got faster and where it did not help (e.g. threads do not pay off below a few thousand particles).

### Profiling on native Ubuntu (thumbdrive)

WSL2 usually does not expose hardware counters, so use the native Ubuntu boot for `perf`.

```bash
sudo apt install linux-tools-common linux-tools-$(uname -r)
sudo sysctl kernel.perf_event_paranoid=-1
sudo cpupower frequency-set -g performance
make profile
```

| To show | Command |
|---|---|
| Busy-wait CPU use | `perf stat -e task-clock,context-switches ./build/baseline_server` |
| Lock contention | `perf lock contention -a -b -- ./build/baseline_server` |
| Hotspots | `perf record -g ./build/server --stress 50000` → `perf report` / `perf annotate` |
| Cache misses | `perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-load-misses ...` |
| Branch misses | `perf stat -e branches,branch-misses ...` |
| ILP | `perf stat -e cycles,instructions ...` (IPC) |
| Thread scaling | `for t in 1 2 4 8; do perf stat -r 5 ./build/bench_particles soa $t; done` |
| False sharing | `perf c2c record ./build/bench_false_share` → `perf c2c report` |
| Syscalls / wakeups | `perf trace -s ./build/client` |
| Live game | `perf record -g -p $(pgrep -x server) -- sleep 15` |
| Flame graph | `perf script \| stackcollapse-perf.pl \| flamegraph.pl > out.svg` |

On Intel hybrid CPUs, pin runs to P-cores (`taskset`). Save results to persistent storage — a live USB session loses its home folder on reboot.

---

## 8. Timeline

| Dates | Server / lead | Client / graphics | Security / performance |
|---|---|---|---|
| **27–28 Oct** | Split code into modules, Makefile | Client render loop, vsync | Check DDS security plugins; record baseline readings |
| **29–31 Oct** | New IDL + QoS, fixed tick, snapshots, join nonce, leave/liveliness | Interpolation, bomb animation, particles, HUD | CA, certificates, governance/permissions XML; encrypted DDS working |
| **1–2 Nov** | Bomb, fuse, rounds, pickups, dash | Lobby, win screen, screen shake, text caching | Login, tokens, HMAC, sequence numbers, validation, rate limits |
| **3 Nov** | Waitsets, condition variables, shorter lock sections | Batched drawing, particle pool | Re-measure: perf, TSan, Wireshark |
| **4 Nov** | Bots (stretch), bug fixes | Polish | Slides: diagrams, before/after tables |
| **5 Nov** | **Code freeze.** Comment everything; record video (all members narrate in their own voices) | | |
| **6 Nov** | Buffer; zip and submit | | |

With two members, merge the client and security/performance columns and drop bots and pickups first.

---

## 9. Deliverables checklist

- [ ] Source zip, clearly commented (no `build/`, `generated/` or private keys)
- [ ] Slides (.pdf): build/run instructions, who did what, concept and design diagrams, optimisations with measurements, security features, AI use and testing evidence
- [ ] Video (.mp4, ≤ 5 min, ≤ 200 MB): team intro, game demo, optimisations, security — narrated by every member, no AI voice
- [ ] `docs/ai_log.md` kept up to date: prompts used and how generated code was tested
