/* cal_rrule -- the RRULE subset this device expands, as a day generator.
 *
 * Works on local civil days only. The time of day of every instance is DTSTART's, so the
 * caller adds it and converts to UTC with the event's zone; that keeps a 09:00 meeting at
 * 09:00 across a DST change.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    CAL_FREQ_NONE = 0,
    CAL_FREQ_DAILY,
    CAL_FREQ_WEEKLY,
    CAL_FREQ_MONTHLY,
    CAL_FREQ_YEARLY,
};

#define CAL_RRULE_MAX_BYDAY    16
#define CAL_RRULE_MAX_MONTHDAY 31
#define CAL_RRULE_MAX_SETPOS   16

typedef struct {
    int8_t  ord;      /* 0 = every such weekday; +-1..53 = nth in the period */
    uint8_t wd;       /* 0 = Sunday..6 */
} cal_byday_t;

typedef struct {
    uint8_t  freq;
    bool     unsupported;  /* parts outside the subset; expand DTSTART only */
    uint8_t  wkst;         /* 0 = Sunday..6; default Monday */
    uint32_t interval;
    uint32_t count;        /* 0 = unbounded */
    /* UNTIL, raw. until_is_utc: seconds since the epoch; otherwise local
     * wall-clock seconds, with until_is_date meaning "through the end of that day". */
    bool     has_until, until_is_utc, until_is_date;
    int64_t  until;
    uint16_t bymonth;      /* bit m set for month m */
    uint8_t  n_byday, n_bymonthday, n_bysetpos;
    cal_byday_t byday[CAL_RRULE_MAX_BYDAY];
    int8_t   bymonthday[CAL_RRULE_MAX_MONTHDAY];
    int16_t  bysetpos[CAL_RRULE_MAX_SETPOS];
} cal_rrule_t;

/**
 * @brief Parse an RRULE value such as "FREQ=MONTHLY;BYDAY=-1FR".
 *
 * Never fails outright. A rule that names a part outside the subset (BYHOUR, BYWEEKNO,
 * HOURLY, ...) or that cannot be read comes back with unsupported set, and the caller
 * treats the event as its DTSTART alone.
 *
 * @param s         NUL-terminated value.
 * @param[out] out  Always written.
 */
void cal_rrule_parse(const char *s, cal_rrule_t *out);

/* Days of one period, sorted. 372 covers a YEARLY rule with every weekday
 * plus a few ordinals. */
#define CAL_RRULE_PERIOD_MAX 400

typedef struct {
    const cal_rrule_t *r;
    int64_t dtstart_day;
    uint8_t dtstart_month, dtstart_mday, dtstart_wd;
    int64_t k;                        /* period index from DTSTART's */
    int64_t base;                     /* DTSTART's day/week start/month/year */
    int32_t n, i;
    /* int32 days reach year 5.8 million, and halve this buffer's stack
     * cost on the device. */
    int32_t days[CAL_RRULE_PERIOD_MAX];
} cal_rrule_iter_t;

/**
 * @brief Start generating the days of a rule.
 * @param[out] it       Iterator; holds a pointer to r, which must outlive it.
 * @param r             A supported rule (freq != NONE, unsupported false).
 * @param dtstart_day   DTSTART's local day.
 * @param skip_to_day   Periods ending before this day may be skipped. Pass INT64_MIN when
 *                      COUNT is set, since skipped instances still count.
 */
void cal_rrule_iter_init(cal_rrule_iter_t *it, const cal_rrule_t *r, int64_t dtstart_day,
                         int64_t skip_to_day);

/**
 * @brief Next day strictly after DTSTART's that the rule produces, in increasing order.
 * @param it        Iterator.
 * @param limit     Stop once a period starts after this day.
 * @param[out] day  The day.
 * @return true with a day, false when past limit.
 */
bool cal_rrule_iter_next(cal_rrule_iter_t *it, int64_t limit, int64_t *day);
