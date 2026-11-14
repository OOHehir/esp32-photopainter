#include <stdlib.h>

#include "cal_parse.h"
#include "cal_test_util.h"
#include "test.h"

/* The two weeks Mon 2026-10-19 00:00 to Mon 2026-11-02 00:00, Dublin time.
 * They straddle the end of Irish Summer Time on Sunday 2026-10-25 and the end of US DST
 * on 2026-11-01, so every fixture event that keeps its wall clock across a DST change
 * shows it. Expected tables come from Python's zoneinfo, not from this code. */
#define WEEK_FROM 1792364400LL
#define WEEK_TO   1793577600LL

static cal_tz_t dublin;

static cal_window_t two_weeks(void)
{
    cal_tz_parse_posix("GMT0IST,M3.5.0/1,M10.5.0", &dublin);
    return (cal_window_t){WEEK_FROM, WEEK_TO, &dublin, 2, false};
}

static const cal_want_t GOOGLE_WANT[] = {
    {"Gym", 20745, 420, 20745, 480, false, 1792389600LL},
    {"Medication", 20745, 510, 20745, 520, false, 1792395000LL},
    {"Team standup", 20745, 540, 20745, 555, false, 1792396800LL},
    {"Medication", 20746, 510, 20746, 520, false, 1792481400LL},
    {"(No title)", 20746, 720, 20746, 780, false, 1792494000LL},
    {"Medication", 20747, 510, 20747, 520, false, 1792567800LL},
    {"Flight EI 154", 20747, 1110, 20747, 1230, false, 1792603800LL},
    {"Dentist", 20748, 0, 20748, 0, true, 1792623600LL},
    {"Medication", 20748, 510, 20748, 520, false, 1792654200LL},
    {"Film night", 20748, 1260, 20748, 1440, false, 1792699200LL},
    {"Gym", 20749, 420, 20749, 480, false, 1792735200LL},
    {"Medication", 20749, 510, 20749, 520, false, 1792740600LL},
    {"Late gig", 20749, 1320, 20750, 90, false, 1792789200LL},
    {"Medication", 20750, 510, 20750, 520, false, 1792827000LL},
    {"Swim lessons (moved)", 20750, 840, 20750, 900, false, 1792846800LL},
    {"Medication", 20751, 510, 20751, 520, false, 1792917000LL},
    {"Medication", 20752, 510, 20752, 520, false, 1793003400LL},
    {"Team standup", 20752, 540, 20752, 555, false, 1793005200LL},
    {"Web Summit", 20753, 0, 20755, 0, true, 1793059200LL},
    {"Medication", 20753, 510, 20753, 520, false, 1793089800LL},
    {"Medication", 20754, 510, 20754, 520, false, 1793176200LL},
    {"Medication", 20755, 510, 20755, 520, false, 1793262600LL},
    {"Mam's birthday", 20756, 0, 20756, 0, true, 1793318400LL},
    {"Medication", 20756, 510, 20756, 520, false, 1793349000LL},
    {"Dinner, drinks ; chat with Ann \\o/", 20756, 1170, 20756, 1320, false, 1793388600LL},
    {"Medication", 20757, 510, 20757, 520, false, 1793435400LL},
    {"Medication", 20758, 510, 20758, 520, false, 1793521800LL},
};
#define N_GOOGLE (sizeof(GOOGLE_WANT) / sizeof(GOOGLE_WANT[0]))

static char *google_crlf(size_t *len)
{
    size_t raw_len;
    char *raw = cal_test_load("fixtures/google_dublin.ics", &raw_len);
    CHECK(raw != NULL, "cannot open fixtures/google_dublin.ics (run from test/host)");
    if (raw == NULL) {
        return NULL;
    }
    char *crlf = cal_test_crlf(raw, raw_len, len);
    free(raw);
    return crlf;
}

/* Covers, by row: weekly 09:00 standup staying 09:00 across the 25th, a
 * UTC-stored flight, all-day single and multi-day with exclusive DTEND, INTERVAL=2 with
 * an EXDATE (no Gym on the 21st or 26th), a daily series from 2010, a moved instance
 * and a cancelled one (no Swim lessons at 10:00 on the 24th or 31st), crossing and ending
 * at midnight, a yearly all-day birthday, a missing SUMMARY, escapes, VALARM SUMMARY not
 * replacing the title, a cancelled event and 2015 history left out. */
