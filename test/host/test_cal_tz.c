#include "cal_tz.h"
#include "test.h"

/* Expected values come from Python's zoneinfo for the matching tzdata zone
 * (Europe/Dublin, Europe/Berlin, America/New_York, Australia/Lord_Howe, Australia/Sydney,
 * Pacific/Auckland), not from the formulas under test. */

static void test_civil_dates_round_trip_known_days(void)
{
    CHECK_INT(cal_days_from_civil(1970, 1, 1), 0);
    CHECK_INT(cal_days_from_civil(1969, 12, 31), -1);
    CHECK_INT(cal_days_from_civil(2000, 3, 1), 11017);
    CHECK_INT(cal_days_from_civil(1601, 1, 1), -134774);
    CHECK_INT(cal_days_from_civil(2400, 2, 29), 157113);
    CHECK_INT(cal_days_from_civil(2026, 10, 19), 20745);

    int64_t y;
    unsigned m, d;
    cal_civil_from_days(-134774, &y, &m, &d);
    CHECK(y == 1601 && m == 1 && d == 1, "got %lld-%u-%u", (long long)y, m, d);
    cal_civil_from_days(157113, &y, &m, &d);
    CHECK(y == 2400 && m == 2 && d == 29, "got %lld-%u-%u", (long long)y, m, d);
}

static void test_weekday_and_month_length(void)
{
    CHECK_INT(cal_weekday(0), 4);         /* 1970-01-01, Thursday */
    CHECK_INT(cal_weekday(-1), 3);
    CHECK_INT(cal_weekday(20745), 1);     /* 2026-10-19, Monday */
    CHECK_INT(cal_days_in_month(2024, 2), 29);
    CHECK_INT(cal_days_in_month(2100, 2), 28);
    CHECK_INT(cal_days_in_month(2000, 2), 29);
    CHECK_INT(cal_days_in_month(2026, 9), 30);
}

static void test_posix_offsets_either_side_of_2026_transitions(void)
{
    static const struct {
        const char *tz;
        int64_t utc;
        int32_t offset;
    } cases[] = {
        {"UTC0", 1782903600LL, 0},
        {"GMT0IST,M3.5.0/1,M10.5.0", 1774745999LL, 0},
        {"GMT0IST,M3.5.0/1,M10.5.0", 1774746000LL, 3600},
        {"GMT0IST,M3.5.0/1,M10.5.0", 1792889999LL, 3600},
        {"GMT0IST,M3.5.0/1,M10.5.0", 1792890000LL, 0},
        /* tzdata's own Dublin string: "standard" is summer, DST is winter. */
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 1774745999LL, 0},
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 1774746000LL, 3600},
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 1792889999LL, 3600},
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 1792890000LL, 0},
        {"CET-1CEST,M3.5.0,M10.5.0/3", 1774745999LL, 3600},
        {"CET-1CEST,M3.5.0,M10.5.0/3", 1774746000LL, 7200},
        {"CET-1CEST,M3.5.0,M10.5.0/3", 1792889999LL, 7200},
        {"CET-1CEST,M3.5.0,M10.5.0/3", 1792890000LL, 3600},
        {"EST5EDT,M3.2.0,M11.1.0", 1772953199LL, -18000},
        {"EST5EDT,M3.2.0,M11.1.0", 1772953200LL, -14400},
        {"EST5EDT,M3.2.0,M11.1.0", 1793512799LL, -14400},
        {"EST5EDT,M3.2.0,M11.1.0", 1793512800LL, -18000},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 1775314799LL, 39600},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 1775314800LL, 37800},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 1791041399LL, 37800},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 1791041400LL, 39600},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 1775318399LL, 39600},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 1775318400LL, 36000},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 1791043199LL, 36000},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 1791043200LL, 39600},
        {"NZST-12NZDT,M9.5.0,M4.1.0/3", 1775311199LL, 46800},
        {"NZST-12NZDT,M9.5.0,M4.1.0/3", 1775311200LL, 43200},
        {"NZST-12NZDT,M9.5.0,M4.1.0/3", 1790431199LL, 43200},
        {"NZST-12NZDT,M9.5.0,M4.1.0/3", 1790431200LL, 46800},
        /* mid-January, inside a southern DST that spans the new year */
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 1768435200LL, 39600},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cal_tz_t tz;
        CHECK(cal_tz_parse_posix(cases[i].tz, &tz) == 0, "%s rejected", cases[i].tz);
        const int32_t got = cal_tz_offset_at_utc(&tz, cases[i].utc);
        CHECK(got == cases[i].offset, "%s at %lld: got %d, want %d", cases[i].tz,
              (long long)cases[i].utc, (int)got, (int)cases[i].offset);
    }
}

