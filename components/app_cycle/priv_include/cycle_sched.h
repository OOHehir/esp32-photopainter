/* When to wake next. Pure arithmetic, tested on the host. */
#pragma once

#include <stdint.h>

#include "cal_tz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The next wake time: the next wall-clock multiple of `interval_min` (e.g. :00 and
 *        :30), moved to `quiet_end_h`:00 when it falls in the quiet hours, and never
 *        sooner than 60 s from now.
 * @param now_utc       Current time.
 * @param tz            Local zone; the grid and the quiet hours are local wall time.
 * @param interval_min  1..1440; values that do not divide 1440 still work but the grid
 *                      restarts at local midnight.
 * @param quiet_start_h Local hour the quiet period starts, 0..23.
 * @param quiet_end_h   Local hour it ends. Equal to quiet_start_h means no quiet hours.
 * @return UTC seconds of the next wake.
 */
int64_t cycle_next_wake(int64_t now_utc, const cal_tz_t *tz, int interval_min,
                        int quiet_start_h, int quiet_end_h);

#ifdef __cplusplus
}
#endif