static void test_google_feed_expands_to_the_two_weeks_shown(void)
{
    size_t len;
    char *ics = google_crlf(&len);
    if (ics == NULL) {
        return;
    }
    const cal_window_t w = two_weeks();
    cal_event_t out[64];
    size_t n;
    cal_stats_t st;

    const int rc = cal_test_run(ics, len, len, &w, out, 64, &n, &st);

    CHECK_INT(rc, 0);
    cal_test_expect("google", out, n, GOOGLE_WANT, N_GOOGLE);
    CHECK_INT(out[0].calendar, 2);
    CHECK_INT(st.vevents, 17);
    CHECK_INT(st.instances, 27);
    CHECK_INT(st.overrides, 2);
    CHECK_INT(st.cancelled, 2);
    CHECK_INT(st.unsupported_rrule, 0);
    CHECK_INT(st.tz_fallback, 0);
    CHECK_INT(st.lines_truncated, 0);
    CHECK_INT(st.dropped_full, 0);
    free(ics);
}

static void test_bare_lf_feed_parses_like_crlf(void)
{
    size_t len;
    char *ics = cal_test_load("fixtures/google_dublin.ics", &len);
    CHECK(ics != NULL, "cannot open fixture");
    if (ics == NULL) {
        return;
    }
    const cal_window_t w = two_weeks();
    cal_event_t out[64];
    size_t n;

    cal_test_run(ics, len, len, &w, out, 64, &n, NULL);

    cal_test_expect("google LF", out, n, GOOGLE_WANT, N_GOOGLE);
    free(ics);
}

static bool same_events(const cal_event_t *a, const cal_event_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i].start_day != b[i].start_day || a[i].end_day != b[i].end_day ||
            a[i].start_min != b[i].start_min || a[i].end_min != b[i].end_min ||
            a[i].all_day != b[i].all_day || a[i].start_utc != b[i].start_utc ||
            strcmp(a[i].title, b[i].title) != 0) {
            return false;
        }
    }
    return true;
}

/* Splits land inside CRLF pairs, folds, parameters and the UTF-8 bytes of
 * the overlong-line case appended below, at every offset. */
static void test_every_chunk_size_from_1_to_64_matches_one_feed(void)
{
    size_t len;
    char *base = google_crlf(&len);
    if (base == NULL) {
        return;
    }
    /* An extra event with a multi-byte title and a folded, overlong line. */
    static const char extra_head[] =
        "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:x\r\nDESCRIPTION:";
    static const char extra_tail[] =
        "\r\nSUMMARY:Caf\xc3\xa9 \xe2\x98\x95 with \xf0\x9f\x90\x88\r\n"
        "DTSTART;TZID=Europe/Dublin:20261028T110000\r\n"
        "DTEND;TZID=Europe/Dublin:20261028T113000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    const size_t desc = 6000;
    const size_t total = len + sizeof(extra_head) - 1 + desc * 3 / 2 + sizeof(extra_tail);
    char *ics = malloc(total + 1);
    size_t at = 0;
    memcpy(ics, base, len);
    at = len;
    memcpy(ics + at, extra_head, sizeof(extra_head) - 1);
    at += sizeof(extra_head) - 1;
    for (size_t i = 0; i < desc; i++) {
        ics[at++] = (char)('a' + i % 26);
        if (i % 70 == 69) {
            memcpy(ics + at, "\r\n ", 3);
            at += 3;
        }
    }
    memcpy(ics + at, extra_tail, sizeof(extra_tail) - 1);
    at += sizeof(extra_tail) - 1;
    free(base);

    const cal_window_t w = two_weeks();
    cal_event_t ref[64], got[64];
    size_t n_ref, n_got;
    cal_stats_t st_ref, st_got;
    cal_test_run(ics, at, at, &w, ref, 64, &n_ref, &st_ref);
    CHECK_INT(n_ref, N_GOOGLE + 1);
    CHECK_INT(st_ref.lines_truncated, 1);

    for (size_t chunk = 1; chunk <= 64; chunk++) {
        memset(got, 0, sizeof(got));
        cal_test_run(ics, at, chunk, &w, got, 64, &n_got, &st_got);
        CHECK(n_got == n_ref && same_events(got, ref, n_ref) &&
                  memcmp(&st_got, &st_ref, sizeof(st_ref)) == 0,
              "chunk size %zu: %zu events, differs from the single feed", chunk, n_got);
    }
    free(ics);
}

