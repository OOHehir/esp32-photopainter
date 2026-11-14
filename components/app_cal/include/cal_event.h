/* cal_event -- one calendar occurrence, already in the device's local time.
 *
 * The contract between the iCalendar parser (cal_parse.c) and the layout (app_ui). The
 * parser does every time-zone and recurrence conversion, so the layout only ever sees
 * local civil days and minutes and needs no clock, no time zone and no libc time API.
 * That is what lets both sides run under plain gcc in test/host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes, including the NUL. Longer titles are cut at a UTF-8 boundary. */
#define CAL_TITLE_MAX 64

/* Calendars a device can show at once; one colour each on the panel. */
#define CAL_MAX_CALENDARS 4

typedef struct {
    /* Days since 1970-01-01 in the device's local zone. Inclusive at both
     * ends, so a one-day event has start_day == end_day. A timed event ending exactly at
     * midnight ends on the previous day. */
    int32_t start_day;
    int32_t end_day;
    /* Minutes after local midnight on start_day / end_day, 0..1440.
     * Both 0 for an all-day event. */
    int16_t start_min;
    int16_t end_min;
    bool    all_day;
    uint8_t calendar;            /* index into the configured calendar list */
    int8_t  ink;                 /* the event's own COLOR as a panel ink
                                  * (app_gfx_colour_t), or -1 to use the calendar's */
    int64_t start_utc;           /* sort key; local midnight for all-day */
    char    title[CAL_TITLE_MAX];/* UTF-8, unescaped, NUL-terminated */
} cal_event_t;

#ifdef __cplusplus
}
#endif
