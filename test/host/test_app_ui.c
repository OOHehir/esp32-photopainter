/* The week layout. The band test proves the shipped layout is seam-safe;
 * the volatile test proves a temperature change alone cannot trigger a refresh. */
#include "test.h"
#include "app_ui.h"

#include <stdlib.h>
#include <string.h>

#define WHOLE ((size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT)
#define DAY0  20771   /* 2026-11-14 */

static const app_ui_calendar_t CALS[] = {
    { "Family", APP_GFX_BLUE }, { "Work", APP_GFX_RED }, { "Birthdays", APP_GFX_YELLOW },
};

static cal_event_t timed(int day, int s, int e, int cal, const char *title)
{
    cal_event_t x = { .start_day = DAY0 + day, .end_day = DAY0 + day,
                      .start_min = (int16_t)s, .end_min = (int16_t)e, .calendar = (uint8_t)cal,
                      .ink = -1 };
    snprintf(x.title, sizeof(x.title), "%s", title);
    return x;
}

static cal_event_t all_day(int d0, int d1, int cal, const char *title)
{
    cal_event_t x = timed(d0, 0, 0, cal, title);
    x.end_day = DAY0 + d1;
    x.all_day = true;
    return x;
}

static app_ui_model_t week(const cal_event_t *ev, size_t n)
{
    return (app_ui_model_t){
        .mode = APP_UI_WEEK, .today = DAY0, .now_min = 600, .events = ev, .n_events = n,
        .calendars = CALS, .n_calendars = 3, .have_climate = true, .temp_c10 = 214,
        .rh_pct = 48, .battery_pct = 80, .version = "v0.1.0",
    };
}

static void render(uint8_t *px, const app_ui_model_t *m, bool vol)
{
    app_gfx_surface_t s = { .pixels = px, .y0 = 0, .rows = APP_GFX_HEIGHT };
    app_ui_render(&s, m, vol);
}

static void test_week_in_bands_matches_whole_frame(void)
{
    cal_event_t ev[] = {
        all_day(-1, 2, 0, "Grandma visiting"),
        timed(0, 540, 600, 1, "Stand-up meeting with the whole team"),
        timed(3, 1380, 1440, 0, "Late"),
    };
    app_ui_model_t m = week(ev, 3);
    uint8_t *whole = malloc(WHOLE), *banded = malloc(WHOLE);
    CHECK(whole && banded, "allocation failed");
    if (!whole || !banded) { free(whole); free(banded); return; }

    render(whole, &m, true);
    for (uint16_t y0 = 0; y0 < APP_GFX_HEIGHT; y0 += 32) {
        app_gfx_surface_t b = { .pixels = banded + (size_t)y0 * APP_GFX_WIDTH, .y0 = y0,
                                .rows = 32 };
        app_ui_render(&b, &m, true);
    }

    CHECK(memcmp(whole, banded, WHOLE) == 0, "banded week differs from whole-frame week");
    free(whole); free(banded);
}

static void test_temperature_change_leaves_hash_frame_unchanged(void)
{
    app_ui_model_t a = week(NULL, 0), b = week(NULL, 0);
    b.temp_c10 = 199;
    b.battery_pct = 20;
    uint8_t *fa = malloc(WHOLE), *fb = malloc(WHOLE);
    CHECK(fa && fb, "allocation failed");
    if (!fa || !fb) { free(fa); free(fb); return; }

    render(fa, &a, false);
    render(fb, &b, false);
    CHECK(memcmp(fa, fb, WHOLE) == 0, "volatile readings leaked into the hash frame");

    render(fa, &a, true);
    render(fb, &b, true);
    CHECK(memcmp(fa, fb, WHOLE) != 0, "readings not drawn in the shown frame");
    free(fa); free(fb);
}

