# AI Use Log

Required by the brief: summary of prompts used and how generated code was tested.

| Date | Who | Tool | Prompt summary | Output used in | How it was tested |
|---|---|---|---|---|---|
| 2026-10-27 | | Claude Code | Game concept, optimisation plan, directory structure, Makefile | Project layout, Makefile | Built in WSL with `make`; ran baseline server + client |
| 2026-10-27 | | Claude Code | Script to profile baseline and later builds with perf (stat, lock, trace, record), saving setup, raw data and a report per run; Makefile `baseline-O%` targets | `bench/profile.sh`, `Makefile` | Short smoke run (2 × 4 s) checked every output file; parsers checked against an earlier full run; frame-pointer call stacks checked to reach `main`/`worker`; full -O0 and -O2 baseline runs |
| 2026-10-27 | | Claude Code | Split baseline server/client into the planned modules without changing behaviour | `server/*.c`, `client/*.c`, `common/config.h`, `common/types.h`, `common/net.c` | Built with no new warnings; DDS test driver (join + 30 right + 1000 up) gave matching results on baseline and split servers; split server with new and baseline clients (joins and positions); `bench/profile.sh split-modules` compared with `baseline-O2` |
