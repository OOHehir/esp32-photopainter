/* Helpers shared by test_cal_parse.c and test_cal_rrule.c: load a fixture,
 * run a feed through the parser in chunks, and compare the result with a literal table. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cal_parse.h"
#include "test.h"

typedef struct {
    const char *title;
    int32_t start_day;
    int16_t start_min;
    int32_t end_day;
    int16_t end_min;
    bool all_day;
    int64_t start_utc;
} cal_want_t;

/* Path relative to test/host, where `make -C test/host` runs the binary. */
static inline char *cal_test_load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

/* The fixtures are stored with LF so they diff cleanly; Google serves CRLF. */
static inline char *cal_test_crlf(const char *s, size_t len, size_t *out_len)
{
    char *o = malloc(2 * len + 1);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            o[n++] = '\r';
        }
        o[n++] = s[i];
    }
    o[n] = '\0';
    *out_len = n;
    return o;
}

static inline int cal_test_run(const char *ics, size_t len, size_t chunk, const cal_window_t *w,
                               cal_event_t *out, size_t cap, size_t *n, cal_stats_t *st)
{
    cal_parser_t *p = cal_parser_new(w);
    int rc = 0;
    for (size_t off = 0; off < len && rc == 0; off += chunk) {
        rc = cal_parser_feed(p, ics + off, len - off < chunk ? len - off : chunk);
    }
    const int frc = cal_parser_finish(p, out, cap, n, st);
    cal_parser_free(p);
    return rc != 0 ? rc : frc;
}

static inline void cal_test_expect(const char *what, const cal_event_t *got, size_t n,
                                   const cal_want_t *want, size_t n_want)
{
    CHECK(n == n_want, "%s: got %zu events, want %zu", what, n, n_want);
    for (size_t i = 0; i < n && i < n_want; i++) {
        const cal_event_t *g = &got[i];
        const cal_want_t *e = &want[i];
        CHECK(strcmp(g->title, e->title) == 0 && g->start_day == e->start_day &&
                  g->start_min == e->start_min && g->end_day == e->end_day &&
                  g->end_min == e->end_min && g->all_day == e->all_day &&
                  g->start_utc == e->start_utc,
              "%s[%zu]: got {\"%s\", %d, %d, %d, %d, %d, %lld}, "
              "want {\"%s\", %d, %d, %d, %d, %d, %lld}",
              what, i, g->title, (int)g->start_day, g->start_min, (int)g->end_day,
              g->end_min, g->all_day, (long long)g->start_utc, e->title, (int)e->start_day,
              e->start_min, (int)e->end_day, e->end_min, e->all_day, (long long)e->start_utc);
    }
}
