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
- `server/main.c` and `client/main.c` are still identical copies of the baseline; all other modules are empty stubs.
- Verified in WSL: `make` and `make baseline` build cleanly; baseline runs.
- `bench/profile.sh <label> <bin_dir>`: every profiling run writes `bench/results/<label>/` with `setup.txt`, `report.txt` and `raw/` (see `bench/README.md`). Profile every optimisation step with it, one label per step, so the final report can use all the data.
- Baseline profiled at -O0 and -O2 (`make baseline-O0 baseline-O2`, -g and frame pointers): `bench/results/baseline-O0/`, `bench/results/baseline-O2/`. At -O2: server ~1.1 CPUs, client ~1.0 CPU, ~50% of samples in `pthread_mutex_lock`/`unlock`, server ~600k futex calls/s.

Not done yet:
- No flame graph SVGs: needs FlameGraph scripts on PATH, then `STEPS=record bench/profile.sh <label> <bin_dir>` (raw/record_*.script.gz can also be fed to stackcollapse-perf.pl later).
- Baseline results not committed or tagged yet. `bench/results/old-baseline-O2-first-run/` is the earlier run (no raw perf.data); can be deleted once the new runs are committed.

Next, in order (see PLAN.md section 8 for the full timeline):
1. Commit `bench/`, Makefile and docs, then `git tag baseline-measured`.
2. Module split: move baseline code out of `server/main.c` and `client/main.c` into the stub modules, behaviour unchanged.
3. Security: `security/scripts/make_ca.sh`, `make_certs.sh`, governance/permissions XML; get the unmodified baseline running encrypted.
4. Agree the new IDL as a team (draft in PLAN.md section 2), then fixed tick, snapshots, join nonce, leave.

## Team

| Member | GitHub | Role |
|---|---|---|
| weijun | weijunlor | Server, Client, Security |
| sheeyang | | |
| zhikai | | |
