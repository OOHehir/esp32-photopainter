#include "cal_rrule.h"

#include <stdlib.h>
#include <string.h>

#include "cal_tz.h"
#include "cal_value.h"

static const char WD_NAMES[7][3] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

static int weekday_code(const char *s)
{
    for (int i = 0; i < 7; i++) {
        if (s[0] == WD_NAMES[i][0] && s[1] == WD_NAMES[i][1]) {
            return i;
        }
    }
    return -1;
}

/* Signed decimal at *sp, advancing it. Rejects an empty number and
 * magnitudes above lim. */
static bool read_int(const char **sp, long lim, long *out)
{
    const char *s = *sp;
    long sign = 1;
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        if (v > lim) {
            return false;
        }
    }
    *sp = s;
    *out = sign * v;
    return true;
}

static bool parse_part(cal_rrule_t *r, const char *key, size_t klen, const char *v)
{
#define KEY(k) (klen == sizeof(k) - 1 && memcmp(key, k, klen) == 0)
    long n;
    if (KEY("FREQ")) {
        static const char *const f[] = {"DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
        for (int i = 0; i < 4; i++) {
            if (strcmp(v, f[i]) == 0) {
                r->freq = (uint8_t)(CAL_FREQ_DAILY + i);
                return true;
            }
        }
        return false;
    }
    if (KEY("INTERVAL")) {
        if (!read_int(&v, 100000, &n) || n < 1 || *v) {
            return false;
        }
        r->interval = (uint32_t)n;
        return true;
    }
    if (KEY("COUNT")) {
        if (!read_int(&v, 1000000, &n) || n < 1 || *v) {
            return false;
        }
        r->count = (uint32_t)n;
        return true;
    }
    if (KEY("UNTIL")) {
        r->has_until = cal_value_datetime(v, strlen(v), &r->until, &r->until_is_date,
                                          &r->until_is_utc);
        return r->has_until;
    }
    if (KEY("WKST")) {
        const int w = weekday_code(v);
        if (w < 0 || v[2]) {
            return false;
        }
        r->wkst = (uint8_t)w;
        return true;
    }
    /* The list parts below share one loop: an item, then ',' or the end. */
    const bool byday = KEY("BYDAY"), bymd = KEY("BYMONTHDAY"), bym = KEY("BYMONTH"),
               bysp = KEY("BYSETPOS");
    if (!byday && !bymd && !bym && !bysp) {
        return false;
    }
    while (*v) {
        if (byday) {
            n = 0;
            if ((*v == '+' || *v == '-' || (*v >= '0' && *v <= '9')) &&
                (!read_int(&v, 53, &n) || n == 0)) {
                return false;
            }
            const int w = weekday_code(v);
            if (w < 0 || r->n_byday >= CAL_RRULE_MAX_BYDAY) {
                return false;
            }
            r->byday[r->n_byday++] = (cal_byday_t){(int8_t)n, (uint8_t)w};
            v += 2;
        } else {
            if (!read_int(&v, 366, &n) || n == 0) {
                return false;
            }
            if (bym) {
                if (n < 1 || n > 12) {
                    return false;
                }
                r->bymonth |= (uint16_t)(1u << n);
            } else if (bymd) {
                if (n > 31 || n < -31 || r->n_bymonthday >= CAL_RRULE_MAX_MONTHDAY) {
                    return false;
                }
                r->bymonthday[r->n_bymonthday++] = (int8_t)n;
            } else {
                if (r->n_bysetpos >= CAL_RRULE_MAX_SETPOS) {
                    return false;
                }
                r->bysetpos[r->n_bysetpos++] = (int16_t)n;
            }
        }
        if (*v == ',') {
            v++;
        } else if (*v) {
            return false;
        }
    }
    return true;
#undef KEY
}

void cal_rrule_parse(const char *s, cal_rrule_t *r)
{
    memset(r, 0, sizeof(*r));
    r->interval = 1;
    r->wkst = 1;
    char part[256];
    while (*s) {
        const char *end = strchr(s, ';');
        const size_t len = end ? (size_t)(end - s) : strlen(s);
        const char *eq = memchr(s, '=', len);
        if (eq != NULL && len < sizeof(part)) {
            memcpy(part, eq + 1, len - (size_t)(eq - s) - 1);
            part[len - (size_t)(eq - s) - 1] = '\0';
            if (!parse_part(r, s, (size_t)(eq - s), part)) {
                r->unsupported = true;
            }
        } else if (len > 0) {
            r->unsupported = true;
        }
        s += len + (end != NULL);
    }
    if (r->freq == CAL_FREQ_NONE) {
        r->unsupported = true;
    }
    /* RFC 5545 section 3.3.10 forbids an ordinal BYDAY outside MONTHLY and
     * YEARLY, and BYMONTHDAY in WEEKLY; their meaning would be a guess. */
    if (r->freq == CAL_FREQ_DAILY || r->freq == CAL_FREQ_WEEKLY) {
        for (int i = 0; i < r->n_byday; i++) {
            if (r->byday[i].ord != 0) {
                r->unsupported = true;
            }
        }
    }
    if (r->freq == CAL_FREQ_WEEKLY && r->n_bymonthday) {
        r->unsupported = true;
    }
}

static int cmp_i32(const void *a, const void *b)
{
    const int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static void add(cal_rrule_iter_t *it, int64_t d)
{
    if (it->n < CAL_RRULE_PERIOD_MAX) {
        it->days[it->n++] = (int32_t)d;
    }
}

static bool month_ok(const cal_rrule_t *r, unsigned m)
{
    return r->bymonth == 0 || (r->bymonth >> m & 1u);
}

static bool weekday_ok(const cal_rrule_t *r, int64_t d)
{
    if (r->n_byday == 0) {
        return true;
    }
    const int w = cal_weekday(d);
    for (int i = 0; i < r->n_byday; i++) {
        if (r->byday[i].wd == w) {
            return true;
        }
    }
    return false;
}

static bool monthday_ok(const cal_rrule_t *r, int md, int dim)
{
    if (r->n_bymonthday == 0) {
        return true;
    }
    for (int i = 0; i < r->n_bymonthday; i++) {
        const int v = r->bymonthday[i];
        if ((v > 0 ? v : dim + v + 1) == md) {
            return true;
        }
    }
    return false;
}

/* BYDAY used as a filter on a known day of a month, ordinal included:
 * "BYMONTHDAY=13;BYDAY=FR" is Friday the 13th, "BYMONTHDAY=...;BYDAY=1MO" the first
 * Monday among those days. */
static bool byday_matches_in_month(const cal_rrule_t *r, int64_t d, int md, int dim)
{
    const int w = cal_weekday(d);
    for (int i = 0; i < r->n_byday; i++) {
        const cal_byday_t e = r->byday[i];
        if (e.wd != w) {
            continue;
        }
        if (e.ord == 0 || (e.ord > 0 && (md - 1) / 7 + 1 == e.ord) ||
            (e.ord < 0 && (dim - md) / 7 + 1 == -e.ord)) {
            return true;
        }
    }
    return false;
}

/* Every BYDAY entry within [first, first + len): a month or a year. */
static void expand_byday(cal_rrule_iter_t *it, int64_t first, int len)
{
    const cal_rrule_t *r = it->r;
    for (int i = 0; i < r->n_byday; i++) {
        const cal_byday_t e = r->byday[i];
        const int off = (e.wd - cal_weekday(first) + 7) % 7;
        if (off >= len) {
            continue;
        }
        const int cnt = (len - 1 - off) / 7 + 1;
        if (e.ord == 0) {
            for (int j = 0; j < cnt; j++) {
                add(it, first + off + 7 * j);
            }
        } else if (e.ord > 0 && e.ord <= cnt) {
            add(it, first + off + 7 * (e.ord - 1));
        } else if (e.ord < 0 && -e.ord <= cnt) {
            add(it, first + off + 7 * (cnt + e.ord));
        }
    }
}

static void expand_month(cal_rrule_iter_t *it, int64_t y, unsigned m)
{
    const cal_rrule_t *r = it->r;
    const int dim = cal_days_in_month(y, m);
    const int64_t first = cal_days_from_civil(y, m, 1);
    if (r->n_bymonthday) {
        for (int i = 0; i < r->n_bymonthday; i++) {
            const int v = r->bymonthday[i];
            const int md = v > 0 ? v : dim + v + 1;
            /* Skipped, not clamped: BYMONTHDAY=31 has no instance in a
             * 30-day month (RFC 5545 section 3.3.10). */
            if (md < 1 || md > dim) {
                continue;
            }
            if (r->n_byday && !byday_matches_in_month(r, first + md - 1, md, dim)) {
                continue;
            }
            add(it, first + md - 1);
        }
    } else if (r->n_byday) {
        expand_byday(it, first, dim);
    } else if (it->dtstart_mday <= dim) {
        add(it, first + it->dtstart_mday - 1);
    }
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

static int64_t period_start(const cal_rrule_iter_t *it, int64_t k)
{
    const int64_t step = (int64_t)it->r->interval * k;
    switch (it->r->freq) {
    case CAL_FREQ_DAILY:
        return it->base + step;
    case CAL_FREQ_WEEKLY:
        return it->base + 7 * step;
    case CAL_FREQ_MONTHLY: {
        const int64_t mi = it->base + step;
        return cal_days_from_civil(floor_div(mi, 12), (unsigned)(mi - 12 * floor_div(mi, 12)) + 1, 1);
    }
    default:
        return cal_days_from_civil(it->base + step, 1, 1);
    }
}

static void fill_period(cal_rrule_iter_t *it, int64_t start)
{
    const cal_rrule_t *r = it->r;
    it->n = 0;
    it->i = 0;
    int64_t y;
    unsigned m, d;
    cal_civil_from_days(start, &y, &m, &d);
    switch (r->freq) {
    case CAL_FREQ_DAILY:
        if (month_ok(r, m) && monthday_ok(r, (int)d, cal_days_in_month(y, m)) &&
            weekday_ok(r, start)) {
            add(it, start);
        }
        break;
    case CAL_FREQ_WEEKLY:
        for (int i = 0; i < 7; i++) {
            const int64_t day = start + i;
            const bool wd = r->n_byday ? weekday_ok(r, day) : cal_weekday(day) == it->dtstart_wd;
            cal_civil_from_days(day, &y, &m, &d);
            if (wd && month_ok(r, m)) {
                add(it, day);
            }
        }
        break;
    case CAL_FREQ_MONTHLY:
        if (month_ok(r, m)) {
            expand_month(it, y, m);
        }
        break;
    default:
        /* YEARLY. BYDAY alone counts ordinals through the year ("20MO");
         * with BYMONTH they count through each month. BYMONTHDAY without BYMONTH applies
         * to every month. With no BY parts, DTSTART's month and day, so a Feb 29
         * birthday falls only in leap years. */
        if (r->bymonth == 0 && r->n_bymonthday == 0 && r->n_byday) {
            const bool leap = cal_days_in_month(y, 2) == 29;
            expand_byday(it, start, leap ? 366 : 365);
            break;
        }
        for (unsigned mm = 1; mm <= 12; mm++) {
            const bool want = r->bymonth ? (r->bymonth >> mm & 1u)
                                         : (r->n_bymonthday ? true : mm == it->dtstart_month);
            if (want) {
                expand_month(it, y, mm);
            }
        }
        break;
    }
    qsort(it->days, (size_t)it->n, sizeof(it->days[0]), cmp_i32);
    int32_t u = 0;
    for (int32_t i = 0; i < it->n; i++) {
        if (u == 0 || it->days[i] != it->days[u - 1]) {
            it->days[u++] = it->days[i];
        }
    }
    it->n = u;
    if (r->n_bysetpos) {
        int32_t pick[CAL_RRULE_MAX_SETPOS];
        int32_t np = 0;
        for (int i = 0; i < r->n_bysetpos; i++) {
            const int p = r->bysetpos[i];
            const int idx = p > 0 ? p - 1 : it->n + p;
            if (idx >= 0 && idx < it->n) {
                pick[np++] = it->days[idx];
            }
        }
        qsort(pick, (size_t)np, sizeof(pick[0]), cmp_i32);
        it->n = 0;
        for (int32_t i = 0; i < np; i++) {
            if (it->n == 0 || pick[i] != it->days[it->n - 1]) {
                it->days[it->n++] = pick[i];
            }
        }
    }
}

void cal_rrule_iter_init(cal_rrule_iter_t *it, const cal_rrule_t *r, int64_t dtstart_day,
                         int64_t skip_to_day)
{
    memset(it, 0, sizeof(*it));
    it->r = r;
    it->dtstart_day = dtstart_day;
    int64_t y;
    unsigned m, d;
    cal_civil_from_days(dtstart_day, &y, &m, &d);
    it->dtstart_month = (uint8_t)m;
    it->dtstart_mday = (uint8_t)d;
    it->dtstart_wd = (uint8_t)cal_weekday(dtstart_day);

    int64_t span, skip_units;
    int64_t sy;
    unsigned sm, sd;
    switch (r->freq) {
    case CAL_FREQ_DAILY:
        it->base = dtstart_day;
        span = 1;
        skip_units = skip_to_day > dtstart_day ? skip_to_day - it->base : 0;
        break;
    case CAL_FREQ_WEEKLY:
        it->base = dtstart_day - (it->dtstart_wd - r->wkst + 7) % 7;
        span = 7;
        skip_units = skip_to_day > dtstart_day ? skip_to_day - it->base : 0;
        break;
    case CAL_FREQ_MONTHLY:
        it->base = y * 12 + (m - 1);
        span = 1;
        cal_civil_from_days(skip_to_day > dtstart_day ? skip_to_day : dtstart_day, &sy, &sm, &sd);
        skip_units = sy * 12 + (sm - 1) - it->base;
        break;
    default:
        it->base = y;
        span = 1;
        cal_civil_from_days(skip_to_day > dtstart_day ? skip_to_day : dtstart_day, &sy, &sm, &sd);
        skip_units = sy - it->base;
        break;
    }
    /* One period short of the target, so the period containing
     * skip_to_day is never jumped over whatever the rounding. */
    if (skip_to_day > dtstart_day) {
        const int64_t k = skip_units / (span * (int64_t)r->interval) - 1;
        it->k = k > 0 ? k : 0;
    }
}

bool cal_rrule_iter_next(cal_rrule_iter_t *it, int64_t limit, int64_t *day)
{
    for (;;) {
        while (it->i < it->n) {
            const int64_t d = it->days[it->i++];
            if (d > it->dtstart_day) {
                *day = d;
                return true;
            }
        }
        const int64_t start = period_start(it, it->k);
        if (start > limit) {
            return false;
        }
        it->k++;
        fill_period(it, start);
    }
}
