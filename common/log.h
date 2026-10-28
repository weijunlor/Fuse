/* common/log.h
Log-level macros (optimisation #17). Debug logging compiles out of hot paths.

Levels, most important first: LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG.
Messages above the build's LOG_LEVEL are removed by the preprocessor, so they
cost nothing at run time (no call, no formatting, no string in the binary).
  make              LOG_LEVEL = LOG_LEVEL_INFO: errors, warnings, one-off events (joins)
  make LOG=debug    LOG_LEVEL = LOG_LEVEL_DEBUG: also one line per network message

Use LOG_DEBUG for anything that runs per message, per tick or per frame, and
LOG_INFO for events that happen a few times per game. Errors and warnings go
to stderr, which is unbuffered, so they appear even if the program crashes.

Measured: the baseline printed ~40 lines/s per process, which are block-
buffered when stdout is not a terminal (2 write syscalls in 30 s in
bench/results/baseline-O2), so removing them is not a measurable speed-up.
The macros keep it that way as message rates grow (snapshots at 60 Hz, inputs). */

#ifndef COMMON_LOG_H
#define COMMON_LOG_H

#include <stdio.h>

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

// Default for builds that do not set it (the Makefile passes -DLOG_LEVEL=...)
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

// A disabled message: the "if (0)" keeps the compiler checking the format
// string and arguments, then removes the call entirely (even at -O0)
#define LOG_DISABLED(...) do { if (0) printf(__VA_ARGS__); } while (0)

#define LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG_WARN(...) LOG_DISABLED(__VA_ARGS__)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(...) printf(__VA_ARGS__)
#else
#define LOG_INFO(...) LOG_DISABLED(__VA_ARGS__)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) printf(__VA_ARGS__)
#else
#define LOG_DEBUG(...) LOG_DISABLED(__VA_ARGS__)
#endif

#endif /* COMMON_LOG_H */
