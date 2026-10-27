# FUSE — notes for Claude Code

CSD3116 team assignment: real-time multiplayer game in C on Cyclone DDS, OpenGL (legacy fixed-function) and GLFW. Deadline Friday 6 November 2026.

- Read `docs/PLAN.md` first: game design, directory structure, threading design, the numbered optimisation list (#1–17), security plan, measurement rules and timeline.
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
