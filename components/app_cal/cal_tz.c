#include "cal_tz.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Howard Hinnant's days_from_civil / civil_from_days, valid for the whole
 * int64 range that matters here and free of any table or loop. */
int64_t cal_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void cal_civil_from_days(int64_t days, int64_t *y, unsigned *m, unsigned *d)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

int cal_weekday(int64_t days)
{
    /* 1970-01-01 was a Thursday (4). */
    const int64_t w = (days + 4) % 7;
    return (int)(w < 0 ? w + 7 : w);
}

int cal_days_in_month(int64_t y, unsigned m)
{
    static const unsigned char dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return dim[(m - 1) % 12];
}

int64_t cal_tz_rule_day(const cal_tz_rule_t *r, int64_t year)
{
    const int dim = cal_days_in_month(year, r->month);
    if (r->weekday < 0) {
        const int md = r->mday > dim ? dim : r->mday;
        return cal_days_from_civil(year, r->month, (unsigned)md);
    }
    if (r->week == 5) {
        const int64_t last = cal_days_from_civil(year, r->month, (unsigned)dim);
        return last - ((cal_weekday(last) - r->weekday + 7) % 7);
    }
    const unsigned from = r->week ? 1u + 7u * (r->week - 1u) : (r->mday ? r->mday : 1u);
    const int64_t first = cal_days_from_civil(year, r->month, from);
    return first + ((r->weekday - cal_weekday(first) + 7) % 7);
}

void cal_tz_fixed(cal_tz_t *out, int32_t offset_secs)
{
    memset(out, 0, sizeof(*out));
    out->std_offset = offset_secs;
    out->dst_offset = offset_secs;
}

/* [+-]hh[:mm[:ss]] -> seconds; hours up to 167 as POSIX allows for rule
 * times. Returns NULL on a malformed field. */
static const char *parse_hms(const char *s, int32_t *out)
{
    int sign = 1;
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    if (!isdigit((unsigned char)*s)) {
        return NULL;
    }
    int32_t part[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            if (*s != ':') {
                break;
            }
            s++;
            if (!isdigit((unsigned char)*s)) {
                return NULL;
            }
        }
        int n = 0;
        int v = 0;
        while (isdigit((unsigned char)*s) && n < 3) {
            v = v * 10 + (*s++ - '0');
            n++;
        }
        part[i] = v;
    }
    if (part[0] > 167 || part[1] > 59 || part[2] > 59) {
        return NULL;
    }
    *out = sign * (part[0] * 3600 + part[1] * 60 + part[2]);
    return s;
}

static const char *parse_name(const char *s)
{
    if (*s == '<') {
        const char *e = strchr(s, '>');
        return (e == NULL || e - s < 4) ? NULL : e + 1;
    }
    const char *b = s;
    while (isalpha((unsigned char)*s)) {
        s++;
    }
    return s - b < 3 ? NULL : s;
}

static int parse_int(const char **sp, int lo, int hi)
{
    const char *s = *sp;
    if (!isdigit((unsigned char)*s)) {
        return -1;
    }
    int v = 0;
    while (isdigit((unsigned char)*s) && v <= hi) {
        v = v * 10 + (*s++ - '0');
    }
    *sp = s;
    return (v < lo || v > hi) ? -1 : v;
}

static const char *parse_rule(const char *s, cal_tz_rule_t *r)
{
    if (*s++ != 'M') {
        return NULL;
    }
    const int m = parse_int(&s, 1, 12);
    if (m < 0 || *s++ != '.') {
        return NULL;
    }
    const int w = parse_int(&s, 1, 5);
    if (w < 0 || *s++ != '.') {
        return NULL;
    }
    const int d = parse_int(&s, 0, 6);
    if (d < 0) {
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->month = (uint8_t)m;
    r->week = (uint8_t)w;
    r->weekday = (int8_t)d;
    r->time = 2 * 3600;
    if (*s == '/') {
        s = parse_hms(s + 1, &r->time);
    }
    return s;
}

int cal_tz_parse_posix(const char *s, cal_tz_t *out)
{
    if (s == NULL) {
        return -1;
    }
    cal_tz_t tz;
    memset(&tz, 0, sizeof(tz));
    int32_t off;
    if ((s = parse_name(s)) == NULL || (s = parse_hms(s, &off)) == NULL) {
        return -1;
    }
    tz.std_offset = -off;
    tz.dst_offset = tz.std_offset;
    if (*s == '\0') {
        *out = tz;
        return 0;
    }
    if ((s = parse_name(s)) == NULL) {
        return -1;
    }
    tz.dst_offset = tz.std_offset + 3600;
    if (*s != ',' && *s != '\0') {
        if ((s = parse_hms(s, &off)) == NULL) {
            return -1;
        }
        tz.dst_offset = -off;
    }
    if (*s++ != ',' || (s = parse_rule(s, &tz.dst_start)) == NULL || *s++ != ',' ||
        (s = parse_rule(s, &tz.dst_end)) == NULL || *s != '\0') {
        return -1;
    }
    tz.has_dst = true;
    *out = tz;
    return 0;
}

static int64_t year_of_utc(int64_t utc)
{
    int64_t days = utc / 86400 - (utc % 86400 < 0);
    int64_t y;
    unsigned m, d;
    cal_civil_from_days(days, &y, &m, &d);
    return y;
}

int32_t cal_tz_offset_at_utc(const cal_tz_t *tz, int64_t utc)
{
    if (!tz->has_dst) {
        return tz->std_offset;
    }
    const int64_t y = year_of_utc(utc + tz->std_offset);
    const int64_t on = cal_tz_rule_day(&tz->dst_start, y) * 86400 + tz->dst_start.time -
                       tz->std_offset;
    const int64_t off = cal_tz_rule_day(&tz->dst_end, y) * 86400 + tz->dst_end.time -
                        tz->dst_offset;
    /* on > off is the southern hemisphere (and Ireland's tzdata form,
     * where "DST" is winter): DST spans the new year. */
    const bool dst = on < off ? (utc >= on && utc < off) : (utc >= on || utc < off);
    return dst ? tz->dst_offset : tz->std_offset;
}

int64_t cal_tz_local_to_utc(const cal_tz_t *tz, int64_t local)
{
    if (!tz->has_dst) {
        return local - tz->std_offset;
    }
    const int64_t a = local - tz->std_offset;
    const int64_t b = local - tz->dst_offset;
    const bool a_ok = cal_tz_offset_at_utc(tz, a) == tz->std_offset;
    const bool b_ok = cal_tz_offset_at_utc(tz, b) == tz->dst_offset;
    if (a_ok != b_ok) {
        return a_ok ? a : b;
    }
    /* Both valid is the overlap: take the earlier instant. Neither is the
     * gap: the later candidate is the wall time read with the pre-gap offset. */
    if (a_ok) {
        return a < b ? a : b;
    }
    return a > b ? a : b;
}
