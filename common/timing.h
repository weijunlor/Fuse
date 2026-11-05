/** @file common/timing.h
 * @brief Monotonic clock helper.
 */

#ifndef COMMON_TIMING_H
#define COMMON_TIMING_H

#include <stdint.h>

/** @brief Current time from CLOCK_MONOTONIC.
 * @return Nanoseconds; the clock never jumps, and only differences are meaningful.
 * @note Thread-safe.
 */
uint64_t timing_now_ns(void);

#endif /* COMMON_TIMING_H */
