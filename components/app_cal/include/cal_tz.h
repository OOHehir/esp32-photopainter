/* cal_tz -- civil-date arithmetic and a rule-based time zone.
 *
 * Pure C, no libc time API: mktime/localtime read the process TZ, which the device does
 * not have and the host tests must not depend on. A zone here is one standard offset and
 * at most one yearly DST pair, which is what both a POSIX TZ string and the current rules
 * of an iCalendar VTIMEZONE describe. Historical rule changes are not modeled.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One yearly transition. Either the nth weekday of a month (POSIX
 * Mm.w.d), the first given weekday on or after a day of the month, or a fixed day. */
typedef struct {
    uint8_t month;    /* 1..12 */
    uint8_t week;     /* 1..5 (5 = last) when weekday >= 0; 0 = use mday */
    int8_t  weekday;  /* 0 = Sunday..6; -1 = exactly mday */
    uint8_t mday;     /* week == 0: weekday on or after mday, or mday itself */
    int32_t time;     /* seconds after midnight, wall clock before the
                       * transition; POSIX allows -167h..167h */
} cal_tz_rule_t;

typedef struct {
    int32_t std_offset;      /* seconds east of UTC (POSIX sign inverted) */
    int32_t dst_offset;      /* seconds east of UTC; meaningful if has_dst */
    bool    has_dst;
    cal_tz_rule_t dst_start; /* time is in standard wall clock */
    cal_tz_rule_t dst_end;   /* time is in DST wall clock */
} cal_tz_t;

/**
 * @brief Days since 1970-01-01 for a proleptic Gregorian date.
 * @param y  Year; any value, including negative.
 * @param m  Month 1..12.
 * @param d  Day 1..31; not range-checked against the month.
 * @return Day number, negative before 1970.
 */
int64_t cal_days_from_civil(int64_t y, unsigned m, unsigned d);

/**
 * @brief Inverse of cal_days_from_civil().
 * @param days    Days since 1970-01-01.
 * @param[out] y  Year.
 * @param[out] m  Month 1..12.
 * @param[out] d  Day 1..31.
 */
void cal_civil_from_days(int64_t days, int64_t *y, unsigned *m, unsigned *d);

/**
 * @brief Day of the week.
 * @param days  Days since 1970-01-01.
 * @return 0 = Sunday .. 6 = Saturday.
 */
int cal_weekday(int64_t days);

/**
 * @brief Number of days in a month.
 * @param y  Year.
 * @param m  Month 1..12.
 * @return 28..31.
 */
int cal_days_in_month(int64_t y, unsigned m);

/**
 * @brief Day on which a transition rule falls in a year.
 * @param rule  The rule.
 * @param year  Year.
 * @return Days since 1970-01-01.
 */
int64_t cal_tz_rule_day(const cal_tz_rule_t *rule, int64_t year);

/**
 * @brief A zone with a fixed offset and no DST.
 * @param[out] out     Zone to fill.
 * @param offset_secs  Seconds east of UTC.
 */
void cal_tz_fixed(cal_tz_t *out, int32_t offset_secs);

/**
 * @brief Parse a POSIX TZ string such as "CET-1CEST,M3.5.0,M10.5.0/3".
 *
 * Accepts alphabetic or <quoted> names, offsets [+-]hh[:mm[:ss]], an optional DST
 * offset (default one hour ahead of standard) and Mm.w.d[/time] rules (default time
 * 02:00). Jn and n rule forms, and a DST name without rules, are rejected.
 *
 * @param s         NUL-terminated TZ string.
 * @param[out] out  Written only on success.
 * @return 0, or -1 if the string is malformed or uses an unsupported form.
 */
int cal_tz_parse_posix(const char *s, cal_tz_t *out);

/**
 * @brief Offset in effect at an instant.
 * @param tz   Zone.
 * @param utc  Seconds since the epoch.
 * @return Seconds east of UTC.
 */
int32_t cal_tz_offset_at_utc(const cal_tz_t *tz, int64_t utc);

/**
 * @brief Convert a wall-clock time to an instant.
 *
 * A wall time skipped by a forward transition (the spring gap) is read with the offset
 * in effect before the gap, so it lands that far after the transition: 01:30 in a
 * 01:00->02:00 gap becomes 02:30. A wall time that occurs twice (the autumn overlap)
 * resolves to the earlier instant, the one before the transition.
 *
 * @param tz     Zone.
 * @param local  Wall-clock seconds since 1970-01-01T00:00 (no offset applied).
 * @return Seconds since the epoch.
 */
int64_t cal_tz_local_to_utc(const cal_tz_t *tz, int64_t local);

#ifdef __cplusplus
}
#endif
