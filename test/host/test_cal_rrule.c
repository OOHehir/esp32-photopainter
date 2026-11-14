#include <stdio.h>

#include "cal_parse.h"
#include "cal_test_util.h"
#include "test.h"

/* Recurrence through the public parser, in UTC so the dates are the point.
 * Expected dates were listed with Python's calendar module, or are RFC 5545's own
 * examples (section 3.8.5.3). */

static int64_t day_of(const char *iso)
{
    int y, m, d;
    sscanf(iso, "%d-%d-%d", &y, &m, &d);
    return cal_days_from_civil(y, (unsigned)m, (unsigned)d);
}

/* Start dates of the instances in [from, to), space-separated. */
static const char *expand(const char *props, const char *from, const char *to,
                          cal_stats_t *st)
{
    static char ics[4096], dates[2048];
    snprintf(ics, sizeof(ics),
             "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u1\r\nSUMMARY:E\r\n%sEND:VEVENT\r\n"
             "END:VCALENDAR\r\n", props);
    const cal_window_t w = {day_of(from) * 86400, day_of(to) * 86400, NULL, 0, false};
    cal_event_t out[128];
    size_t n;
    cal_test_run(ics, strlen(ics), strlen(ics), &w, out, 128, &n, st);
    size_t at = 0;
    dates[0] = '\0';
    for (size_t i = 0; i < n && at + 12 < sizeof(dates); i++) {
        int64_t y;
        unsigned m, d;
        cal_civil_from_days(out[i].start_day, &y, &m, &d);
        at += (size_t)snprintf(dates + at, sizeof(dates) - at, "%s%04d-%02u-%02u",
                               i ? " " : "", (int)y, m, d);
    }
    return dates;
}

static void test_weekly_interval_2_on_mon_wed_fri(void)
{
    CHECK_STR(expand("DTSTART:20261005T070000\r\n"
                     "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR\r\n",
                     "2026-10-01", "2026-11-01", NULL),
              "2026-10-05 2026-10-07 2026-10-09 2026-10-19 2026-10-21 2026-10-23");
}

static void test_monthly_second_tuesday(void)
{
    CHECK_STR(expand("DTSTART:20260113T193000\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU\r\n",
                     "2026-01-01", "2027-01-01", NULL),
              "2026-01-13 2026-02-10 2026-03-10 2026-04-14 2026-05-12 2026-06-09 "
              "2026-07-14 2026-08-11 2026-09-08 2026-10-13 2026-11-10 2026-12-08");
}

static void test_monthly_last_friday(void)
{
    CHECK_STR(expand("DTSTART:20260130T120000\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR\r\n",
                     "2026-01-01", "2027-01-01", NULL),
              "2026-01-30 2026-02-27 2026-03-27 2026-04-24 2026-05-29 2026-06-26 "
              "2026-07-31 2026-08-28 2026-09-25 2026-10-30 2026-11-27 2026-12-25");
}

static void test_monthly_31st_skips_short_months(void)
{
    CHECK_STR(expand("DTSTART:20260131T090000\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=31\r\n",
                     "2026-01-01", "2027-01-01", NULL),
              "2026-01-31 2026-03-31 2026-05-31 2026-07-31 2026-08-31 2026-10-31 2026-12-31");
}

static void test_monthly_31st_from_dtstart_alone_skips_short_months(void)
{
    CHECK_STR(expand("DTSTART:20260131T090000\r\nRRULE:FREQ=MONTHLY;COUNT=4\r\n",
                     "2026-01-01", "2027-01-01", NULL),
              "2026-01-31 2026-03-31 2026-05-31 2026-07-31");
}

static void test_monthly_last_day_by_negative_monthday(void)
{
    CHECK_STR(expand("DTSTART:20260131T090000\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1\r\n",
                     "2026-01-01", "2026-05-01", NULL),
              "2026-01-31 2026-02-28 2026-03-31 2026-04-30");
}

static void test_monthly_friday_13th(void)
{
    CHECK_STR(expand("DTSTART:20260213T090000\r\n"
                     "RRULE:FREQ=MONTHLY;BYDAY=FR;BYMONTHDAY=13\r\n",
                     "2026-01-01", "2028-01-01", NULL),
              "2026-02-13 2026-03-13 2026-11-13 2027-08-13");
}