static void test_outlook_feed_with_windows_zone_names(void)
{
    size_t raw_len, len;
    char *raw = cal_test_load("fixtures/outlook_work.ics", &raw_len);
    CHECK(raw != NULL, "cannot open fixtures/outlook_work.ics");
    if (raw == NULL) {
        return;
    }
    char *ics = cal_test_crlf(raw, raw_len, &len);
    free(raw);
    const cal_window_t w = two_weeks();
    cal_event_t out[16];
    size_t n;
    cal_stats_t st;

    cal_test_run(ics, len, len, &w, out, 16, &n, &st);

    /* Boston 11:00 is 16:00 in Dublin on the 21st and 15:00 on the 28th,
     * when Ireland has left summer time and the US has not. The month-end report is the
     * last weekday, Friday the 30th, not Saturday the 31st. Weekend cover is on the
     * last summer-time Saturday, so it shows where the 1601 DTSTART's 02:00 lands. */
    static const cal_want_t want[] = {
        {"Sync with Boston", 20747, 960, 20747, 990, false, 1792594800LL},
        {"Weekend cover", 20750, 720, 20750, 780, false, 1792839600LL},
        {"October bank holiday", 20752, 0, 20752, 0, true, 1792972800LL},
        {"Sync with Boston", 20754, 900, 20754, 930, false, 1793199600LL},
        {"Month-end report", 20756, 960, 20756, 1020, false, 1793376000LL},
    };
    cal_test_expect("outlook", out, n, want, 5);
    CHECK_INT(st.tz_fallback, 0);
    free(ics);
}

static void run_inline(const char *ics, cal_event_t *out, size_t cap, size_t *n,
                       cal_stats_t *st)
{
    const cal_window_t w = two_weeks();
    cal_test_run(ics, strlen(ics), strlen(ics), &w, out, cap, n, st);
}

static void test_vtimezone_after_the_event_using_it_still_applies(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\nUID:a\nSUMMARY:Call NY\n"
        "DTSTART;TZID=America/New_York:20261028T110000\n"
        "DTEND;TZID=America/New_York:20261028T120000\nEND:VEVENT\n"
        "BEGIN:VTIMEZONE\nTZID:America/New_York\n"
        "BEGIN:DAYLIGHT\nTZOFFSETFROM:-0500\nTZOFFSETTO:-0400\nDTSTART:19700308T020000\n"
        "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\nEND:DAYLIGHT\n"
        "BEGIN:STANDARD\nTZOFFSETFROM:-0400\nTZOFFSETTO:-0500\nDTSTART:19701101T020000\n"
        "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\nEND:STANDARD\n"
        "END:VTIMEZONE\nEND:VCALENDAR\n";
    cal_event_t out[4];
    size_t n;
    cal_stats_t st;

    run_inline(ics, out, 4, &n, &st);

    static const cal_want_t want[] = {
        {"Call NY", 20754, 900, 20754, 960, false, 1793199600LL},
    };
    cal_test_expect("vtimezone after use", out, n, want, 1);
    CHECK_INT(st.tz_fallback, 0);
}

static void test_unknown_tzid_falls_back_to_device_zone(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nSUMMARY:Mars time\n"
        "DTSTART;TZID=Mars/Olympus_Mons:20261028T110000\n"
        "DTEND;TZID=Mars/Olympus_Mons:20261028T120000\nEND:VEVENT\nEND:VCALENDAR\n";
    cal_event_t out[4];
    size_t n;
    cal_stats_t st;

    run_inline(ics, out, 4, &n, &st);

    static const cal_want_t want[] = {
        {"Mars time", 20754, 660, 20754, 720, false, 1793185200LL},
    };
    cal_test_expect("unknown tzid", out, n, want, 1);
    CHECK_INT(st.tz_fallback, 1);
}

