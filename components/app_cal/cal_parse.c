#include "cal_parse.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cal_rrule.h"
#include "cal_value.h"

#define DAY 86400
/* Slack between wall-clock and UTC when deciding at END:VEVENT whether an
 * event can reach the window: real offsets stay within +-14 h, and the zone may not be
 * known yet. */
#define NEAR_MARGIN (2 * DAY)
#define TZID_MAX 128
#define NO_TZID 0xFFFFu

enum { DT_DATE, DT_UTC, DT_FLOAT, DT_TZID };

typedef struct {
    int64_t wall;      /* wall clock; for DT_UTC this is the UTC instant */
    uint16_t tzid;     /* index into names[] for DT_TZID */
    uint8_t kind;
} dt_t;

typedef struct {
    dt_t *v;
    uint32_t n, cap;
} dt_list_t;

typedef struct {
    char *uid;
    char title[CAL_TITLE_MAX];
    bool has_title, has_start, has_end, has_dur, has_rrule, has_recurid, cancelled, private;
    uint8_t ink1;   /* COLOR as ink + 1; 0 = none */
    dt_t start, end, recurid;
    int32_t dur_days, dur_secs;
    cal_rrule_t rrule;
    dt_list_t exdates, rdates;
} ev_t;

typedef struct {
    bool present, has_dtstart, has_to, has_rrule;
    int64_t dtstart;
    int32_t to;
    cal_rrule_t rrule;
} tz_sub_t;

typedef struct {
    uint16_t tzid;
    cal_tz_t tz;
} zone_t;

enum { C_NONE, C_EVENT, C_TZ, C_TZSUB };

struct cal_parser {
    cal_window_t win;
    cal_tz_t local;
    cal_stats_t st;
    bool failed;

    char line[CAL_LINE_MAX];
    size_t len;
    bool trunc, eol;

    int comp;
    uint32_t skip;     /* depth inside components being ignored (VALARM, VTODO) */
    ev_t cur;
    uint16_t tz_id;
    tz_sub_t sub, tz_std, tz_dst;
    bool sub_is_dst;

    ev_t *evs;
    size_t n_evs, cap_evs;
    zone_t *zones;
    size_t n_zones, cap_zones;
    char **names;
    size_t n_names, cap_names;
};

