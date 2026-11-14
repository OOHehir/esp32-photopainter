#include "cycle_sched.h"

#include <stdbool.h>

#define MIN_SLEEP_S 60

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static bool in_quiet(int minute_of_day, int qs, int qe)
{
    if (qs == qe) return false;
    int s = qs * 60, e = qe * 60;
    return s < e ? (minute_of_day >= s && minute_of_day < e)
                 : (minute_of_day >= s || minute_of_day < e);
}

int64_t cycle_next_wake(int64_t now_utc, const cal_tz_t *tz, int interval_min,
                        int quiet_start_h, int quiet_end_h)
{
    if (interval_min < 1) interval_min = 1;
    if (interval_min > 1440) interval_min = 1440;

    int64_t local = now_utc + cal_tz_offset_at_utc(tz, now_utc);
    int64_t day = floor_div(local, 86400);
    int64_t sec_of_day = local - day * 86400;

    /* Next grid point strictly after now, plus MIN_SLEEP_S of slack so a
     * wake that ran long does not schedule a sleep of a few seconds. */
    int64_t step = (int64_t)interval_min * 60;
    int64_t next = (floor_div(sec_of_day + MIN_SLEEP_S, step) + 1) * step;
    if (next >= 86400) {
        day += 1;
        next = 0;
    }

    int minute = (int)(next / 60);
    if (in_quiet(minute, quiet_start_h, quiet_end_h)) {
        if (minute >= quiet_end_h * 60) day += 1;
        next = (int64_t)quiet_end_h * 3600;
    }
    int64_t wake = cal_tz_local_to_utc(tz, day * 86400 + next);
    return wake < now_utc + MIN_SLEEP_S ? now_utc + MIN_SLEEP_S : wake;
}
