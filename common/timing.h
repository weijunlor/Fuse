/* common/timing.h
Monotonic clock helpers and fixed-timestep accumulator. */

#ifndef COMMON_TIMING_H
#define COMMON_TIMING_H

#include <stdint.h>

// Nanoseconds from a monotonic clock (never jumps; only differences are meaningful)
uint64_t timing_now_ns(void);

#endif /* COMMON_TIMING_H */