/* Grow *v to hold at least need elements of size sz; false on OOM. */
static bool grow(void **v, size_t *cap, size_t need, size_t sz)
{
    if (need <= *cap) {
        return true;
    }
    size_t nc = *cap ? *cap * 2 : 8;
    while (nc < need) {
        nc *= 2;
    }
    void *nv = realloc(*v, nc * sz);
    if (nv == NULL) {
        return false;
    }
    *v = nv;
    *cap = nc;
    return true;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

static bool ieq(const char *a, size_t alen, const char *b)
{
    if (alen != strlen(b)) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        char c = a[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

/* Panel inks as RGB, for "#rrggbb" values and named colors alike. */
static const struct { uint8_t r, g, b; int8_t ink; } INKS[] = {
    {0, 0, 0, 0}, {230, 200, 0, 2}, {200, 30, 30, 3}, {30, 70, 200, 5}, {30, 140, 60, 6},
};

/* The CSS names a calendar app is likely to write, by hue family. A
 * name not here returns -1 and the calendar's own color is used. */
static const struct { const char *name; int8_t ink; } CSS_INKS[] = {
    {"black", 0}, {"dimgray", 0}, {"gray", 0}, {"grey", 0}, {"darkslategray", 0},
    {"yellow", 2}, {"gold", 2}, {"khaki", 2}, {"goldenrod", 2}, {"orange", 2},
    {"red", 3}, {"darkred", 3}, {"crimson", 3}, {"firebrick", 3}, {"tomato", 3},
    {"orangered", 3}, {"salmon", 3}, {"coral", 3}, {"maroon", 3}, {"deeppink", 3},
    {"hotpink", 3}, {"pink", 3}, {"magenta", 3}, {"fuchsia", 3},
    {"blue", 5}, {"navy", 5}, {"darkblue", 5}, {"mediumblue", 5}, {"royalblue", 5},
    {"dodgerblue", 5}, {"steelblue", 5}, {"cornflowerblue", 5}, {"skyblue", 5},
    {"deepskyblue", 5}, {"lightblue", 5}, {"purple", 5}, {"indigo", 5}, {"darkviolet", 5},
    {"blueviolet", 5}, {"slateblue", 5}, {"mediumpurple", 5}, {"orchid", 5}, {"violet", 5},
    {"green", 6}, {"darkgreen", 6}, {"lime", 6}, {"limegreen", 6}, {"forestgreen", 6},
    {"seagreen", 6}, {"mediumseagreen", 6}, {"olive", 6}, {"olivedrab", 6}, {"teal", 6},
    {"darkcyan", 6}, {"cyan", 6}, {"aqua", 6}, {"turquoise", 6}, {"lightgreen", 6},
    {"yellowgreen", 6}, {"springgreen", 6}, {"darkolivegreen", 6}, {"lightseagreen", 6},
};

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int cal_colour_to_ink(const char *value)
{
    if (value == NULL) return -1;
    size_t n = strlen(value);
    if (n == 7 && value[0] == '#') {
        int rgb[3];
        for (int i = 0; i < 3; i++) {
            int hi = hexval(value[1 + 2 * i]), lo = hexval(value[2 + 2 * i]);
            if (hi < 0 || lo < 0) return -1;
            rgb[i] = hi * 16 + lo;
        }
        long best = -1;
        int ink = -1;
        for (size_t i = 0; i < sizeof(INKS) / sizeof(INKS[0]); i++) {
            long dr = rgb[0] - INKS[i].r, dg = rgb[1] - INKS[i].g, db = rgb[2] - INKS[i].b;
            long d = dr * dr + dg * dg + db * db;
            if (best < 0 || d < best) { best = d; ink = INKS[i].ink; }
        }
        return ink;
    }
    for (size_t i = 0; i < sizeof(CSS_INKS) / sizeof(CSS_INKS[0]); i++) {
        const char *a = value, *b = CSS_INKS[i].name;
        while (*a && (*a | 0x20) == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return CSS_INKS[i].ink;
    }
    return -1;
}

static void ev_free(ev_t *e)
{
    free(e->uid);
    free(e->exdates.v);
    free(e->rdates.v);
    memset(e, 0, sizeof(*e));
}

static uint16_t intern(cal_parser_t *p, const char *s)
{
    for (size_t i = 0; i < p->n_names; i++) {
        if (strcmp(p->names[i], s) == 0) {
            return (uint16_t)i;
        }
    }
    if (p->n_names >= NO_TZID) {
        return NO_TZID;
    }
    char *c = malloc(strlen(s) + 1);
    if (c == NULL || !grow((void **)&p->names, &p->cap_names, p->n_names + 1, sizeof(char *))) {
        free(c);
        p->failed = true;
        return NO_TZID;
    }
    strcpy(c, s);
    p->names[p->n_names] = c;
    return (uint16_t)p->n_names++;
}

/* TEXT unescape (RFC 5545 section 3.3.11) into a one-line title, cut at a
 * UTF-8 boundary. A newline becomes a space because the panel shows one line. */
static void set_title(char *dst, const char *s)
{
    size_t n = 0;
    while (*s && n < CAL_TITLE_MAX - 1) {
        char c = *s++;
        if (c == '\\' && *s) {
            c = *s++;
            if (c == 'n' || c == 'N') {
                c = ' ';
            }
        }
        dst[n++] = c;
    }
    /* Drop a trailing sequence missing its continuation bytes, whether
     * this cut or the CAL_LINE_MAX cut made it. */
    size_t j = n;
    while (j > 0 && ((unsigned char)dst[j - 1] & 0xC0) == 0x80) {
        j--;
    }
    if (j > 0) {
        const unsigned char lead = (unsigned char)dst[j - 1];
        const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (j - 1) < need) {
            n = j - 1;
        }
    }
    dst[n] = '\0';
}

typedef struct {
    char tzid[TZID_MAX];
    bool has_tzid, value_period;
} params_t;

/* Splits "NAME;P=V;P2="q:v":value". Returns the value, or NULL when there
 * is no ':' outside quotes. */
static const char *split_line(char *line, size_t *name_len, params_t *pp)
{
    memset(pp, 0, sizeof(*pp));
    char *s = line;
    while (*s && *s != ';' && *s != ':') {
        s++;
    }
    *name_len = (size_t)(s - line);
    while (*s == ';') {
        const char *pn = ++s;
        while (*s && *s != '=' && *s != ':' && *s != ';') {
            s++;
        }
        const size_t pnl = (size_t)(s - pn);
        if (*s != '=') {
            continue;
        }
        s++;
        /* Only the first value of a list is kept; TZID and VALUE take one. */
        char val[TZID_MAX];
        size_t vl = 0;
        bool first = true;
        for (;;) {
            const char *vs;
            size_t len;
            if (*s == '"') {
                vs = ++s;
                while (*s && *s != '"') {
                    s++;
                }
                len = (size_t)(s - vs);
                if (*s == '"') {
                    s++;
                }
            } else {
                vs = s;
                while (*s && *s != ',' && *s != ';' && *s != ':') {
                    s++;
                }
                len = (size_t)(s - vs);
            }
            if (first) {
                vl = len < sizeof(val) - 1 ? len : sizeof(val) - 1;
                memcpy(val, vs, vl);
                val[vl] = '\0';
                first = false;
            }
            if (*s != ',') {
                break;
            }
            s++;
        }
        if (ieq(pn, pnl, "TZID")) {
            memcpy(pp->tzid, val, vl + 1);
            pp->has_tzid = vl > 0;
        } else if (ieq(pn, pnl, "VALUE")) {
            pp->value_period = ieq(val, vl, "PERIOD");
        }
    }
    return *s == ':' ? s + 1 : NULL;
}

static bool read_dt(cal_parser_t *p, const char *v, size_t len, const params_t *pp, dt_t *out)
{
    bool is_date, is_utc;
    if (!cal_value_datetime(v, len, &out->wall, &is_date, &is_utc)) {
        return false;
    }
    out->tzid = NO_TZID;
    if (is_date) {
        out->kind = DT_DATE;
    } else if (is_utc) {
        out->kind = DT_UTC;
    } else if (pp->has_tzid) {
        out->kind = DT_TZID;
        out->tzid = intern(p, pp->tzid);
        if (out->tzid == NO_TZID) {
            out->kind = DT_FLOAT;
        }
    } else {
        out->kind = DT_FLOAT;
    }
    return true;
}

/* EXDATE/RDATE lists. On a cut line the last item may be a fragment
 * ("2026011" or "20260115T09"), and a fragment that happens to parse as a DATE would
 * exclude the wrong day, so it is dropped. */
static void read_dt_list(cal_parser_t *p, char *v, const params_t *pp, dt_list_t *list,
                         bool cut, bool is_rdate)
{
    if (is_rdate && pp->value_period) {
        p->st.unsupported_rrule++;
        return;
    }
    if (cut) {
        char *c = strrchr(v, ',');
        if (c == NULL) {
            return;
        }
        *c = '\0';
    }
    while (*v) {
        char *e = strchr(v, ',');
        const size_t len = e ? (size_t)(e - v) : strlen(v);
        dt_t d;
        if (read_dt(p, v, len, pp, &d)) {
            size_t cap = list->cap;
            if (!grow((void **)&list->v, &cap, (size_t)list->n + 1, sizeof(dt_t))) {
                p->failed = true;
                return;
            }
            list->cap = (uint32_t)cap;
            list->v[list->n++] = d;
        } else if (is_rdate && strchr(v, '/') != NULL) {
            p->st.unsupported_rrule++;
        }
        v += len + (e != NULL);
    }
}

static void event_property(cal_parser_t *p, const char *name, size_t nl, char *v,
                           const params_t *pp, bool cut)
{
    ev_t *e = &p->cur;
    if (ieq(name, nl, "SUMMARY")) {
        set_title(e->title, v);
        e->has_title = e->title[0] != '\0';
        return;
    }
    if (ieq(name, nl, "EXDATE")) {
        read_dt_list(p, v, pp, &e->exdates, cut, false);
        return;
    }
    if (ieq(name, nl, "RDATE")) {
        read_dt_list(p, v, pp, &e->rdates, cut, true);
        return;
    }
    if (cut) {
        return;
    }
    if (ieq(name, nl, "DTSTART")) {
        e->has_start = read_dt(p, v, strlen(v), pp, &e->start);
    } else if (ieq(name, nl, "DTEND")) {
        e->has_end = read_dt(p, v, strlen(v), pp, &e->end);
    } else if (ieq(name, nl, "RECURRENCE-ID")) {
        e->has_recurid = read_dt(p, v, strlen(v), pp, &e->recurid);
    } else if (ieq(name, nl, "DURATION")) {
        e->has_dur = cal_value_duration(v, &e->dur_days, &e->dur_secs);
    } else if (ieq(name, nl, "RRULE") && !e->has_rrule) {
        cal_rrule_parse(v, &e->rrule);
        e->has_rrule = true;
    } else if (ieq(name, nl, "STATUS")) {
        e->cancelled = ieq(v, strlen(v), "CANCELLED");
    } else if (ieq(name, nl, "COLOR")) {
        int ink = cal_colour_to_ink(v);
        e->ink1 = ink >= 0 ? (uint8_t)(ink + 1) : 0;
        p->st.with_colour++;
    } else if (ieq(name, nl, "CLASS")) {
        e->private = ieq(v, strlen(v), "PRIVATE") || ieq(v, strlen(v), "CONFIDENTIAL");
    } else if (ieq(name, nl, "UID")) {
        free(e->uid);
        e->uid = malloc(strlen(v) + 1);
        if (e->uid == NULL) {
            p->failed = true;
            return;
        }
        strcpy(e->uid, v);
    }
}

/* Wall-clock span an event's instances can occupy, for the cheap keep/drop
 * decision at END:VEVENT. INT64_MAX when the rule has no UNTIL. */
static void wall_span(const ev_t *e, int64_t *lo, int64_t *hi)
{
    int64_t len = e->start.kind == DT_DATE ? DAY : 0;
    if (e->has_end) {
        len = e->end.wall - e->start.wall;
    } else if (e->has_dur) {
        len = (int64_t)e->dur_days * DAY + e->dur_secs;
    }
    if (len < 0) {
        len = 0;
    }
    *lo = e->start.wall;
    int64_t last = e->start.wall;
    if (e->has_rrule && !e->rrule.unsupported && !e->has_recurid) {
        last = e->rrule.has_until ? (e->rrule.until > last ? e->rrule.until : last) : INT64_MAX;
    }
    for (uint32_t i = 0; i < e->rdates.n; i++) {
        const int64_t w = e->rdates.v[i].wall;
        *lo = w < *lo ? w : *lo;
        last = w > last ? w : last;
    }
    *hi = last == INT64_MAX ? INT64_MAX : last + len + DAY;
}

static bool near_window(const cal_parser_t *p, int64_t lo, int64_t hi)
{
    return lo - NEAR_MARGIN < p->win.to_utc &&
           (hi == INT64_MAX || hi + NEAR_MARGIN > p->win.from_utc);
}

static void end_event(cal_parser_t *p)
{
    ev_t *e = &p->cur;
    p->st.vevents++;
    if (e->cancelled) {
        p->st.cancelled++;
    } else if (e->private && p->win.hide_private) {
        /* Hidden by the same path as a cancellation, so a private
         * override of a public series removes that one instance rather than showing
         * the series' original. */
        p->st.hidden_private++;
        e->cancelled = true;
    }
    if (e->has_rrule && e->rrule.unsupported) {
        p->st.unsupported_rrule++;
    }
    bool keep = e->has_start;
    if (keep && e->has_recurid) {
        int64_t lo, hi;
        wall_span(e, &lo, &hi);
        /* An override matters if its new time is near the window, or if
         * the instance it replaces was, since that instance must then be suppressed. */
        keep = near_window(p, e->recurid.wall, e->recurid.wall + 2 * DAY) ||
               (!e->cancelled && near_window(p, lo, hi));
    } else if (keep) {
        int64_t lo, hi;
        wall_span(e, &lo, &hi);
        keep = !e->cancelled && near_window(p, lo, hi);
    }
    if (keep) {
        if (!e->has_title) {
            strcpy(e->title, "(No title)");
        }
        if (!grow((void **)&p->evs, &p->cap_evs, p->n_evs + 1, sizeof(ev_t))) {
            p->failed = true;
            ev_free(e);
            return;
        }
        if (e->has_recurid) {
            p->st.overrides++;
        }
        p->evs[p->n_evs++] = *e;
        memset(e, 0, sizeof(*e));
    } else {
        ev_free(e);
    }
}

/* A STANDARD/DAYLIGHT RRULE as a cal_tz_rule_t. Handles BYMONTH with BYDAY
 * "-1SU"/"2SU", BYDAY with a BYMONTHDAY range ("SU" on 8..14), or a fixed day. */
static bool tz_rule(const tz_sub_t *s, cal_tz_rule_t *out)
{
    const cal_rrule_t *r = &s->rrule;
    if (!s->has_rrule || r->unsupported || r->freq != CAL_FREQ_YEARLY || r->n_byday > 1) {
        return false;
    }
    int64_t y;
    unsigned m, d;
    cal_civil_from_days(floor_div(s->dtstart, DAY), &y, &m, &d);
    memset(out, 0, sizeof(*out));
    out->time = (int32_t)(s->dtstart - floor_div(s->dtstart, DAY) * DAY);
    out->month = (uint8_t)m;
    if (r->bymonth) {
        if (r->bymonth & (r->bymonth - 1)) {
            return false;
        }
        for (unsigned i = 1; i <= 12; i++) {
            if (r->bymonth >> i & 1u) {
                out->month = (uint8_t)i;
            }
        }
    }
    if (r->n_byday == 1 && r->byday[0].ord != 0) {
        const int o = r->byday[0].ord;
        if (r->n_bymonthday || o < -1 || o > 4) {
            return false;
        }
        out->week = (uint8_t)(o == -1 ? 5 : o);
        out->weekday = (int8_t)r->byday[0].wd;
    } else if (r->n_byday == 1) {
        int lo = 32;
        for (int i = 0; i < r->n_bymonthday; i++) {
            if (r->bymonthday[i] > 0 && r->bymonthday[i] < lo) {
                lo = r->bymonthday[i];
            }
        }
        if (lo == 32) {
            return false;
        }
        out->weekday = (int8_t)r->byday[0].wd;
        out->mday = (uint8_t)lo;
    } else {
        if (r->n_bymonthday > 1 || (r->n_bymonthday == 1 && r->bymonthday[0] < 1)) {
            return false;
        }
        out->weekday = -1;
        out->mday = (uint8_t)(r->n_bymonthday ? r->bymonthday[0] : (int)d);
    }
    return true;
}

static void end_vtimezone(cal_parser_t *p)
{
    const tz_sub_t *s = &p->tz_std, *dl = &p->tz_dst;
    if (p->tz_id == NO_TZID || (!s->present && !dl->present)) {
        return;
    }
    cal_tz_t tz;
    cal_tz_rule_t on, off;
    if (s->present && dl->present && tz_rule(dl, &on) && tz_rule(s, &off)) {
        cal_tz_fixed(&tz, s->to);
        tz.dst_offset = dl->to;
        tz.has_dst = true;
        tz.dst_start = on;
        tz.dst_end = off;
    } else {
        /* No usable yearly pair: whichever block took effect last. */
        const tz_sub_t *last = !dl->present ? s : !s->present ? dl
                             : (dl->dtstart > s->dtstart ? dl : s);
        cal_tz_fixed(&tz, last->to);
    }
    for (size_t i = 0; i < p->n_zones; i++) {
        if (p->zones[i].tzid == p->tz_id) {
            return;
        }
    }
    if (!grow((void **)&p->zones, &p->cap_zones, p->n_zones + 1, sizeof(zone_t))) {
        p->failed = true;
        return;
    }
    p->zones[p->n_zones++] = (zone_t){p->tz_id, tz};
}

static void end_tz_sub(cal_parser_t *p)
{
    tz_sub_t *best = p->sub_is_dst ? &p->tz_dst : &p->tz_std;
    if (!p->sub.has_to) {
        return;
    }
    p->sub.present = true;
    if (!best->present || p->sub.dtstart > best->dtstart) {
        *best = p->sub;
    }
}

static void tz_property(cal_parser_t *p, const char *name, size_t nl, char *v)
{
    if (p->comp == C_TZ) {
        if (ieq(name, nl, "TZID")) {
            /* Cut as split_line() cuts a TZID parameter, so the two match. */
            if (strlen(v) >= TZID_MAX) {
                v[TZID_MAX - 1] = '\0';
            }
            p->tz_id = intern(p, v);
        }
        return;
    }
    tz_sub_t *s = &p->sub;
    bool is_date, is_utc;
    if (ieq(name, nl, "DTSTART")) {
        s->has_dtstart = cal_value_datetime(v, strlen(v), &s->dtstart, &is_date, &is_utc);
    } else if (ieq(name, nl, "TZOFFSETTO")) {
        s->has_to = cal_value_utc_offset(v, &s->to);
    } else if (ieq(name, nl, "RRULE")) {
        cal_rrule_parse(v, &s->rrule);
        s->has_rrule = true;
    }
}

static void begin_end(cal_parser_t *p, bool begin, const char *v)
{
    const size_t vl = strlen(v);
    if (p->skip > 0) {
        /* An END for the enclosing component closes any ignored children
         * left open, so a VALARM missing its END cannot swallow the rest of the feed. */
        const bool closes_outer = !begin && ((p->comp == C_EVENT && ieq(v, vl, "VEVENT")) ||
                                             (p->comp == C_TZ && ieq(v, vl, "VTIMEZONE")));
        if (!closes_outer) {
            p->skip += begin ? 1 : -1;
            return;
        }
        p->skip = 0;
    }
    switch (p->comp) {
    case C_NONE:
        if (begin && ieq(v, vl, "VEVENT")) {
            ev_free(&p->cur);
            p->comp = C_EVENT;
        } else if (begin && ieq(v, vl, "VTIMEZONE")) {
            p->comp = C_TZ;
            p->tz_id = NO_TZID;
            memset(&p->tz_std, 0, sizeof(p->tz_std));
            memset(&p->tz_dst, 0, sizeof(p->tz_dst));
        } else if (begin && !ieq(v, vl, "VCALENDAR")) {
            p->skip = 1;
        }
        break;
    case C_EVENT:
        if (!begin && ieq(v, vl, "VEVENT")) {
            end_event(p);
            p->comp = C_NONE;
        } else if (begin && ieq(v, vl, "VEVENT")) {
            ev_free(&p->cur);  /* the previous one never ended; discard it */
        } else if (begin) {
            p->skip = 1;
        } else if (ieq(v, vl, "VCALENDAR")) {
            ev_free(&p->cur);
            p->comp = C_NONE;
        }
        break;
    case C_TZ:
        if (begin && (ieq(v, vl, "STANDARD") || ieq(v, vl, "DAYLIGHT"))) {
            p->comp = C_TZSUB;
            p->sub_is_dst = ieq(v, vl, "DAYLIGHT");
            memset(&p->sub, 0, sizeof(p->sub));
        } else if (!begin && ieq(v, vl, "VTIMEZONE")) {
            end_vtimezone(p);
            p->comp = C_NONE;
        } else if (begin) {
            p->skip = 1;
        }
        break;
    default:
        if (!begin && (ieq(v, vl, "STANDARD") || ieq(v, vl, "DAYLIGHT"))) {
            end_tz_sub(p);
            p->comp = C_TZ;
        } else if (begin) {
            p->skip = 1;
        }
        break;
    }
}

static void process_line(cal_parser_t *p)
{
    p->line[p->len] = '\0';
    const bool cut = p->trunc;
    if (cut) {
        p->st.lines_truncated++;
    }
    p->len = 0;
    p->trunc = false;

    size_t nl;
    params_t pp;
    char *v = (char *)split_line(p->line, &nl, &pp);
    if (v == NULL) {
        return;
    }
    const bool begin = ieq(p->line, nl, "BEGIN");
    if (begin || ieq(p->line, nl, "END")) {
        if (!cut) {
            begin_end(p, begin, v);
        }
        return;
    }
    if (p->skip > 0) {
        return;
    }
    if (p->comp == C_EVENT) {
        event_property(p, p->line, nl, v, &pp, cut);
    } else if ((p->comp == C_TZ || p->comp == C_TZSUB) && !cut) {
        tz_property(p, p->line, nl, v);
    }
}

cal_parser_t *cal_parser_new(const cal_window_t *win)
{
    cal_parser_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->win = *win;
    if (win->local != NULL) {
        p->local = *win->local;
    } else {
        cal_tz_fixed(&p->local, 0);
    }
    p->win.local = &p->local;
    p->tz_id = NO_TZID;
    return p;
}

int cal_parser_feed(cal_parser_t *p, const char *buf, size_t len)
{
    for (size_t i = 0; i < len && !p->failed; i++) {
        const char c = buf[i];
        if (p->eol) {
            p->eol = false;
            /* CRLF then a space or tab is a fold (RFC 5545 section 3.1). */
            if (c == ' ' || c == '\t') {
                continue;
            }
            process_line(p);
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            p->eol = true;
        } else if (p->len < CAL_LINE_MAX - 1) {
            p->line[p->len++] = c;
        } else {
            p->trunc = true;
        }
    }
    return p->failed ? -1 : 0;
}

typedef struct {
    cal_event_t *v;
    size_t n, cap;
} out_list_t;

typedef struct {
    cal_parser_t *p;
    out_list_t out;
    const ev_t **ovr;      /* overrides sorted by UID */
    size_t n_ovr;
    bool fallback;         /* current event hit an unknown TZID */
} expand_t;

static const cal_tz_t UTC_TZ = {0, 0, false, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};

static bool name_is_utc(const char *n)
{
    return strcmp(n, "UTC") == 0 || strcmp(n, "GMT") == 0 || strcmp(n, "Etc/UTC") == 0 ||
           strcmp(n, "Etc/GMT") == 0 || strcmp(n, "Z") == 0;
}

static const cal_tz_t *zone_of(expand_t *x, const dt_t *d)
{
    if (d->kind == DT_UTC) {
        return &UTC_TZ;
    }
    if (d->kind != DT_TZID) {
        return &x->p->local;
    }
    for (size_t i = 0; i < x->p->n_zones; i++) {
        if (x->p->zones[i].tzid == d->tzid) {
            return &x->p->zones[i].tz;
        }
    }
    if (name_is_utc(x->p->names[d->tzid])) {
        return &UTC_TZ;
    }
    x->fallback = true;
    return &x->p->local;
}

static int64_t dt_utc(expand_t *x, const dt_t *d)
{
    return d->kind == DT_UTC ? d->wall : cal_tz_local_to_utc(zone_of(x, d), d->wall);
}

static void push(expand_t *x, const ev_t *e, bool all_day, int64_t a, int64_t b)
{
    const cal_tz_t *loc = &x->p->local;
    const cal_window_t *w = &x->p->win;
    cal_event_t o;
    memset(&o, 0, sizeof(o));
    o.all_day = all_day;
    o.calendar = w->calendar;
    if (all_day) {
        /* a, b = first day and exclusive end day. */
        const int64_t su = cal_tz_local_to_utc(loc, a * DAY);
        const int64_t eu = cal_tz_local_to_utc(loc, b * DAY);
        if (!(su < w->to_utc && eu > w->from_utc)) {
            return;
        }
        o.start_day = (int32_t)a;
        o.end_day = (int32_t)(b - 1);
        o.start_utc = su;
    } else {
        /* a, b = start and end instants. */
        const bool hit = b > a ? (a < w->to_utc && b > w->from_utc)
                               : (a >= w->from_utc && a < w->to_utc);
        if (!hit) {
            return;
        }
        const int64_t ls = a + cal_tz_offset_at_utc(loc, a);
        o.start_day = (int32_t)floor_div(ls, DAY);
        o.start_min = (int16_t)((ls - (int64_t)o.start_day * DAY) / 60);
        if (b > a) {
            const int64_t le = b + cal_tz_offset_at_utc(loc, b);
            int64_t ed = floor_div(le, DAY);
            int64_t em = (le - ed * DAY) / 60;
            if (le == ed * DAY) {
                ed--;
                em = 1440;
            }
            o.end_day = (int32_t)ed;
            o.end_min = (int16_t)em;
        } else {
            o.end_day = o.start_day;
            o.end_min = o.start_min;
        }
        o.start_utc = a;
    }
    memcpy(o.title, e->title, sizeof(o.title));
    o.ink = (int8_t)e->ink1 - 1;
    size_t cap = x->out.cap;
    if (!grow((void **)&x->out.v, &cap, x->out.n + 1, sizeof(cal_event_t))) {
        x->p->failed = true;
        return;
    }
    x->out.cap = cap;
    x->out.v[x->out.n++] = o;
}

typedef struct {
    int64_t utc;   /* instant; for all-day, the day's wall midnight */
    int64_t day;   /* day in the event's zone */
} occ_t;

static int cmp_occ(const void *a, const void *b)
{
    const occ_t *x = a, *y = b;
    return (x->utc > y->utc) - (x->utc < y->utc);
}

/* Does a date-time list entry name this occurrence? A DATE compares by
 * day, anything else as an instant. */
static bool dt_matches(expand_t *x, const dt_t *d, bool all_day, const occ_t *o)
{
    if (d->kind == DT_DATE || all_day) {
        return floor_div(d->wall, DAY) == o->day;
    }
    return dt_utc(x, d) == o->utc;
}

static bool overridden(expand_t *x, const ev_t *e, bool all_day, const occ_t *o)
{
    if (e->uid == NULL || x->n_ovr == 0) {
        return false;
    }
    size_t lo = 0, hi = x->n_ovr;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (strcmp(x->ovr[mid]->uid, e->uid) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (; lo < x->n_ovr && strcmp(x->ovr[lo]->uid, e->uid) == 0; lo++) {
        if (dt_matches(x, &x->ovr[lo]->recurid, all_day, o)) {
            return true;
        }
    }
    return false;
}

static void emit_occ(expand_t *x, const ev_t *e, const cal_tz_t *zone, const occ_t *o,
                     int64_t exact)
{
    if (e->start.kind == DT_DATE) {
        int64_t len = 1;
        if (e->has_end) {
            len = floor_div(e->end.wall, DAY) - floor_div(e->start.wall, DAY);
        } else if (e->has_dur) {
            len = e->dur_days + e->dur_secs / DAY;
        }
        push(x, e, true, o->day, o->day + (len > 0 ? len : 1));
        return;
    }
    int64_t end = o->utc;
    if (e->has_end) {
        end = o->utc + exact;
    } else if (e->has_dur) {
        const int64_t wall = o->utc + cal_tz_offset_at_utc(zone, o->utc);
        end = cal_tz_local_to_utc(zone, wall + (int64_t)e->dur_days * DAY) + e->dur_secs;
    }
    push(x, e, false, o->utc, end > o->utc ? end : o->utc);
}

static bool add_occ(occ_t **v, size_t *n, size_t *cap, occ_t o)
{
    if (!grow((void **)v, cap, *n + 1, sizeof(occ_t))) {
        return false;
    }
    (*v)[(*n)++] = o;
    return true;
}

static void expand_event(expand_t *x, const ev_t *e)
{
    cal_parser_t *p = x->p;
    const bool all_day = e->start.kind == DT_DATE;
    const cal_tz_t *zone = zone_of(x, &e->start);
    const int64_t s_day = floor_div(e->start.wall, DAY);
    const int64_t tod = e->start.wall - s_day * DAY;
    int64_t exact = 0;
    if (!all_day && e->has_end) {
        exact = dt_utc(x, &e->end) - dt_utc(x, &e->start);
        exact = exact > 0 ? exact : 0;
    }

    /* Bounds in local days, padded by the event's length so an instance
     * that starts before the window and runs into it is still generated. */
    int64_t len_days = 1;
    if (e->has_end) {
        len_days += (e->end.wall - e->start.wall) / DAY + 1;
    } else if (e->has_dur) {
        len_days += e->dur_days + e->dur_secs / DAY + 1;
    }
    const int64_t from_day = floor_div(p->win.from_utc, DAY) - 2 - len_days;
    const int64_t to_day = floor_div(p->win.to_utc, DAY) + 2;

    occ_t *occ = NULL;
    size_t n_occ = 0, cap_occ = 0;
    bool ok = true;
#define OCC_AT(d) ((occ_t){all_day ? (d) * DAY : cal_tz_local_to_utc(zone, (d) * DAY + tod), (d)})
    if (s_day >= from_day && s_day <= to_day) {
        ok = add_occ(&occ, &n_occ, &cap_occ, OCC_AT(s_day));
    }
    const cal_rrule_t *r = &e->rrule;
    if (e->has_rrule && !r->unsupported && !e->has_recurid) {
        cal_rrule_iter_t *it = malloc(sizeof(*it));
        if (it == NULL) {
            ok = false;
        } else {
            /* COUNT includes every instance from DTSTART, so nothing may be
             * skipped; without it, whole periods before the window are jumped. */
            cal_rrule_iter_init(it, r, s_day, r->count ? INT64_MIN : from_day);
            uint32_t n = 1;
            int64_t d;
            while (ok && cal_rrule_iter_next(it, to_day, &d)) {
                if (r->count && ++n > r->count) {
                    break;
                }
                const occ_t o = OCC_AT(d);
                if (r->has_until) {
                    const bool past = r->until_is_utc
                        ? (all_day ? cal_tz_local_to_utc(&p->local, d * DAY) : o.utc) > r->until
                        : r->until_is_date ? d > floor_div(r->until, DAY)
                                           : d * DAY + tod > r->until;
                    if (past) {
                        break;
                    }
                }
                if (d > to_day) {
                    break;
                }
                if (d >= from_day) {
                    ok = add_occ(&occ, &n_occ, &cap_occ, o);
                }
            }
            free(it);
        }
    }
#undef OCC_AT
    for (uint32_t i = 0; ok && i < e->rdates.n && !e->has_recurid; i++) {
        const dt_t *rd = &e->rdates.v[i];
        occ_t o;
        if (all_day || rd->kind == DT_DATE) {
            o.day = floor_div(rd->wall, DAY);
            o.utc = all_day ? o.day * DAY : cal_tz_local_to_utc(zone, o.day * DAY + tod);
        } else {
            o.utc = dt_utc(x, rd);
            o.day = floor_div(o.utc + cal_tz_offset_at_utc(zone, o.utc), DAY);
        }
        if (o.day >= from_day && o.day <= to_day) {
            ok = add_occ(&occ, &n_occ, &cap_occ, o);
        }
    }
    if (!ok) {
        p->failed = true;
        free(occ);
        return;
    }
    if (n_occ > 1) {
        qsort(occ, n_occ, sizeof(occ_t), cmp_occ);
    }
    for (size_t i = 0; i < n_occ; i++) {
        if (i > 0 && occ[i].utc == occ[i - 1].utc) {
            continue;
        }
        bool skip = false;
        for (uint32_t j = 0; j < e->exdates.n && !skip; j++) {
            skip = dt_matches(x, &e->exdates.v[j], all_day, &occ[i]);
        }
        if (!skip && !e->has_recurid) {
            skip = overridden(x, e, all_day, &occ[i]);
        }
        if (!skip) {
            emit_occ(x, e, zone, &occ[i], exact);
        }
    }
    free(occ);
}

static int cmp_ovr(const void *a, const void *b)
{
    return strcmp((*(const ev_t *const *)a)->uid, (*(const ev_t *const *)b)->uid);
}

static int cmp_out(const void *a, const void *b)
{
    const cal_event_t *x = a, *y = b;
    if (x->start_day != y->start_day) {
        return x->start_day < y->start_day ? -1 : 1;
    }
    if (x->all_day != y->all_day) {
        return x->all_day ? -1 : 1;
    }
    if (x->start_min != y->start_min) {
        return x->start_min < y->start_min ? -1 : 1;
    }
    const int t = strcmp(x->title, y->title);
    if (t != 0) {
        return t;
    }
    if (x->end_day != y->end_day) {
        return x->end_day < y->end_day ? -1 : 1;
    }
    return (x->end_min > y->end_min) - (x->end_min < y->end_min);
}

int cal_parser_finish(cal_parser_t *p, cal_event_t *out, size_t cap, size_t *count,
                      cal_stats_t *stats)
{
    *count = 0;
    if (!p->failed && (p->eol || p->len > 0)) {
        p->eol = false;
        process_line(p);
    }
    if (p->comp == C_EVENT) {
        ev_free(&p->cur);  /* feed ended inside a VEVENT; it is incomplete */
        p->comp = C_NONE;
    }

    expand_t x;
    memset(&x, 0, sizeof(x));
    x.p = p;
    if (!p->failed) {
        x.ovr = malloc((p->n_evs ? p->n_evs : 1) * sizeof(*x.ovr));
        p->failed = x.ovr == NULL;
    }
    for (size_t i = 0; !p->failed && i < p->n_evs; i++) {
        if (p->evs[i].has_recurid && p->evs[i].uid != NULL) {
            x.ovr[x.n_ovr++] = &p->evs[i];
        }
    }
    if (!p->failed) {
        qsort(x.ovr, x.n_ovr, sizeof(*x.ovr), cmp_ovr);
    }
    for (size_t i = 0; !p->failed && i < p->n_evs; i++) {
        const ev_t *e = &p->evs[i];
        x.fallback = false;
        if (!(e->has_recurid && e->cancelled)) {
            expand_event(&x, e);
        }
        if (x.fallback) {
            p->st.tz_fallback++;
        }
    }
    free(x.ovr);

    int rc = 0;
    if (p->failed) {
        rc = -1;
    } else {
        if (x.out.n > 1) {
            qsort(x.out.v, x.out.n, sizeof(cal_event_t), cmp_out);
        }
        const size_t n = x.out.n < cap ? x.out.n : cap;
        if (n > 0) {
            memcpy(out, x.out.v, n * sizeof(cal_event_t));
        }
        *count = n;
        p->st.instances = (uint32_t)x.out.n;
        p->st.dropped_full = (uint32_t)(x.out.n - n);
    }
    free(x.out.v);
    if (stats != NULL) {
        *stats = p->st;
    }
    return rc;
}

void cal_parser_free(cal_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    ev_free(&p->cur);
    for (size_t i = 0; i < p->n_evs; i++) {
        ev_free(&p->evs[i]);
    }
    free(p->evs);
    free(p->zones);
    for (size_t i = 0; i < p->n_names; i++) {
        free(p->names[i]);
    }
    free(p->names);
    free(p);
}
