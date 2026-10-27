# FUSE

Real-time multiplayer bomb-passing arena game built on Cyclone DDS, OpenGL and GLFW (CSD3116 team assignment).

Project plan, architecture and optimisation list: [docs/PLAN.md](docs/PLAN.md).

## Build (WSL / Ubuntu)

```bash
sudo apt install -y libgl1-mesa-dev libglfw3-dev cyclonedds-dev cyclonedds-tools
make            # build/server, build/client
make baseline   # build/baseline_server, build/baseline_client (original programs)
```

## Run

```bash
./build/server
./build/client   # start one per player, max 5
```

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
