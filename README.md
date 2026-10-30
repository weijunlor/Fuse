# FUSE

Real-time multiplayer bomb-passing arena game built on Cyclone DDS, OpenGL and GLFW (CSD3116 team assignment).

Project plan, architecture and optimisation list: [docs/PLAN.md](docs/PLAN.md).

## Build (WSL / Ubuntu)

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
| | |

## AI use

See `docs/ai_log.md`.
