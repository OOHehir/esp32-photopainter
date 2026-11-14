/* cal_parse -- a streaming iCalendar (RFC 5545) parser and recurrence
 * expander that turns a feed into the cal_event_t instances inside a time window.
 *
 * Use: cal_parser_new(), cal_parser_feed() each chunk of the HTTP body as it arrives,
 * cal_parser_finish() once, cal_parser_free(). Chunks may split anywhere, including
 * inside a CRLF or a UTF-8 sequence. The feed is never buffered: memory is one logical
 * line (CAL_LINE_MAX) plus what is kept per VEVENT, and only VEVENTs that can reach the
 * window are kept -- recurring masters, overrides (RECURRENCE-ID) and single events near
 * it. Expansion waits for finish() because a VTIMEZONE may follow the events using it.
 *
 * Supported: VEVENT with DTSTART/DTEND/DURATION, SUMMARY, UID, STATUS, RRULE, EXDATE,
 * RDATE, RECURRENCE-ID; VTIMEZONE whose newest STANDARD/DAYLIGHT pair is a yearly
 * rule. The RRULE subset is FREQ=DAILY|WEEKLY|MONTHLY|YEARLY with INTERVAL, COUNT,
 * UNTIL, BYDAY, BYMONTHDAY, BYMONTH, BYSETPOS and WKST; a rule using anything else
 * yields only its DTSTART and is counted in unsupported_rrule. Not supported: BYHOUR,
 * BYMINUTE, BYSECOND, BYWEEKNO, BYYEARDAY, sub-daily FREQ, RDATE;VALUE=PERIOD,
 * RANGE=THISANDFUTURE beyond the one instance it names, VTODO/VJOURNAL, and historical
 * VTIMEZONE rules (the newest rule is applied to every year).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cal_event.h"
#include "cal_tz.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest logical (unfolded) line kept, NUL included. Longer lines are
 * cut and counted; only a cut SUMMARY, EXDATE or RDATE is still used, the last minus its
 * partial final item. */
#define CAL_LINE_MAX 4096

typedef struct {
    int64_t from_utc, to_utc;   /* window [from, to), seconds since the epoch */
    const cal_tz_t *local;      /* device zone; copied, NULL = UTC. Output
                                 * days and minutes, floating times and dates use it. */
    uint8_t calendar;           /* copied into each cal_event_t */
    bool hide_private;          /* drop CLASS:PRIVATE and CLASS:CONFIDENTIAL
                                 * events as if cancelled; a hidden override still removes
                                 * the instance it replaces */
} cal_window_t;

typedef struct {
    uint32_t vevents;           /* VEVENTs read */
    uint32_t instances;         /* instances in the window, before the cap */
    uint32_t overrides;         /* kept VEVENTs with RECURRENCE-ID */
    uint32_t cancelled;         /* VEVENTs with STATUS:CANCELLED */
    uint32_t unsupported_rrule; /* RRULEs outside the subset, and RDATE periods */
    uint32_t tz_fallback;       /* kept events using a TZID with no VTIMEZONE */
    uint32_t lines_truncated;   /* lines longer than CAL_LINE_MAX - 1 */
    uint32_t dropped_full;      /* instances beyond cap */
    uint32_t hidden_private;    /* VEVENTs dropped by hide_private */
    uint32_t with_colour;       /* VEVENTs carrying a COLOR property */
} cal_stats_t;

typedef struct cal_parser cal_parser_t;

/**
 * @brief Map an RFC 7986 COLOR value -- a CSS color name, or "#rrggbb" as some feeds
 *        write it -- to the nearest of the panel's six inks.
 * @param value  Case-insensitive.
 * @return An ink code (0 black, 2 yellow, 3 red, 5 blue, 6 green), or -1 for a value not
 *         recognized. White is never returned: a white block is invisible on the panel.
 */
int cal_colour_to_ink(const char *value);

/**
 * @brief Create a parser for one feed.
 * @param win  Window and device zone; copied.
 * @return Parser, or NULL when out of memory.
 */
cal_parser_t *cal_parser_new(const cal_window_t *win);

/**
 * @brief Consume the next chunk of the feed.
 * @param p    Parser.
 * @param buf  Bytes; need not be NUL-terminated or end on a line boundary.
 * @param len  Bytes in buf; 0 is allowed.
 * @return 0, or -1 once memory has run out. The failure is sticky: later feeds and
 *         finish() also return -1.
 */
int cal_parser_feed(cal_parser_t *p, const char *buf, size_t len);

/**
 * @brief End the feed, expand recurrences and write the window's instances.
 *
 * Instances are sorted by start_day, then all-day before timed, then start_min, then
 * title. When more than cap overlap the window, the earliest cap are kept. Call once;
 * after it only cal_parser_free() is valid.
 *
 * @param p           Parser.
 * @param[out] out    Room for cap instances; may be NULL when cap is 0.
 * @param cap         Capacity of out.
 * @param[out] count  Instances written.
 * @param[out] stats  Counters; may be NULL.
 * @return 0, or -1 when memory ran out (count is then 0).
 */
int cal_parser_finish(cal_parser_t *p, cal_event_t *out, size_t cap, size_t *count,
                      cal_stats_t *stats);

/**
 * @brief Release a parser. NULL is allowed.
 * @param p  Parser.
 */
void cal_parser_free(cal_parser_t *p);

#ifdef __cplusplus
}
#endif
