# FUSE

Real-time multiplayer bomb-passing arena game built on Cyclone DDS, OpenGL and GLFW (CSD3116 team assignment).

Project plan, architecture and optimisation list: [docs/PLAN.md](docs/PLAN.md).

## Build (WSL / Ubuntu)

Packages (libssl-dev: OpenSSL headers for the login code in `common/crypto.c`
and the certificate check in `common/dds_security.c`):

```bash
sudo apt install -y pkg-config libssl-dev libgl1-mesa-dev libglfw3-dev libstb-dev cyclonedds-dev cyclonedds-tools
```

On native Ubuntu 24.04 or later, the packaged Cyclone DDS 0.10.5 includes the
security plugins (in `/usr/lib/x86_64-linux-gnu/libddsc0debian/`, found by the
Makefile), so `setup_wsl.sh` below can be skipped. On WSL, `setup_wsl.sh`
installs these packages and builds the pinned Cyclone DDS from source.

```bash
bash security/scripts/setup_wsl.sh
source security/scripts/env.sh
bash security/scripts/make_ca.sh
bash security/scripts/make_certs.sh
python3 security/scripts/accounts.py demo  # once: private application accounts
make            # build/server, build/client
make baseline   # build/baseline_server, build/baseline_client (original programs)
```

## Run

On Windows, run these from separate terminals in this folder:

```bat
.\run-server.bat
.\run-client.bat
.\run-client.bat --player 2
```

The batch files call the Linux scripts through your default WSL distribution.

On Linux or inside Ubuntu/WSL, use the same scripts directly in separate terminals:

```bash
bash run-server.sh
bash run-client.sh
bash run-client.sh --player 2
```

Run the first-time setup under **Build** above before using these scripts.
Each script builds only its own target, sources the DDS environment, and starts
its process with security enabled. `--player` accepts 1–5 (default 1); use a
different identity for each client. Both launchers accept `--no-build` to skip
compilation; the batch files forward these options to the Linux scripts.
Output stays in each terminal. Close a game window to stop that client; Ctrl+C
in the server terminal stops the server independently. Movement is WASD/arrows;
dash is Space/Shift. The game requires WSLg (or another graphical display).

To run each process manually in its own Ubuntu/WSL terminal:

```bash
source security/scripts/env.sh
bash security/scripts/run_secure.sh server ./build/server
bash security/scripts/run_secure.sh client-1 ./build/client   # use client-2..5 for others
```

DDS security is on by default. First-time certificate setup, the pinned WSL
dependency build, security tests and explicit plaintext comparisons are documented
in [security/README.md](security/README.md).
Application login setup, custom accounts and authentication tests are in
[security/AUTH.md](security/AUTH.md).

Concurrency uses SPSC command queues and double-buffered snapshots by default.
Set `FUSE_HANDOFF=mutex` on both server and clients for the comparison path.
Both modes fix the player-ID race. See [concurrency checks and profiling](docs/CONCURRENCY.md).

Cache-line padding (#11) is a compile-time choice: `make PADDING=1` (default)
separates queue/buffer fields and particle worker results; `make PADDING=0`
uses compact fields. Switching the setting rebuilds affected targets without
requiring a clean. `make bench-padding` keeps both benchmark binaries side by
side. See [padding checks and measurements](docs/CONCURRENCY.md#cache-line-padding-11).

Move with arrows or WASD; press Space or either Shift key to dash. A dash
lasts 0.2 seconds with a 1-second cooldown and requires a fresh key press.
Touch another player to pass the bomb; it cannot be passed straight back for
0.5 seconds. Shields block one tag, speed pickups last 5 seconds, and fuse
pickups reset the fuse to 20 seconds. Embers slow players for their 8-second
lifetime.

With two or more connected players, the lobby stays open for 5 seconds, then
a 3-second countdown starts. New players wait for the next lobby once the
countdown begins. Each player starts with 3 lives. The last survivor wins;
after a 5-second win screen, connected players automatically enter a new
lobby with lives restored. Eliminated players spectate until the next match.

For a solo game against a bot:

```bash
./build/server --bots 1   # 0..4 bots; bots count toward the 5 player limit
./build/client            # with the security setup above (FUSE_DDS_IDENTITY, login)
```

Server profiling options keep each slower path runnable:

| Option | Default | Comparison path |
|---|---|---|
| `--wait` | `sleep` | `busy` (#2, both server threads) |
| `--publish` | `unlocked` | `locked` (#1, snapshot publication) |
| `--collision` | `grid` | `brute` (#8, ember queries) |
| `--embers` | `ring` | `array` (#8b, ember storage: FIFO ring of SoA arrays vs fixed 4096-slot array) |
| `--branches` | `branchless` | `branchy` (#9, overlap/clamp/held-key lookup) |
| `--reduction` | `accum` | `serial` (#10, player counting) |

`--snapshot-hz H` selects a divisor of 60; `--player-timeout S` sets the
heartbeat timeout (5 seconds by default, 0 disables it). Bots use the same
game rules and are exempt from network timeouts.

## Checks

```bash
make test           # deterministic game/lobby/bot/collision/input regressions
make test-network   # isolated real DDS scenarios; no window required
make bench          # collision, branch, reduction and particle benchmarks
```

The network checks use unique discovery tags and stop only the server
processes they start. Profiling commands and comparison rules are in
[bench/README.md](bench/README.md). Implementation and validation notes for
Zhikai's remaining work are in [docs/report-notes/zhikai-completion.md](docs/report-notes/zhikai-completion.md).

## Layout

| Folder | Contents |
|---|---|
| `idl/` | DDS message definitions |
| `generated/` | idlc output (do not edit) |
| `common/` | Code shared by server and client |
| `server/`, `client/` | Each program, one module per system |
| `third_party/` | External code (stb_easy_font) |
| `security/` | DDS Security XML, certificate scripts |
| `bench/` | Microbenchmarks and results for the optimisation slides |
| `baseline/` | Original lecturer programs, untouched, for before/after measurements |
| `docs/` | Diagrams, slides, AI use log |

## Team

| Member | Responsibilities |
|---|---|
| Zhikai | Server, game rules, lobby, bots, collision and reduction benchmarks |
| Weijun | Client, rendering, assets and performance measurements |
| Sheeyang | Security and concurrency |

## AI use

See `docs/ai_log.md`.