static void test_overlong_line_is_cut_without_derailing_the_next(void)
{
    const size_t big = 3 * CAL_LINE_MAX;
    static const char head[] = "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nDESCRIPTION:";
    static const char tail[] =
        "\nSUMMARY:After the long line\nDTSTART:20261028T110000Z\n"
        "DTEND:20261028T120000Z\nEND:VEVENT\nEND:VCALENDAR\n";
    char *ics = malloc(sizeof(head) + big + sizeof(tail));
    strcpy(ics, head);
    /* ':' and ';' in the cut-off part must not be read as a new property. */
    for (size_t i = 0; i < big; i++) {
        ics[sizeof(head) - 1 + i] = "x:;\\"[i % 4];
    }
    strcpy(ics + sizeof(head) - 1 + big, tail);
    cal_event_t out[4];
    size_t n;
    cal_stats_t st;

    run_inline(ics, out, 4, &n, &st);

    CHECK_INT(n, 1);
    CHECK_STR(out[0].title, "After the long line");
    CHECK_INT(st.lines_truncated, 1);
    free(ics);
}

static void test_cut_exdate_list_drops_only_its_partial_last_item(void)
{
    /* A cut "20261028T110000Z" can end as "20261028", a valid DATE; that
     * fragment must not remove the 28th. */
    char ics[2 * CAL_LINE_MAX];
    int at = snprintf(ics, sizeof(ics),
                      "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nSUMMARY:Daily\n"
                      "DTSTART:20261019T110000Z\nRRULE:FREQ=DAILY;COUNT=10\nEXDATE:");
    const int prefix = (int)strlen("EXDATE:");
    const int line_start = at - prefix;
    while (at - line_start + 17 < CAL_LINE_MAX - 1) {
        at += snprintf(ics + at, sizeof(ics) - (size_t)at, "20261020T110000Z,");
    }
    at += snprintf(ics + at, sizeof(ics) - (size_t)at, "20261028T110000Z\nEND:VEVENT\nEND:VCALENDAR\n");
    cal_event_t out[16];
    size_t n;
    cal_stats_t st;

    run_inline(ics, out, 16, &n, &st);

    CHECK_INT(st.lines_truncated, 1);
    CHECK_INT(n, 9);
    CHECK_INT(out[0].start_day, 20745);
    CHECK_INT(out[1].start_day, 20747);  /* the 20th is excluded */
    CHECK_INT(out[8].start_day, 20754);  /* the 28th is not */
}

static void test_cap_keeps_the_earliest_and_counts_the_rest(void)
{
    size_t len;
    char *ics = google_crlf(&len);
    if (ics == NULL) {
        return;
    }
    const cal_window_t w = two_weeks();
    cal_event_t out[5];
    size_t n;
    cal_stats_t st;

    cal_test_run(ics, len, len, &w, out, 5, &n, &st);

    cal_test_expect("capped", out, n, GOOGLE_WANT, 5);
    CHECK_INT(st.instances, 27);
    CHECK_INT(st.dropped_full, 22);
    free(ics);
}

static void title_of(const char *summary, char *title)
{
    char ics[8192];
    snprintf(ics, sizeof(ics),
             "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:a\nSUMMARY:%s\n"
             "DTSTART:20261028T110000Z\nEND:VEVENT\nEND:VCALENDAR\n", summary);
    cal_event_t out[1];
    size_t n;
    run_inline(ics, out, 1, &n, NULL);
    CHECK_INT(n, 1);
    strcpy(title, n == 1 ? out[0].title : "");
}

static void test_title_cut_at_a_utf8_boundary(void)
{
    char a62[63], a61[62];
    memset(a62, 'a', 62);
    a62[62] = '\0';
    memset(a61, 'a', 61);
    a61[61] = '\0';
    char summary[128], title[CAL_TITLE_MAX];

    /* 62 + 2 bytes: the e-acute would straddle byte 63, so it goes. */
    snprintf(summary, sizeof(summary), "%s\xc3\xa9", a62);
    title_of(summary, title);
    CHECK_STR(title, a62);

    /* 61 + 2 bytes fit exactly. */
    snprintf(summary, sizeof(summary), "%s\xc3\xa9", a61);
    title_of(summary, title);
    char want[64];
    snprintf(want, sizeof(want), "%s\xc3\xa9", a61);
    CHECK_STR(title, want);

    /* 4-byte emoji at bytes 60..63 is dropped whole. */
    snprintf(summary, sizeof(summary), "%.60s\xf0\x9f\x90\x88zz", a62);
    title_of(summary, title);
    CHECK_INT(strlen(title), 60);
}

