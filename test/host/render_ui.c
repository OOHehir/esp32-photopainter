/* Render app_ui to PPM files so the layout can be looked at.
 *
 *     make -C test/host preview
 *
 * Not a test: the seam tests prove the layout renders consistently, but only looking
 * shows a title clipped against a column edge or a glyph missing from a font.
 * Writes week.ppm, setup.ppm and nocal.ppm into the directory given as argv[1].
 */
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

/* Approximate Spectra 6 pigments, indexed by color code. */
static const uint8_t PAL[8][3] = {
    {20, 20, 20}, {245, 245, 240}, {235, 200, 0}, {190, 30, 30},
    {0, 0, 0}, {30, 70, 170}, {40, 120, 60}, {0, 0, 0},
};

static void write_ppm(const char *path, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", APP_GFX_WIDTH, APP_GFX_HEIGHT);
    for (size_t i = 0; i < (size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT; i++) {
        fwrite(PAL[px[i] & 7], 1, 3, f);
    }
    fclose(f);
    printf("  %s\n", path);
}

#define DAY0 20771   /* 2026-11-14, a Saturday */

static cal_event_t ev(int day, int s, int e, int cal, const char *t)
{
    cal_event_t x = {0};
    x.start_day = x.end_day = DAY0 + day;
    x.start_min = (int16_t)s; x.end_min = (int16_t)e; x.calendar = (uint8_t)cal;
    x.ink = -1;
    snprintf(x.title, sizeof(x.title), "%s", t);
    return x;
}

static cal_event_t allday(int d0, int d1, int cal, const char *t)
{
    cal_event_t x = ev(d0, 0, 0, cal, t);
    x.end_day = DAY0 + d1; x.all_day = true;
    return x;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint8_t px[(size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT];
    app_gfx_surface_t s = { .pixels = px, .y0 = 0, .rows = APP_GFX_HEIGHT };
    char path[512];

    static const app_ui_calendar_t cals[] = {
        { "Family", APP_GFX_BLUE }, { "Owen", APP_GFX_RED },
        { "School", APP_GFX_GREEN }, { "Birthdays", APP_GFX_YELLOW },
    };
    cal_event_t e[] = {
        allday(-1, 1, 0, "Grandma visiting"),
        allday(3, 3, 3, "Aoife's birthday"),
        allday(5, 6, 2, "Mid-term break"),
        ev(0, 9 * 60, 10 * 60, 1, "Gym"),
        ev(0, 11 * 60, 12 * 60 + 30, 2, "Soccer training – Liam"),
        ev(0, 18 * 60 + 30, 21 * 60, 0, "Dinner at Café Olé with the Murphys"),
        ev(2, 9 * 60, 17 * 60, 1, "Work"),
        ev(2, 17 * 60 + 15, 18 * 60, 2, "Swimming"),
        ev(3, 14 * 60 + 30, 15 * 60, 0, "Dentist"),
        ev(4, 9 * 60, 17 * 60, 1, "Work"),
        ev(4, 19 * 60, 20 * 60, 0, "Parent–teacher meeting"),
        ev(4, 20 * 60, 21 * 60, 0, "Book club"),
        ev(4, 21 * 60, 22 * 60, 1, "Call with Sydney office re: Q1 planning"),
        ev(4, 22 * 60, 23 * 60, 2, "Late one"),
        ev(4, 23 * 60, 23 * 60 + 30, 2, "Later one"),
        ev(6, 18 * 60, 19 * 60, 0, "Pizza night"),
    };
    app_ui_model_t m = {
        .mode = APP_UI_WEEK, .today = DAY0, .now_min = 10 * 60 + 20,
        .events = e, .n_events = sizeof(e) / sizeof(e[0]),
        .calendars = cals, .n_calendars = 4,
        .have_climate = true, .temp_c10 = 214, .rh_pct = 48,
        .battery_pct = 87, .version = "v0.1.0",
    };
    snprintf(path, sizeof(path), "%s/week.ppm", dir);
    app_ui_render(&s, &m, true); write_ppm(path, px);

    m.stale = true; snprintf(m.stale_since, sizeof(m.stale_since), "13:20");
    snprintf(path, sizeof(path), "%s/stale.ppm", dir);
    app_ui_render(&s, &m, true); write_ppm(path, px);

    m.mode = APP_UI_SETUP; m.ap_ssid = "photopainter-a1b2c3"; m.ap_pass = "K7MPQ2XR9WTF";
    snprintf(path, sizeof(path), "%s/setup.ppm", dir);
    app_ui_render(&s, &m, true); write_ppm(path, px);

    m.mode = APP_UI_NO_CALENDARS; m.url = "http://192.168.1.40/";
    snprintf(path, sizeof(path), "%s/nocal.ppm", dir);
    app_ui_render(&s, &m, true); write_ppm(path, px);
    return 0;
}
