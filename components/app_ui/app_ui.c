#include "app_ui.h"

#include <stdio.h>
#include <string.h>

/* Geometry, in panel pixels. The panel is 800 x 480 at 0.2 mm pitch, so
 * 15 px text is 3 mm tall -- readable across a kitchen, which is the design target. */
#define W            APP_GFX_WIDTH
#define H            APP_GFX_HEIGHT
#define DAYS         7
#define HEADER_H     72
#define COLHEAD_Y    76
#define COLHEAD_H    26
#define GRID_Y       (COLHEAD_Y + COLHEAD_H + 3)
#define FOOT_Y       456
#define LANE_H       22
#define MAX_LANES    3
#define PAD          3
#define TITLE_LINES  3
#define LINE_MAX     (CAL_TITLE_MAX + 4)

static const char *const WDAY_SHORT[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const WDAY_LONG[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday" };
static const char *const MONTH[12] = { "January", "February", "March", "April", "May", "June",
                                       "July", "August", "September", "October", "November",
                                       "December" };

/* Days since 1970-01-01 to a civil date; Howard Hinnant's algorithm. Kept
 * local so the layout does not depend on the parser's headers for three integers. */
static void civil(int32_t days, int *y, int *m, int *d)
{
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* 1970-01-01 was a Thursday; 0 = Sunday. */
static int weekday(int32_t days)
{
    int w = (int)((days + 4) % 7);
    return w < 0 ? w + 7 : w;
}

static int col_x(int col) { return col * W / DAYS; }

static app_gfx_colour_t ink_on(app_gfx_colour_t bg)
{
    return (bg == APP_GFX_YELLOW || bg == APP_GFX_WHITE) ? APP_GFX_BLACK : APP_GFX_WHITE;
}

/* An event's own color wins; otherwise its calendar's. */
static app_gfx_colour_t event_ink(const app_ui_model_t *m, const cal_event_t *e)
{
    if (e->ink >= 0) return (app_gfx_colour_t)e->ink;
    return e->calendar < m->n_calendars ? m->calendars[e->calendar].colour : APP_GFX_BLUE;
}

static void text_right(const app_gfx_surface_t *s, int right, int baseline,
                       const app_gfx_font_t *f, const char *t, app_gfx_colour_t c)
{
    app_gfx_text(s, right - app_gfx_text_width(f, t), baseline, f, t, c);
}

static void text_center(const app_gfx_surface_t *s, int cx, int baseline,
                        const app_gfx_font_t *f, const char *t, app_gfx_colour_t c)
{
    app_gfx_text(s, cx - app_gfx_text_width(f, t) / 2, baseline, f, t, c);
}

/* Byte length of the UTF-8 sequence starting at `p`; 1 for a stray byte,
 * so malformed input still advances. */
static size_t cp_len(const char *p)
{
    unsigned char c = (unsigned char)*p;
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4 : 1;
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)p[i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return n;
}

size_t app_ui_wrap(const app_gfx_font_t *font, const char *text, int width,
                   char *lines, size_t line_len, size_t max_lines)
{
    static const char ELLIPSIS[] = "\xE2\x80\xA6";
    size_t n = 0;
    const char *p = text;
    while (*p && n < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *line = lines + n * line_len;
        size_t len = 0, last_space = 0;
        const char *after_space = NULL;
        line[0] = '\0';
        while (*p) {
            size_t k = cp_len(p);
            if (len + k + 1 > line_len) break;
            memcpy(line + len, p, k);
            line[len + k] = '\0';
            if (app_gfx_text_width(font, line) > width && len > 0) {
                line[len] = '\0';
                break;
            }
            if (*p == ' ') {
                last_space = len;
                after_space = p + 1;
            } else if (*p == '-' || (k == 3 && !memcmp(p, "\xE2\x80\x93", 3))) {
                /* A hyphen or en dash is also a break, kept on this line. */
                last_space = len + k;
                after_space = p + k;
            }
            len += k;
            p += k;
        }
        /* Broke mid-word with a space earlier on the line: move the
         * partial word to the next line instead of splitting it. */
        if (*p && *p != ' ' && after_space && last_space > 0) {
            line[last_space] = '\0';
            p = after_space;
        }
        n++;
    }
    while (*p == ' ') p++;
    if (*p && n > 0) {
        char *last = lines + (n - 1) * line_len;
        size_t len = strlen(last);
        for (;;) {
            char probe[LINE_MAX + 4];
            snprintf(probe, sizeof(probe), "%.*s%s", (int)len, last, ELLIPSIS);
            if ((app_gfx_text_width(font, probe) <= width && len + 4 <= line_len) || len == 0) {
                strcpy(last + len, ELLIPSIS);
                break;
            }
            do { len--; } while (len > 0 && ((unsigned char)last[len] & 0xC0) == 0x80);
            while (len > 0 && last[len - 1] == ' ') len--;
        }
    }
    return n;
}

static void fit_one_line(const app_gfx_font_t *f, const char *text, int width, char *out,
                         size_t out_len)
{
    if (app_ui_wrap(f, text, width, out, out_len, 1) == 0) {
        out[0] = '\0';
    }
}

/* ------------------------------------------------------------------------- header */

static void draw_battery(const app_gfx_surface_t *s, int right, int baseline, int pct,
                         bool charging)
{
    char t[8];
    snprintf(t, sizeof(t), "%d%%", pct);
    int tw = app_gfx_text_width(&app_gfx_font_label, t);
    int bx = right - tw - 34, by = baseline - 12;
    app_gfx_rect(s, bx, by, 26, 13, APP_GFX_BLACK);
    app_gfx_fill_rect(s, bx + 26, by + 4, 3, 5, APP_GFX_BLACK);
    int fill = pct * 22 / 100;
    app_gfx_colour_t c = pct <= 15 ? APP_GFX_RED : charging ? APP_GFX_GREEN : APP_GFX_BLACK;
    app_gfx_fill_rect(s, bx + 2, by + 2, fill, 9, c);
    app_gfx_text(s, right - tw, baseline, &app_gfx_font_label, t, APP_GFX_BLACK);
}

static void draw_header(const app_gfx_surface_t *s, const app_ui_model_t *m, bool vol)
{
    int y, mo, d;
    civil(m->today, &y, &mo, &d);
    char num[4];
    snprintf(num, sizeof(num), "%d", d);
    app_gfx_text(s, 14, 60, &app_gfx_font_big, num, APP_GFX_RED);
    int x = 14 + app_gfx_text_width(&app_gfx_font_big, num) + 14;
    app_gfx_text(s, x, 32, &app_gfx_font_head, WDAY_LONG[weekday(m->today)], APP_GFX_BLACK);
    char my[24];
    snprintf(my, sizeof(my), "%s %d", MONTH[mo - 1], y);
    app_gfx_text(s, x, 58, &app_gfx_font_body, my, APP_GFX_BLACK);

    if (vol && m->have_climate) {
        char c[32];
        int t = m->temp_c10;
        snprintf(c, sizeof(c), "%s%d.%d\xC2\xB0" "C   %u%%", t < 0 ? "-" : "",
                 (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10, (unsigned)m->rh_pct);
        text_right(s, W - 14, 30, &app_gfx_font_head, c, APP_GFX_BLACK);
    }
    if (vol && m->battery_pct >= 0) {
        draw_battery(s, W - 14, 58, m->battery_pct, m->charging);
    }
    app_gfx_fill_rect(s, 0, HEADER_H - 1, W, 2, APP_GFX_BLACK);
}

/* --------------------------------------------------------------------------- grid */

/* Timed events longer than a day go in the all-day lanes, as Google's
 * own week view does: a 3-day conference as a block in each column says less than one
 * bar across them. */
static bool is_lane_event(const cal_event_t *e)
{
    if (e->all_day) return true;
    int64_t span = ((int64_t)e->end_day - e->start_day) * 1440 + e->end_min - e->start_min;
    return span >= 1440;
}

static void fmt_hm(char *out, size_t len, int min)
{
    if (min < 0) min = 0;
    if (min > 1440) min = 1440;
    snprintf(out, len, "%02u:%02u", (unsigned)(min / 60) % 100u, (unsigned)(min % 60));
}

typedef struct {
    int  y;        /* next free row in this column */
    int  hidden;   /* events that did not fit, all-day and timed */
    bool full;     /* a timed block did not fit; later ones are hidden too, in order */
} column_t;

static void draw_lanes(const app_gfx_surface_t *s, const app_ui_model_t *m, column_t *cols)
{
    int lane_end[MAX_LANES];
    int lanes_used = 0;
    for (int l = 0; l < MAX_LANES; l++) lane_end[l] = -1;

    for (size_t i = 0; i < m->n_events; i++) {
        const cal_event_t *e = &m->events[i];
        if (!is_lane_event(e)) continue;
        int first = e->start_day - m->today, last = e->end_day - m->today;
        if (last < 0 || first >= DAYS) continue;
        if (first < 0) first = 0;
        if (last >= DAYS) last = DAYS - 1;

        int lane = -1;
        for (int l = 0; l < MAX_LANES; l++) {
            if (lane_end[l] < first) { lane = l; break; }
        }
        if (lane < 0) {
            for (int c = first; c <= last; c++) cols[c].hidden++;
            continue;
        }
        lane_end[lane] = last;
        if (lane + 1 > lanes_used) lanes_used = lane + 1;

        app_gfx_colour_t bg = event_ink(m, e);
        int x0 = col_x(first) + PAD, x1 = col_x(last + 1) - PAD;
        int y = GRID_Y + lane * LANE_H;
        app_gfx_fill_rect(s, x0, y, x1 - x0, LANE_H - 2, bg);
        char line[LINE_MAX];
        fit_one_line(&app_gfx_font_label, e->title, x1 - x0 - 2 * PAD, line, sizeof(line));
        app_gfx_text(s, x0 + PAD, y + 15, &app_gfx_font_label, line, ink_on(bg));
    }
    for (int c = 0; c < DAYS; c++) {
        cols[c].y = GRID_Y + lanes_used * LANE_H + (lanes_used ? 2 : 0);
    }
}

static void draw_timed(const app_gfx_surface_t *s, const app_ui_model_t *m, column_t *cols)
{
    const int inner = W / DAYS - 2 * PAD;
    for (size_t i = 0; i < m->n_events; i++) {
        const cal_event_t *e = &m->events[i];
        if (is_lane_event(e)) continue;
        for (int32_t day = e->start_day; day <= e->end_day; day++) {
            int c = day - m->today;
            if (c < 0 || c >= DAYS) continue;
            column_t *col = &cols[c];

            char when[24], b[8];
            if (day == e->start_day) {
                fmt_hm(when, sizeof(when), e->start_min);
            } else {
                fmt_hm(b, sizeof(b), e->end_min);
                snprintf(when, sizeof(when), "until %s", b);
            }
            char lines[TITLE_LINES][LINE_MAX];
            size_t n = app_ui_wrap(&app_gfx_font_label, e->title, inner - 2 * PAD, lines[0],
                                   LINE_MAX, TITLE_LINES);
            int h = PAD + 13 + (int)n * 17 + PAD + 1;
            /* Keep room for the "+N more" line rather than letting the last
             * block run into the footer. */
            if (col->full || col->y + h > FOOT_Y - 18) {
                col->full = true;
                col->hidden++;
                continue;
            }

            app_gfx_colour_t cc = event_ink(m, e);
            bool past = (day == m->today && e->end_day == m->today && e->end_min <= m->now_min);
            int x = col_x(c) + PAD;
            app_gfx_colour_t ink;
            if (past) {
                /* Finished today: outline only, so what is still to come
                 * stands out without the morning's events vanishing. */
                app_gfx_rect(s, x, col->y, inner, h, cc);
                app_gfx_rect(s, x + 1, col->y + 1, inner - 2, h - 2, cc);
                ink = APP_GFX_BLACK;
            } else {
                app_gfx_fill_rect(s, x, col->y, inner, h, cc);
                ink = ink_on(cc);
            }
            app_gfx_text(s, x + PAD, col->y + PAD + 10, &app_gfx_font_small, when, ink);
            for (size_t l = 0; l < n; l++) {
                app_gfx_text(s, x + PAD, col->y + PAD + 13 + 13 + (int)l * 17,
                             &app_gfx_font_label, lines[l], ink);
            }
            col->y += h + 4;
        }
    }
}

static void draw_week(const app_gfx_surface_t *s, const app_ui_model_t *m, bool vol)
{
    draw_header(s, m, vol);

    for (int c = 0; c < DAYS; c++) {
        int32_t day = m->today + c;
        int y, mo, d;
        civil(day, &y, &mo, &d);
        char t[16];
        snprintf(t, sizeof(t), "%s %d", WDAY_SHORT[weekday(day)], d);
        int x0 = col_x(c), x1 = col_x(c + 1);
        int cx = (x0 + x1) / 2;
        if (c == 0) {
            app_gfx_fill_rect(s, x0 + 2, COLHEAD_Y, x1 - x0 - 4, COLHEAD_H, APP_GFX_RED);
            text_center(s, cx, COLHEAD_Y + 19, &app_gfx_font_label, t, APP_GFX_WHITE);
        } else {
            int wd = weekday(day);
            text_center(s, cx, COLHEAD_Y + 19, &app_gfx_font_label, t,
                        (wd == 0 || wd == 6) ? APP_GFX_BLUE : APP_GFX_BLACK);
        }
        if (c > 0) {
            app_gfx_vline(s, x0, COLHEAD_Y, FOOT_Y - 4, APP_GFX_BLACK);
        }
    }
    app_gfx_hline(s, 0, W - 1, COLHEAD_Y + COLHEAD_H + 1, APP_GFX_BLACK);

    column_t cols[DAYS] = { 0 };
    draw_lanes(s, m, cols);
    draw_timed(s, m, cols);

    for (int c = 0; c < DAYS; c++) {
        if (cols[c].hidden) {
            char t[16];
            snprintf(t, sizeof(t), "+%d more", cols[c].hidden);
            text_center(s, (col_x(c) + col_x(c + 1)) / 2, FOOT_Y - 6, &app_gfx_font_small, t,
                        APP_GFX_BLACK);
        }
    }

    app_gfx_hline(s, 0, W - 1, FOOT_Y, APP_GFX_BLACK);
    int x = 14;
    for (size_t i = 0; i < m->n_calendars; i++) {
        app_gfx_fill_rect(s, x, FOOT_Y + 6, 12, 12, m->calendars[i].colour);
        if (m->calendars[i].colour == APP_GFX_WHITE || m->calendars[i].colour == APP_GFX_YELLOW) {
            app_gfx_rect(s, x, FOOT_Y + 6, 12, 12, APP_GFX_BLACK);
        }
        x += 18;
        app_gfx_text(s, x, FOOT_Y + 17, &app_gfx_font_small, m->calendars[i].name, APP_GFX_BLACK);
        x += app_gfx_text_width(&app_gfx_font_small, m->calendars[i].name) + 18;
    }
    if (m->stale) {
        char t[48];
        if (m->stale_since[0]) {
            snprintf(t, sizeof(t), "Offline \xE2\x80\x93 last updated %s", m->stale_since);
        } else {
            snprintf(t, sizeof(t), "Offline \xE2\x80\x93 some events missing");
        }
        text_right(s, W - 14, FOOT_Y + 17, &app_gfx_font_label, t, APP_GFX_RED);
    } else if (m->version) {
        text_right(s, W - 14, FOOT_Y + 17, &app_gfx_font_small, m->version, APP_GFX_BLACK);
    }
}

/* ------------------------------------------------------------------ setup screens */

static void draw_setup(const app_gfx_surface_t *s, const app_ui_model_t *m)
{
    app_gfx_text(s, 40, 70, &app_gfx_font_head, "Calendar setup", APP_GFX_RED);
    app_gfx_text(s, 40, 130, &app_gfx_font_body, "1.  On a phone, join this Wi-Fi network:",
                 APP_GFX_BLACK);
    app_gfx_text(s, 80, 170, &app_gfx_font_head, m->ap_ssid ? m->ap_ssid : "", APP_GFX_BLUE);
    app_gfx_text(s, 40, 220, &app_gfx_font_body, "    Passphrase:", APP_GFX_BLACK);
    app_gfx_text(s, 80, 260, &app_gfx_font_head, m->ap_pass ? m->ap_pass : "", APP_GFX_BLUE);
    app_gfx_text(s, 40, 320, &app_gfx_font_body,
                 "2.  Pick your home network on the page that opens.", APP_GFX_BLACK);
    app_gfx_text(s, 40, 350, &app_gfx_font_body,
                 "     If no page opens, browse to http://192.168.4.1/", APP_GFX_BLACK);
    app_gfx_text(s, 40, 400, &app_gfx_font_body,
                 "The setup network closes after 15 minutes. Hold KEY for 3 s to open it again.",
                 APP_GFX_BLACK);
    if (m->version) text_right(s, W - 14, H - 10, &app_gfx_font_small, m->version, APP_GFX_BLACK);
}

static void draw_no_calendars(const app_gfx_surface_t *s, const app_ui_model_t *m)
{
    app_gfx_text(s, 40, 70, &app_gfx_font_head, "Add a calendar", APP_GFX_RED);
    app_gfx_text(s, 40, 130, &app_gfx_font_body, "On a computer or phone on the same network, open:",
                 APP_GFX_BLACK);
    app_gfx_text(s, 80, 175, &app_gfx_font_head, m->url ? m->url : "", APP_GFX_BLUE);
    app_gfx_text(s, 40, 230, &app_gfx_font_body,
                 "and paste a calendar's \"Secret address in iCal format\".", APP_GFX_BLACK);
    app_gfx_text(s, 40, 265, &app_gfx_font_body,
                 "Google Calendar: Settings \xE2\x80\xA2 your calendar \xE2\x80\xA2 Integrate calendar.",
                 APP_GFX_BLACK);
    app_gfx_text(s, 40, 330, &app_gfx_font_body,
                 "The page stays reachable for 15 minutes. Hold KEY for 3 s to open it again.",
                 APP_GFX_BLACK);
    if (m->version) text_right(s, W - 14, H - 10, &app_gfx_font_small, m->version, APP_GFX_BLACK);
}

void app_ui_render(const app_gfx_surface_t *surface, const app_ui_model_t *model,
                   bool with_volatile)
{
    app_gfx_clear(surface, APP_GFX_WHITE);
    switch (model->mode) {
    case APP_UI_SETUP:        draw_setup(surface, model); break;
    case APP_UI_NO_CALENDARS: draw_no_calendars(surface, model); break;
    case APP_UI_WEEK:
    default:                  draw_week(surface, model, with_volatile); break;
    }
}