static void test_summary_escapes_and_folds_are_undone(void)
{
    char title[CAL_TITLE_MAX];
    title_of("Tea\\, cake\\; \\\\ and\\Nchat", title);
    CHECK_STR(title, "Tea, cake; \\ and chat");
    title_of("Split \n across\n\t fold", title);
    CHECK_STR(title, "Split across fold");
    title_of("", title);
    CHECK_STR(title, "(No title)");
}

static void test_feed_without_final_newline_keeps_last_event(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nSUMMARY:Last\r\n"
        "DTSTART:20261028T110000Z\r\nEND:VEVENT";
    cal_event_t out[2];
    size_t n;

    run_inline(ics, out, 2, &n, NULL);

    CHECK_INT(n, 1);
}

static void test_durations_set_the_end(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\nUID:a\nSUMMARY:Ninety\nDTSTART:20261028T110000Z\n"
        "DURATION:PT1H30M\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:b\nSUMMARY:Week\nDTSTART;VALUE=DATE:20261020\n"
        "DURATION:P1W\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:c\nSUMMARY:Instant\nDTSTART:20261021T090000Z\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:d\nSUMMARY:No end\nDTSTART;VALUE=DATE:20261022\nEND:VEVENT\n"
        "END:VCALENDAR\n";
    cal_event_t out[8];
    size_t n;

    run_inline(ics, out, 8, &n, NULL);

    static const cal_want_t want[] = {
        {"Week", 20746, 0, 20752, 0, true, 1792450800LL},
        {"Instant", 20747, 600, 20747, 600, false, 1792573200LL},
        {"No end", 20748, 0, 20748, 0, true, 1792623600LL},
        {"Ninety", 20754, 660, 20754, 750, false, 1793185200LL},
    };
    cal_test_expect("durations", out, n, want, 4);
}

static void test_event_outside_window_or_touching_its_edge_is_left_out(void)
{
    /* Ends exactly at the window start: [from, to) does not include it. */
    static const char ics[] =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\nUID:a\nSUMMARY:Before\nDTSTART:20261018T220000Z\n"
        "DTEND:20261018T230000Z\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:b\nSUMMARY:After\nDTSTART:20261102T000000Z\n"
        "DTEND:20261102T010000Z\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:c\nSUMMARY:Day before\nDTSTART;VALUE=DATE:20261018\n"
        "DTEND;VALUE=DATE:20261019\nEND:VEVENT\n"
        "END:VCALENDAR\n";
    cal_event_t out[4];
    size_t n;

    run_inline(ics, out, 4, &n, NULL);

    CHECK_INT(n, 0);
}

/* An HTTP body can stop anywhere, including mid-line and mid-VEVENT. The
 * parser must still finish cleanly (ASan watches the partial state) and never emit more
 * than the whole feed would; a cut before a cancelling override can add one instance. */
static void test_feed_cut_off_anywhere_finishes_cleanly(void)
{
    size_t len;
    char *ics = google_crlf(&len);
    if (ics == NULL) {
        return;
    }
    const cal_window_t w = two_weeks();
    cal_event_t out[64];
    size_t n;
    bool all_ok = true;

    for (size_t cut = 0; cut < len; cut += 7) {
        const int rc = cal_test_run(ics, cut, cut ? cut : 1, &w, out, 64, &n, NULL);
        all_ok = all_ok && rc == 0 && n <= N_GOOGLE + 1;
    }

    CHECK(all_ok, "a truncated feed failed or produced extra events");
    free(ics);
}

/* A public weekly series whose second instance was made private, plus a
 * private one-off. */
static const char PRIVATE_ICS[] =
    "BEGIN:VCALENDAR\n"
    "BEGIN:VEVENT\nUID:s\nSUMMARY:Series\nDTSTART:20261019T090000Z\n"
    "DTEND:20261019T100000Z\nRRULE:FREQ=WEEKLY;COUNT=2\nEND:VEVENT\n"
    "BEGIN:VEVENT\nUID:s\nSUMMARY:Moved\nRECURRENCE-ID:20261026T090000Z\n"
    "DTSTART:20261026T120000Z\nDTEND:20261026T130000Z\nCLASS:PRIVATE\nEND:VEVENT\n"
    "BEGIN:VEVENT\nUID:p\nSUMMARY:Secret\nCLASS:CONFIDENTIAL\n"
    "DTSTART:20261020T090000Z\nEND:VEVENT\n"
    "END:VCALENDAR\n";