static void test_local_to_utc_gap_moves_forward_and_overlap_takes_first(void)
{
    static const struct {
        const char *tz;
        int y, mo, d, h, mi;
        int64_t want;
        const char *what;
    } cases[] = {
        {"GMT0IST,M3.5.0/1,M10.5.0", 2026, 7, 1, 12, 0, 1782903600LL, "Dublin summer"},
        {"GMT0IST,M3.5.0/1,M10.5.0", 2026, 3, 29, 1, 30, 1774747800LL, "Dublin gap"},
        {"GMT0IST,M3.5.0/1,M10.5.0", 2026, 10, 25, 1, 30, 1792888200LL, "Dublin overlap"},
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 2026, 3, 29, 1, 30, 1774747800LL, "Dublin gap, tzdata form"},
        {"IST-1GMT0,M10.5.0,M3.5.0/1", 2026, 10, 25, 1, 30, 1792888200LL,
         "Dublin overlap, tzdata form"},
        {"EST5EDT,M3.2.0,M11.1.0", 2026, 3, 8, 2, 30, 1772955000LL, "New York gap"},
        {"EST5EDT,M3.2.0,M11.1.0", 2026, 11, 1, 1, 30, 1793511000LL, "New York overlap"},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 2026, 10, 4, 2, 30, 1791045000LL, "Sydney gap"},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 2026, 4, 5, 2, 30, 1775316600LL, "Sydney overlap"},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 2026, 10, 4, 2, 15, 1791042300LL,
         "Lord Howe 30-minute gap"},
        {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", 2026, 4, 5, 1, 45, 1775313900LL,
         "Lord Howe 30-minute overlap"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cal_tz_t tz;
        CHECK(cal_tz_parse_posix(cases[i].tz, &tz) == 0, "%s rejected", cases[i].tz);
        const int64_t local = cal_days_from_civil(cases[i].y, (unsigned)cases[i].mo,
                                                  (unsigned)cases[i].d) * 86400 +
                              cases[i].h * 3600 + cases[i].mi * 60;
        const int64_t got = cal_tz_local_to_utc(&tz, local);
        CHECK(got == cases[i].want, "%s: got %lld, want %lld", cases[i].what,
              (long long)got, (long long)cases[i].want);
    }
}

static void test_posix_parse_fields_and_sign_inversion(void)
{
    cal_tz_t tz;
    CHECK_INT(cal_tz_parse_posix("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", &tz), 0);
    CHECK_INT(tz.std_offset, 37800);
    CHECK_INT(tz.dst_offset, 39600);
    CHECK_INT(tz.dst_start.month, 10);
    CHECK_INT(tz.dst_start.week, 1);
    CHECK_INT(tz.dst_start.weekday, 0);
    CHECK_INT(tz.dst_start.time, 7200);

    CHECK_INT(cal_tz_parse_posix("CET-1CEST,M3.5.0,M10.5.0/3", &tz), 0);
    CHECK_INT(tz.std_offset, 3600);
    CHECK_INT(tz.dst_offset, 7200);
    CHECK_INT(tz.dst_end.week, 5);
    CHECK_INT(tz.dst_end.time, 10800);

    CHECK_INT(cal_tz_parse_posix("UTC0", &tz), 0);
    CHECK(!tz.has_dst, "UTC0 has DST");
}

static void test_posix_rejects_unsupported_forms(void)
{
    static const char *const bad[] = {
        "", "EST", "EST5EDT", "EST5EDT,J60,J300", "EST5EDT,60,300", "X1",
        "EST5EDT,M3.2.0", "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0",
        "CET-1CEST,M3.5.0,M10.5.0/3x",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cal_tz_t tz;
        CHECK(cal_tz_parse_posix(bad[i], &tz) == -1, "accepted \"%s\"", bad[i]);
    }
}

void test_cal_tz(void)
{
    SUITE("cal_tz -- civil dates and POSIX zones");
    test_civil_dates_round_trip_known_days();
    test_weekday_and_month_length();
    test_posix_offsets_either_side_of_2026_transitions();
    test_local_to_utc_gap_moves_forward_and_overlap_takes_first();
    test_posix_parse_fields_and_sign_inversion();
    test_posix_rejects_unsupported_forms();
}
