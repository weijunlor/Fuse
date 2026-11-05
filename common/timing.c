/** @file common/timing.c
 * @brief Monotonic clock helper (clock_gettime(CLOCK_MONOTONIC)).
 */

#include <time.h>

#include "timing.h"

uint64_t timing_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
