# FUSE — notes for Claude Code

CSD3116 team assignment: real-time multiplayer game in C on Cyclone DDS, OpenGL (legacy fixed-function) and GLFW. Deadline Friday 6 November 2026.

- Read `docs/PLAN.md` first: game design, directory structure, threading design, the numbered optimisation list (#1–17), security plan, measurement rules and timeline.
- `docs/BRIEF.md` has the assignment requirements, deliverables and grading weights. Check work against it.
- On a new machine: install packages from `README.md`, then run `make && make baseline` and confirm the security plugin path (see Findings).
- `baseline/` is the lecturer's original code. Never modify it; it is the "before" for every measurement.
- Build: `make` (game), `make baseline`, `make profile` (for perf), `make debug` (ThreadSanitizer), `make bench`.
- Change one optimisation at a time, keep the slow path runnable (compile flag or bench argument), and save `perf stat -r 10 -x,` output to `bench/results/`.
- Compare before/after numbers only from the same machine.
- Match the existing code style: C11, `snake_case`, type-prefixed names (`int_`, `b_`, `arr_`), clear block comments. Code must be well commented (graded).
- Record any AI-generated code and how it was tested in `docs/ai_log.md` (required by the brief).

## Findings so far

- Cyclone DDS security plugins on Ubuntu 24.04 (WSL) are in `/usr/lib/x86_64-linux-gnu/libddsc0debian/`, not the default library path. Check the path again on other machines.
- Baseline client uses ~300% CPU in WSL: uncapped rendering (no vsync), busy-looping DDS worker, likely llvmpipe software rendering threads.
- TSan does not report the `int_player_id` race at runtime (the main thread locks the same mutex every frame, so the single write is almost always ordered). Treat it as a code-review finding, not a TSan result.
- WSL2 usually lacks hardware perf counters; use native Ubuntu for cache/branch/IPC numbers.
- Native Ubuntu 26.04 laptop (i7-14650HX, P-cores = CPUs 0-15, E-cores 16-23): README packages plus `pkg-config` are needed. Ubuntu's perf has no BPF, so `perf lock contention -b` fails; use `perf lock record` and then `perf lock contention -i`. Kernel symbols need `kptr_restrict=0`; tracepoint files are root-only, so `bench/profile.sh` runs perf lock/trace with sudo (the user must have set paranoid/kptr/governor first; ask them).
- `perf lock contention` (tracepoints) sees kernel locks only, not pthread mutexes. Baseline user-space mutex contention shows as futex syscalls in `perf trace -s`: server main thread 7.3M and DDS worker 11.3M futex calls in 30 s (~620k/s). Use the futex count as the "before" for #1–3.
- Baseline server CPU use is bimodal: 8 of 10 runs ~1.19 CPUs, 2 runs ~0.79. Report per-run values, not just mean ± stddev.

## Status (update this section as work progresses)

Last updated: 2026-10-27

Done:
- Repo scaffold: directory structure, Makefile, module stubs, `docs/PLAN.md`, `docs/BRIEF.md` (assignment requirements and grading).
- Module split (behaviour unchanged, baseline bugs kept for the optimisation steps): server → `collision.c` (AABB), `game.c` (player table, `mutex`, `game_apply_input`), `lobby.c` (join handshake flags, `lobby_poll`), `net_server.c` (DDS worker); client → `net_client.c` (DDS worker, `int_player_id`, mirrored `arr_Players`, `mutex`), `input.c` (`ch_dir`), `render.c`, `ui.c` (`ui_draw_text`); shared → `common/config.h`, `types.h` (`Player`), `net.c` (`net_create_topics`). Still stubs: `auth`, `bot`, `interp`, `particles`, `crypto`, `timing`, `log.h`, `spsc_ring.h`. IDL is still the baseline IDL.
  Tested: a DDS test driver (join, 30 × right, 1000 × up) gives the same results on baseline and split servers; split server works with both new and baseline clients; profiled as `bench/results/split-modules/`.
- Verified in WSL: `make` and `make baseline` build cleanly; baseline runs.
- IDL v1 decided: `idl/fuse.idl` (not yet built; replaces `idl/messages.idl` when the server switches to snapshots), constants in `common/types.h`, rationale in PLAN.md section 2.
- Client draws sprites from `assets/atlas.png` (64x64 pixel-art tiles, layout in `client/atlas.h`; regenerate placeholders with `python3 assets/make_placeholder_atlas.py`). Simple immediate-mode version (one glBegin/glEnd per sprite) = the "before" for #16. F12 or `FUSE_SCREENSHOT=<file>` saves a screenshot. Needs `libstb-dev` (stb_image, stb_image_write via `pkg-config stb`). Client must start from the repo root (atlas path is relative).
- Git: `baseline-measured` tag on d537567 (baseline profiles); module split committed as 2f36af2, tagged `split-modules` (the "before" for the first optimisation). Pushed to github.com/weijunlor/Fuse (gh logged in as weijunlor). The user runs git commit/tag/push themselves.
- `bench/profile.sh <label> <bin_dir>`: every profiling run writes `bench/results/<label>/` with `setup.txt`, `report.txt` and `raw/` (see `bench/README.md`). Profile every optimisation step with it, one label per step, so the final report can use all the data.
- Baseline profiled at -O0 and -O2 (`make baseline-O0 baseline-O2`, -g and frame pointers): `bench/results/baseline-O0/`, `bench/results/baseline-O2/`. At -O2: server ~1.1 CPUs, client ~1.0 CPU, ~50% of samples in `pthread_mutex_lock`/`unlock`, server ~600k futex calls/s.

Not done yet:
- No flame graph SVGs: needs FlameGraph scripts on PATH, then `STEPS=record bench/profile.sh <label> <bin_dir>` (raw/record_*.script.gz can also be fed to stackcollapse-perf.pl later).

Next (see PLAN.md section 8 for the full timeline):
1. All: review the decided IDL (`idl/fuse.idl`, PLAN.md section 2) and raise objections before zhikai builds on it.
2. sheeyang (can start now, independent of IDL): `security/scripts/make_ca.sh`, `make_certs.sh`, governance/permissions XML; get the unmodified baseline running encrypted.
3. zhikai: fixed tick, snapshots, join nonce, leave (switch Makefile to `idl/fuse.idl`), in measurable steps (#12, then #13, #14).
4. weijun: bomb/explosion/ember rendering once snapshots exist; final art swapped into `assets/atlas.png` 3-4 Nov (same layout).
5. Measurement rule: if features merged since the last profile, profile right before and right after each optimisation, so the change is measured on its own.

## Team

| Member | GitHub | Role | Owns (optimisations) |
|---|---|---|---|
| weijun | weijunlor | Client, rendering and assets; **runs all perf measurements** (only perf machine) | Render loop, texture atlas, animations, particles, HUD/UI, interpolation (#5, #6, #7, #13, #15, #16, #17); `bench_particles`; `bench/results/` tables |
| sheeyang | | Security and concurrency | DDS Security layer A, app security layer B (`crypto.c`, `auth.c`), SPSC ring, race fix (#3, #4, #11); `bench_false_share`; Wireshark/TSan evidence |
| zhikai | | Server and game | IDL lead, fixed tick, snapshots, game rules, embers, lobby, bots (#1, #2, #8, #9, #10, #12, #14); `bench_collision`, `bench_reduce` |

Workflow: features merge in any order. Optimisations are merged one at a time by weijun, profiled with `bench/profile.sh opt-NN-<name> build`, then tagged, so each measurement covers one change. Others check correctness themselves (TSan, tests, `-fopt-info-vec`) before handing over.