static void test_last_weekday_of_month_by_setpos(void)
{
    CHECK_STR(expand("DTSTART:20260529T160000\r\n"
                     "RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1\r\n",
                     "2026-05-01", "2026-09-01", NULL),
              "2026-05-29 2026-06-30 2026-07-31 2026-08-31");
}

static void test_yearly_last_sunday_of_march(void)
{
    CHECK_STR(expand("DTSTART:20260329T010000\r\n"
                     "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n",
                     "2026-01-01", "2029-01-01", NULL),
              "2026-03-29 2027-03-28 2028-03-26");
}

static void test_yearly_all_day_birthday(void)
{
    CHECK_STR(expand("DTSTART;VALUE=DATE:19801030\r\nDTEND;VALUE=DATE:19801031\r\n"
                     "RRULE:FREQ=YEARLY\r\n",
                     "2025-01-01", "2028-01-01", NULL),
              "2025-10-30 2026-10-30 2027-10-30");
}

static void test_yearly_feb_29_only_in_leap_years(void)
{
    CHECK_STR(expand("DTSTART;VALUE=DATE:20240229\r\nRRULE:FREQ=YEARLY\r\n",
                     "2025-01-01", "2033-01-01", NULL),
              "2028-02-29 2032-02-29");
}

static void test_count_stops_the_series(void)
{
    CHECK_STR(expand("DTSTART:20261001T090000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n",
                     "2026-09-01", "2026-11-01", NULL),
              "2026-10-01 2026-10-02 2026-10-03 2026-10-04 2026-10-05");
}

static void test_exdate_does_not_extend_count(void)
{
    CHECK_STR(expand("DTSTART:20261001T090000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                     "EXDATE:20261003T090000\r\n",
                     "2026-09-01", "2026-11-01", NULL),
              "2026-10-01 2026-10-02 2026-10-04 2026-10-05");
}

static void test_count_is_honored_when_the_window_starts_late(void)
{
    /* An implementation that jumps ahead to the window would lose count. */
    CHECK_STR(expand("DTSTART:20260101T090000\r\nRRULE:FREQ=WEEKLY;COUNT=40\r\n",
                     "2026-09-20", "2026-11-01", NULL),
              "2026-09-24 2026-10-01");
}

static void test_until_utc_is_inclusive(void)
{
    CHECK_STR(expand("DTSTART:20261001T090000Z\r\n"
                     "RRULE:FREQ=DAILY;UNTIL=20261004T090000Z\r\n",
                     "2026-09-01", "2026-11-01", NULL),
              "2026-10-01 2026-10-02 2026-10-03 2026-10-04");
}

static void test_until_date_on_all_day_series_is_inclusive(void)
{
    CHECK_STR(expand("DTSTART;VALUE=DATE:20261001\r\nRRULE:FREQ=DAILY;UNTIL=20261003\r\n",
                     "2026-09-01", "2026-11-01", NULL),
              "2026-10-01 2026-10-02 2026-10-03");
}

static void test_exdate_lists_dates_and_repeated_lines(void)
{
    CHECK_STR(expand("DTSTART:20261001T090000\r\nRRULE:FREQ=DAILY;COUNT=7\r\n"
                     "EXDATE:20261002T090000,20261004T090000\r\n"
                     "EXDATE;VALUE=DATE:20261006\r\n",
                     "2026-09-01", "2026-11-01", NULL),
              "2026-10-01 2026-10-03 2026-10-05 2026-10-07");
}

static void test_rdate_adds_instances(void)
{
    cal_stats_t st;
    CHECK_STR(expand("DTSTART:20261001T100000\r\n"
                     "RDATE:20261003T100000,20261007T100000\r\n"
                     "RDATE;VALUE=PERIOD:20261009T100000/PT1H\r\n",
                     "2026-09-01", "2026-11-01", &st),
              "2026-10-01 2026-10-03 2026-10-07");
    CHECK_INT(st.unsupported_rrule, 1);
}

