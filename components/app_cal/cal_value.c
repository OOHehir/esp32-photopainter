#include "cal_value.h"

#include <ctype.h>

#include "cal_tz.h"

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool cal_value_datetime(const char *s, size_t len, int64_t *wall, bool *is_date,
                        bool *is_utc)
{
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (len < 8 || !digits(s, 4, &y) || !digits(s + 4, 2, &mo) || !digits(s + 6, 2, &d)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > cal_days_in_month(y, (unsigned)mo)) {
        return false;
    }
    *is_utc = false;
    *is_date = len == 8;
    if (!*is_date) {
        if ((len != 15 && len != 16) || s[8] != 'T' || !digits(s + 9, 2, &h) ||
            !digits(s + 11, 2, &mi) || !digits(s + 13, 2, &se)) {
            return false;
        }
        if (len == 16) {
            if (s[15] != 'Z') {
                return false;
            }
            *is_utc = true;
        }
        /* 60 is a leap second, which RFC 5545 permits; it rolls over. */
        if (h > 23 || mi > 59 || se > 60) {
            return false;
        }
    }
    *wall = cal_days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

bool cal_value_duration(const char *s, int32_t *days, int32_t *secs)
{
    int sign = 1;
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    if (*s++ != 'P') {
        return false;
    }
    int32_t dd = 0, ss = 0;
    bool in_time = false, any = false;
    while (*s) {
        if (*s == 'T') {
            in_time = true;
            s++;
            continue;
        }
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
        int32_t v = 0;
        while (isdigit((unsigned char)*s)) {
            if (v > 100000) {
                return false;
            }
            v = v * 10 + (*s++ - '0');
        }
        const char u = *s++;
        if (!in_time && u == 'W') {
            dd += 7 * v;
        } else if (!in_time && u == 'D') {
            dd += v;
        } else if (in_time && u == 'H') {
            ss += 3600 * v;
        } else if (in_time && u == 'M') {
            ss += 60 * v;
        } else if (in_time && u == 'S') {
            ss += v;
        } else {
            return false;
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    *days = sign * dd;
    *secs = sign * ss;
    return true;
}

bool cal_value_utc_offset(const char *s, int32_t *secs)
{
    if (*s != '+' && *s != '-') {
        return false;
    }
    const int sign = *s == '-' ? -1 : 1;
    int h, m, se = 0;
    if (!digits(s + 1, 2, &h) || !digits(s + 3, 2, &m)) {
        return false;
    }
    if (s[5] != '\0' && (!digits(s + 5, 2, &se) || s[7] != '\0')) {
        return false;
    }
    *secs = sign * (h * 3600 + m * 60 + se);
    return true;
}