static void test_private_events_hidden_when_asked(void)
{
    cal_window_t w = two_weeks();
    w.hide_private = true;
    cal_event_t out[8];
    size_t n = 0;
    cal_stats_t st;

    cal_test_run(PRIVATE_ICS, strlen(PRIVATE_ICS), strlen(PRIVATE_ICS), &w, out, 8, &n, &st);

    CHECK_INT(n, 1);
    CHECK_STR(out[0].title, "Series");
    CHECK_INT(out[0].start_day, 20745);
    CHECK_INT(st.hidden_private, 2);
}

static void test_private_events_shown_by_default(void)
{
    cal_event_t out[8];
    size_t n = 0;
    cal_stats_t st;

    run_inline(PRIVATE_ICS, out, 8, &n, &st);

    CHECK_INT(n, 3);
    CHECK_INT(st.hidden_private, 0);
}

static void test_colour_names_and_hex_map_to_panel_inks(void)
{
    CHECK_INT(cal_colour_to_ink("red"), 3);
    CHECK_INT(cal_colour_to_ink("RoyalBlue"), 5);
    CHECK_INT(cal_colour_to_ink("teal"), 6);
    CHECK_INT(cal_colour_to_ink("gold"), 2);
    CHECK_INT(cal_colour_to_ink("#d50000"), 3);   /* Google's "Tomato" */
    CHECK_INT(cal_colour_to_ink("#0b8043"), 6);   /* Google's "Basil" */
    CHECK_INT(cal_colour_to_ink("#3f51b5"), 5);   /* Google's "Blueberry" */
    CHECK_INT(cal_colour_to_ink("chartreuse-ish"), -1);
    CHECK_INT(cal_colour_to_ink("#12345"), -1);
}

static void test_event_colour_is_carried_to_its_instances(void)
{
    static const char ics[] =
        "BEGIN:VCALENDAR\n"
        "BEGIN:VEVENT\nUID:c\nSUMMARY:Tinted\nCOLOR:forestgreen\n"
        "DTSTART:20261020T090000Z\nRRULE:FREQ=DAILY;COUNT=2\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:d\nSUMMARY:Plain\nDTSTART:20261020T100000Z\nEND:VEVENT\n"
        "END:VCALENDAR\n";
    cal_event_t out[4];
    size_t n = 0;
    cal_stats_t st;

    run_inline(ics, out, 4, &n, &st);

    CHECK_INT(n, 3);
    CHECK_INT(out[0].ink, 6);
    CHECK_INT(out[1].ink, -1);
    CHECK_INT(out[2].ink, 6);
    CHECK_INT(st.with_colour, 1);
}

void test_cal_parse(void)
{
    SUITE("cal_parse -- feeds, streaming and output");
    test_google_feed_expands_to_the_two_weeks_shown();
    test_bare_lf_feed_parses_like_crlf();
    test_every_chunk_size_from_1_to_64_matches_one_feed();
    test_outlook_feed_with_windows_zone_names();
    test_vtimezone_after_the_event_using_it_still_applies();
    test_unknown_tzid_falls_back_to_device_zone();
    test_overlong_line_is_cut_without_derailing_the_next();
    test_cut_exdate_list_drops_only_its_partial_last_item();
    test_cap_keeps_the_earliest_and_counts_the_rest();
    test_title_cut_at_a_utf8_boundary();
    test_summary_escapes_and_folds_are_undone();
    test_feed_without_final_newline_keeps_last_event();
    test_durations_set_the_end();
    test_event_outside_window_or_touching_its_edge_is_left_out();
    test_feed_cut_off_anywhere_finishes_cleanly();
    test_private_events_hidden_when_asked();
    test_private_events_shown_by_default();
    test_colour_names_and_hex_map_to_panel_inks();
    test_event_colour_is_carried_to_its_instances();
}
