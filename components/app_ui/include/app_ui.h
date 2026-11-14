/* app_ui -- the layout. Events in, drawing calls out.
 *
 * No I/O, no clock, no time zone: everything arrives in app_ui_model_t already in local
 * civil days and minutes, so a rendering is a pure function of its inputs and the whole
 * component runs under gcc on the build machine (`make -C test/host preview`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_gfx.h"
#include "cal_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_UI_WEEK,           /* the calendar */
    APP_UI_SETUP,          /* no Wi-Fi credentials: how to reach the portal */
    APP_UI_NO_CALENDARS,   /* on the network, but no calendar URL configured */
} app_ui_mode_t;

typedef struct {
    char             name[24];
    app_gfx_colour_t colour;
} app_ui_calendar_t;

typedef struct {
    app_ui_mode_t mode;

    /* Local civil day number (days since 1970-01-01) and minutes after
     * local midnight. The week starts at `today`. */
    int32_t today;
    int16_t now_min;

    const cal_event_t       *events;     /* sorted as cal_parse emits them */
    size_t                   n_events;
    const app_ui_calendar_t *calendars;
    size_t                   n_calendars;

    /* Set when a calendar failed to fetch; its events came from the cache or
     * are missing. `stale_since` is the last good fetch, "HH:MM" today or "Wed HH:MM"
     * earlier, or empty when there is none. */
    bool stale;
    char stale_since[12];

    /* Volatile readings. Drawn when present, but left out of the frame
     * that decides whether the panel needs a refresh -- see app_ui_render(). */
    bool    have_climate;
    int16_t temp_c10;      /* tenths of a degree */
    uint8_t rh_pct;
    int8_t  battery_pct;   /* -1 = no battery */
    bool    charging;

    /* Setup and no-calendar screens. */
    const char *ap_ssid;
    const char *ap_pass;
    const char *url;       /* "http://192.168.1.40/" */
    const char *version;
} app_ui_model_t;

/**
 * @brief Draw the whole panel for `model`.
 * @param surface        Any band; drawing is in absolute panel coordinates and clipped.
 * @param model          What to show.
 * @param with_volatile  false leaves out the climate and battery readings, giving the
 *                       frame whose hash decides whether a refresh is worth 28 seconds of
 *                       flashing and a share of the battery.
 */
void app_ui_render(const app_gfx_surface_t *surface, const app_ui_model_t *model,
                   bool with_volatile);

/**
 * @brief Split `text` into lines no wider than `width` pixels, breaking at spaces and
 *        hard-breaking words that do not fit; the last line gets an ellipsis when text
 *        remains.
 * @param[out] lines     max_lines buffers of line_len bytes each, NUL-terminated.
 * @return Number of lines written, 0..max_lines.
 */
size_t app_ui_wrap(const app_gfx_font_t *font, const char *text, int width,
                   char *lines, size_t line_len, size_t max_lines);

#ifdef __cplusplus
}
#endif
