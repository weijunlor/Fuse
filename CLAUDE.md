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

## Status (update this section as work progresses)

Last updated: 2026-10-27

Done:
- Repo scaffold: directory structure, Makefile, module stubs, `docs/PLAN.md`, `docs/BRIEF.md` (assignment requirements and grading).
- `server/main.c` and `client/main.c` are still identical copies of the baseline; all other modules are empty stubs.
- Verified in WSL: `make` and `make baseline` build cleanly; baseline runs.

Not done yet:
- Baseline readings are not saved: `bench/results/` is empty. Observed but unrecorded: baseline client ~300% CPU in WSL.

Next, in order (see PLAN.md section 8 for the full timeline):
1. On native Ubuntu: record baseline readings of `build/baseline_*` (CPU, `perf lock contention`, flame graph) into `bench/results/`, then `git tag baseline-measured`.
2. Module split: move baseline code out of `server/main.c` and `client/main.c` into the stub modules, behaviour unchanged.
3. Security: `security/scripts/make_ca.sh`, `make_certs.sh`, governance/permissions XML; get the unmodified baseline running encrypted.
4. Agree the new IDL as a team (draft in PLAN.md section 2), then fixed tick, snapshots, join nonce, leave.

## Team

| Member | GitHub | Role |
|---|---|---|
| weijun | weijunlor | Server, Client, Security |
| sheeyang | | |
| zhikai | | |
