/* When the device wakes next. Expected times written out in UTC; the zone is
 * Ireland's, which is UTC+0 in November and UTC+1 in July. */
#include "test.h"
#include "cycle_sched.h"

#define NOV14 (20771LL * 86400)   /* 2026-11-14 00:00 UTC, GMT in force */
#define JUL1  (20635LL * 86400)   /* 2026-07-01 00:00 UTC, IST in force */
#define H(h, m) ((int64_t)(h) * 3600 + (int64_t)(m) * 60)

static cal_tz_t dublin(void)
{
    cal_tz_t tz;
    cal_tz_parse_posix("GMT0IST,M3.5.0/1,M10.5.0", &tz);
    return tz;
}

static void test_wakes_on_next_half_hour(void)
{
    cal_tz_t tz = dublin();
    CHECK_INT(cycle_next_wake(NOV14 + H(10, 7), &tz, 30, 23, 6), NOV14 + H(10, 30));
}

static void test_skips_a_grid_point_less_than_a_minute_away(void)
{
    cal_tz_t tz = dublin();
    CHECK_INT(cycle_next_wake(NOV14 + H(10, 29) + 30, &tz, 30, 23, 6), NOV14 + H(11, 0));
}

static void test_evening_wake_in_quiet_hours_moves_to_next_morning(void)
{
    cal_tz_t tz = dublin();
    CHECK_INT(cycle_next_wake(NOV14 + H(22, 45), &tz, 30, 23, 6), NOV14 + H(24 + 6, 0));
}

static void test_night_wake_in_quiet_hours_moves_to_same_morning(void)
{
    cal_tz_t tz = dublin();
    CHECK_INT(cycle_next_wake(NOV14 + H(2, 0), &tz, 30, 23, 6), NOV14 + H(6, 0));
}

static void test_grid_is_local_time_in_summer(void)
{
    cal_tz_t tz = dublin();
    /* 09:10 IST is 08:10 UTC; the next wake is 09:30 IST. */
    CHECK_INT(cycle_next_wake(JUL1 + H(8, 10), &tz, 30, 23, 6), JUL1 + H(8, 30));
}

static void test_quiet_end_is_local_time_in_summer(void)
{
    cal_tz_t tz = dublin();
    /* 23:40 IST; 06:00 IST next day is 05:00 UTC. */
    CHECK_INT(cycle_next_wake(JUL1 + H(22, 40), &tz, 30, 23, 6), JUL1 + H(24 + 5, 0));
}

static void test_equal_quiet_hours_wake_through_the_night(void)
{
    cal_tz_t tz = dublin();
    CHECK_INT(cycle_next_wake(NOV14 + H(23, 50), &tz, 30, 0, 0), NOV14 + H(24, 0));
}

void test_cycle_sched(void)
{
    SUITE("cycle_sched");
    test_wakes_on_next_half_hour();
    test_skips_a_grid_point_less_than_a_minute_away();
    test_evening_wake_in_quiet_hours_moves_to_next_morning();
    test_night_wake_in_quiet_hours_moves_to_same_morning();
    test_grid_is_local_time_in_summer();
    test_quiet_end_is_local_time_in_summer();
    test_equal_quiet_hours_wake_through_the_night();
}