static void test_wkst_changes_biweekly_weeks_rfc_example(void)
{
    CHECK_STR(expand("DTSTART:19970805T090000\r\n"
                     "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4;BYDAY=TU,SU;WKST=MO\r\n",
                     "1997-08-01", "1997-09-30", NULL),
              "1997-08-05 1997-08-10 1997-08-19 1997-08-24");
    CHECK_STR(expand("DTSTART:19970805T090000\r\n"
                     "RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=4;BYDAY=TU,SU;WKST=SU\r\n",
                     "1997-08-01", "1997-09-30", NULL),
              "1997-08-05 1997-08-17 1997-08-19 1997-08-31");
}

static void test_unsupported_rule_yields_dtstart_only(void)
{
    cal_stats_t st;
    CHECK_STR(expand("DTSTART:20261001T090000\r\nRRULE:FREQ=DAILY;BYHOUR=9,17\r\n",
                     "2026-09-01", "2026-11-01", &st),
              "2026-10-01");
    CHECK_INT(st.unsupported_rrule, 1);
    CHECK_STR(expand("DTSTART:20261001T090000\r\nRRULE:FREQ=HOURLY;INTERVAL=6\r\n",
                     "2026-09-01", "2026-11-01", &st),
              "2026-10-01");
    CHECK_INT(st.unsupported_rrule, 1);
}

static void test_daily_series_from_2010_reaches_the_window(void)
{
    CHECK_STR(expand("DTSTART:20100101T083000\r\nRRULE:FREQ=DAILY\r\n",
                     "2026-10-19", "2026-10-22", NULL),
              "2026-10-19 2026-10-20 2026-10-21");
    CHECK_STR(expand("DTSTART:20100101T083000\r\nRRULE:FREQ=DAILY;INTERVAL=3\r\n",
                     "2026-10-19", "2026-10-25", NULL),
              "2026-10-19 2026-10-22");
}

static void test_instance_starting_before_window_and_running_into_it(void)
{
    /* The Sunday 22:00 instance of a 4-hour weekly event ends on Monday. */
    CHECK_STR(expand("DTSTART:20260104T220000\r\nDTEND:20260105T020000\r\n"
                     "RRULE:FREQ=WEEKLY\r\n",
                     "2026-10-19", "2026-10-20", NULL),
              "2026-10-18");
}

static void test_all_day_override_by_date_moves_one_instance(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\nUID:bins\r\nSUMMARY:Bins\r\nDTSTART;VALUE=DATE:20261001\r\n"
        "RRULE:FREQ=WEEKLY\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:bins\r\nSUMMARY:Bins (late)\r\nRECURRENCE-ID;VALUE=DATE:20261008\r\n"
        "DTSTART;VALUE=DATE:20261009\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    const cal_window_t w = {day_of("2026-10-05") * 86400, day_of("2026-10-19") * 86400,
                            NULL, 0, false};
    cal_event_t out[8];
    size_t n;

    cal_test_run(ics, strlen(ics), strlen(ics), &w, out, 8, &n, NULL);

    static const cal_want_t want[] = {
        {"Bins (late)", 20735, 0, 20735, 0, true, 20735LL * 86400},
        {"Bins", 20741, 0, 20741, 0, true, 20741LL * 86400},
    };
    cal_test_expect("all-day override", out, n, want, 2);
}

void test_cal_rrule(void)
{
    SUITE("cal_rrule -- recurrence expansion");
    test_weekly_interval_2_on_mon_wed_fri();
    test_monthly_second_tuesday();
    test_monthly_last_friday();
    test_monthly_31st_skips_short_months();
    test_monthly_31st_from_dtstart_alone_skips_short_months();
    test_monthly_last_day_by_negative_monthday();
    test_monthly_friday_13th();
    test_last_weekday_of_month_by_setpos();
    test_yearly_last_sunday_of_march();
    test_yearly_all_day_birthday();
    test_yearly_feb_29_only_in_leap_years();
    test_count_stops_the_series();
    test_exdate_does_not_extend_count();
    test_count_is_honored_when_the_window_starts_late();
    test_until_utc_is_inclusive();
    test_until_date_on_all_day_series_is_inclusive();
    test_exdate_lists_dates_and_repeated_lines();
    test_rdate_adds_instances();
    test_wkst_changes_biweekly_weeks_rfc_example();
    test_unsupported_rule_yields_dtstart_only();
    test_daily_series_from_2010_reaches_the_window();
    test_instance_starting_before_window_and_running_into_it();
    test_all_day_override_by_date_moves_one_instance();
}
