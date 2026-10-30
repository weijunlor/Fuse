# FUSE

Real-time multiplayer bomb-passing arena game built on Cyclone DDS, OpenGL and GLFW (CSD3116 team assignment).

Project plan, architecture and optimisation list: [docs/PLAN.md](docs/PLAN.md).

## Build (WSL / Ubuntu)

```bash
sudo apt install -y pkg-config libgl1-mesa-dev libglfw3-dev libstb-dev cyclonedds-dev cyclonedds-tools
make            # build/server, build/client
make baseline   # build/baseline_server, build/baseline_client (original programs)
```

## Run

```bash
./build/server
./build/client   # start one per player, max 5
```

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
./build/client
```

Server profiling options keep each slower path runnable:

| Option | Default | Comparison path |
|---|---|---|
| `--wait` | `sleep` | `busy` (#2, both server threads) |
| `--publish` | `unlocked` | `locked` (#1, snapshot publication) |
| `--collision` | `grid` | `brute` (#8, ember queries) |
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