static void test_events_outside_the_week_draw_nothing(void)
{
    cal_event_t ev[] = { timed(7, 540, 600, 0, "Next Saturday"),
                         timed(-1, 540, 600, 0, "Yesterday") };
    app_ui_model_t with = week(ev, 2), without = week(NULL, 0);
    uint8_t *fa = malloc(WHOLE), *fb = malloc(WHOLE);
    CHECK(fa && fb, "allocation failed");
    if (!fa || !fb) { free(fa); free(fb); return; }

    render(fa, &with, false);
    render(fb, &without, false);
    CHECK(memcmp(fa, fb, WHOLE) == 0, "an event outside the 7 days was drawn");
    free(fa); free(fb);
}

static void test_wrap_breaks_at_spaces(void)
{
    char lines[3][68];
    size_t n = app_ui_wrap(&app_gfx_font_label, "Pizza night at home", 100, lines[0], 68, 3);
    CHECK_INT(n, 2);
    CHECK_STR(lines[0], "Pizza night");
    CHECK_STR(lines[1], "at home");
}

static void test_wrap_ellipsizes_what_does_not_fit(void)
{
    char lines[1][68];
    size_t n = app_ui_wrap(&app_gfx_font_label, "Parent teacher meeting", 80, lines[0], 68, 1);
    CHECK_INT(n, 1);
    CHECK_STR(lines[0], "Parent\xE2\x80\xA6");
}

static void test_wrap_never_splits_a_utf8_sequence(void)
{
    char lines[2][68];
    /* 13 x "é" (2 bytes each) is wider than 60 px, so it must hard-break. */
    const char *t = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    size_t n = app_ui_wrap(&app_gfx_font_label, t, 60, lines[0], 68, 2);
    CHECK_INT(n, 2);
    CHECK_INT(strlen(lines[0]) % 2, 0);
    CHECK((unsigned char)lines[1][0] == 0xC3, "second line starts mid-sequence");
}

static void test_wrap_breaks_after_a_hyphen(void)
{
    char lines[2][68];
    size_t n = app_ui_wrap(&app_gfx_font_label, "Parent-teacher", 80, lines[0], 68, 2);
    CHECK_INT(n, 2);
    CHECK_STR(lines[0], "Parent-");
    CHECK_STR(lines[1], "teacher");
}

/* Four all-day events on one day overflow the three lanes; the day's timed event must
 * still be drawn, not folded into "+N more". */
static void test_lane_overflow_does_not_hide_timed_events(void)
{
    cal_event_t crowded[] = {
        all_day(0, 0, 0, "A"), all_day(0, 0, 0, "B"), all_day(0, 0, 0, "C"),
        all_day(0, 0, 0, "D"), timed(0, 900, 960, 1, "Dentist"),
    };
    cal_event_t without_timed[] = {
        all_day(0, 0, 0, "A"), all_day(0, 0, 0, "B"), all_day(0, 0, 0, "C"),
        all_day(0, 0, 0, "D"),
    };
    app_ui_model_t a = week(crowded, 5), b = week(without_timed, 4);
    uint8_t *fa = malloc(WHOLE), *fb = malloc(WHOLE);
    CHECK(fa && fb, "allocation failed");
    if (!fa || !fb) { free(fa); free(fb); return; }

    render(fa, &a, false);
    render(fb, &b, false);

    /* Rows 140..430 are the grid below the lanes, above the "+N more" line. */
    CHECK(memcmp(fa + (size_t)140 * APP_GFX_WIDTH, fb + (size_t)140 * APP_GFX_WIDTH,
                 (size_t)(430 - 140) * APP_GFX_WIDTH) != 0,
          "timed event hidden behind lane overflow");
    free(fa); free(fb);
}

void test_app_ui(void)
{
    SUITE("app_ui");
    test_week_in_bands_matches_whole_frame();
    test_temperature_change_leaves_hash_frame_unchanged();
    test_events_outside_the_week_draw_nothing();
    test_wrap_breaks_at_spaces();
    test_wrap_ellipsizes_what_does_not_fit();
    test_wrap_never_splits_a_utf8_sequence();
    test_wrap_breaks_after_a_hyphen();
    test_lane_overflow_does_not_hide_timed_events();
}
