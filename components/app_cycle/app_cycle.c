#include "app_cycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_board.h"
#include "app_cal.h"
#include "app_cfg.h"
#include "app_core.h"
#include "app_diag.h"
#include "app_display.h"
#include "app_epd.h"
#include "app_httpd.h"
#include "app_net.h"
#include "app_ota.h"
#include "cycle_sched.h"
#include "driver/rtc_io.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "app_cycle";

#define CONNECT_WAIT_MS   30000
#define TIME_WAIT_MS      15000
#define OTA_WAIT_MS       (10 * 60 * 1000)
#define KEY_LONG_MS       3000
#define MANUAL_GAP_S      180
#define DAYS_SHOWN        7
/* 2025-01-01. A clock before this has never been set, so neither the week
 * nor a certificate's validity can be judged. */
#define CLOCK_VALID_UTC   1735689600LL
#define RTC_MAGIC         0x43414C31u

#define BIT_REFRESH  (1u << 0)
#define BIT_CONFIG   (1u << 1)

/* Survives deep sleep, not power loss; the magic tells the two apart. */
typedef struct {
    uint32_t magic;
    uint32_t wakes;
    int64_t  last_ota_utc;
} rtc_state_t;
static RTC_DATA_ATTR rtc_state_t s_rtc;

static EventGroupHandle_t s_events;
static cal_tz_t s_tz;
static app_cal_week_t s_week;
static esp_err_t s_week_err = ESP_ERR_NOT_FOUND;
static int64_t s_awake_until_us;
static int64_t s_last_manual_us;
static app_board_power_t s_power;
static const char *s_mode = "starting";

/* ------------------------------------------------------------------ httpd hooks */

static uint32_t hook_refresh(void)
{
    int64_t now = esp_timer_get_time();
    int64_t gap = (now - s_last_manual_us) / 1000000;
    if (s_last_manual_us && gap < MANUAL_GAP_S) {
        return (uint32_t)(MANUAL_GAP_S - gap);
    }
    s_last_manual_us = now;
    xEventGroupSetBits(s_events, BIT_REFRESH);
    return 0;
}

static esp_err_t hook_set_calendar(int index, const char *name, const char *colour,
                                   const char *url)
{
    int code = url[0] ? app_cal_colour_from_name(colour) : 0;
    if (index < 0 || code < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_cal_set_source((size_t)index, name, (uint8_t)code, url);
    if (err == ESP_OK) xEventGroupSetBits(s_events, BIT_CONFIG);
    return err;
}

static size_t json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, len - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

static int hook_status(char *buf, size_t len)
{
    int64_t left = (s_awake_until_us - esp_timer_get_time()) / 1000000;
    int n = snprintf(buf, len,
                     "\"mode\":\"%s\",\"events\":%u,\"stale\":%s,\"fetch_ms\":%u,"
                     "\"last_refresh_ms\":%u,\"battery_pct\":%d,\"battery_mv\":%u,"
                     "\"charging\":%s,\"awake_left_s\":%lld,\"calendars\":[",
                     s_mode, (unsigned)s_week.count, s_week.stale ? "true" : "false",
                     (unsigned)s_week.fetch_ms, (unsigned)app_epd_last_refresh_ms(),
                     s_power.battery_pct, s_power.battery_mv,
                     s_power.charging ? "true" : "false", (long long)(left > 0 ? left : 0));
    app_cal_source_t *src = malloc(sizeof(app_cal_source_t) * CAL_MAX_CALENDARS);
    size_t count = src ? app_cal_sources(src) : 0;
    for (size_t i = 0; i < count && n > 0 && (size_t)n < len; i++) {
        char name[64], host[64];
        json_str(name, sizeof(name), src[i].name);
        app_cal_url_host(src[i].url, host, sizeof(host));
        char hesc[80];
        json_str(hesc, sizeof(hesc), host);
        n += snprintf(buf + n, len - n, "%s{\"name\":\"%s\",\"colour\":\"%s\",\"host\":\"%s\","
                      "\"failed\":%s}", i ? "," : "", name,
                      app_cal_colour_name(src[i].colour), hesc,
                      (s_week.failed_mask >> i) & 1 ? "true" : "false");
    }
    free(src);
    if (n > 0 && (size_t)n < len) n += snprintf(buf + n, len - n, "]");
    return (n > 0 && (size_t)n < len) ? n : 0;
}

static const app_httpd_panel_hooks_t s_hooks = {
    .refresh = hook_refresh,
    .status_json = hook_status,
    .set_calendar = hook_set_calendar,
};

/* ------------------------------------------------------------------------ helpers */

static bool clock_valid(void) { return time(NULL) >= CLOCK_VALID_UTC; }

static bool provisioned(void) { return app_cfg_has(APP_CFG_KEY_WIFI_SSID); }

static bool wait_connected(uint32_t ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (app_net_state() != APP_NET_STATE_CONNECTED) {
        if (esp_timer_get_time() > end) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static int32_t local_day(int64_t utc, int16_t *minute)
{
    int64_t local = utc + cal_tz_offset_at_utc(&s_tz, utc);
    int64_t day = local / 86400;
    if (minute) *minute = (int16_t)((local - day * 86400) / 60);
    return (int32_t)day;
}

static void read_sensors(app_ui_model_t *m)
{
    app_board_power(&s_power);
    m->battery_pct = s_power.battery_pct;
    m->charging = s_power.charging;
    float t, rh;
    if (app_board_climate(&t, &rh) == ESP_OK) {
        m->have_climate = true;
        m->temp_c10 = (int16_t)(t * 10.0f + (t >= 0 ? 0.5f : -0.5f));
        m->rh_pct = (uint8_t)(rh + 0.5f);
    }
}

/* Fetch (when on the network with a valid clock) and draw. */
static void update(bool force)
{
    static app_ui_calendar_t cals[CAL_MAX_CALENDARS];
    static app_cal_source_t src[CAL_MAX_CALENDARS];
    static char url[48];
    size_t n_src = app_cal_sources(src);
    for (size_t i = 0; i < n_src; i++) {
        memcpy(cals[i].name, src[i].name, sizeof(cals[i].name) - 1);
        cals[i].name[sizeof(cals[i].name) - 1] = '\0';
        cals[i].colour = (app_gfx_colour_t)src[i].colour;
    }

    app_ui_model_t m = { .calendars = cals, .n_calendars = n_src, .battery_pct = -1 };
    const esp_app_desc_t *desc = esp_app_get_description();
    m.version = desc->version;
    read_sensors(&m);

    app_net_info_t net;
    app_net_get_info(&net);
    if (!provisioned()) {
        m.mode = APP_UI_SETUP;
        s_mode = "setup";
        m.ap_ssid = net.ap_ssid;
        m.ap_pass = app_cfg_ap_passphrase();
        app_display_show(&m, force);
        return;
    }
    if (n_src == 0) {
        snprintf(url, sizeof(url), "http://%s/", net.ip[0] ? net.ip : "?");
        m.mode = APP_UI_NO_CALENDARS;
        s_mode = "no calendars";
        m.url = url;
        app_display_show(&m, force);
        return;
    }
    if (!clock_valid()) {
        ESP_LOGW(TAG, "clock never set -- cannot place a week; panel left as it is");
        return;
    }

    int64_t now = time(NULL);
    m.mode = APP_UI_WEEK;
    s_mode = "week";
    m.today = local_day(now, &m.now_min);
    int64_t from = cal_tz_local_to_utc(&s_tz, (int64_t)m.today * 86400);
    int64_t to = cal_tz_local_to_utc(&s_tz, (int64_t)(m.today + DAYS_SHOWN) * 86400);

    /* Offline still goes through app_cal so the cached week is drawn -- with its
     * Offline marker -- rather than leaving yesterday on the glass after midnight. */
    s_week_err = app_cal_fetch(&s_tz, from, to, net.sta_connected, &s_week);
    if (s_week_err != ESP_OK) {
        ESP_LOGW(TAG, "no events and no cache (%s) -- panel left as it is",
                 esp_err_to_name(s_week_err));
        return;
    }
    m.events = s_week.events;
    m.n_events = s_week.count;
    m.stale = s_week.stale;
    if (m.stale && s_week.stale_since_utc) {
        static const char *const WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        int16_t min;
        int32_t day = local_day(s_week.stale_since_utc, &min);
        int wd = (int)(((day + 4) % 7 + 7) % 7);
        snprintf(m.stale_since, sizeof(m.stale_since), "%s%s%02u:%02u",
                 day == m.today ? "" : WD[wd], day == m.today ? "" : " ",
                 (unsigned)(min / 60) % 24u, (unsigned)(min % 60));
    }
    ESP_LOGI(TAG, "week: %u events%s, fetched in %u ms", (unsigned)s_week.count,
             m.stale ? " (stale)" : "", (unsigned)s_week.fetch_ms);
    app_display_show(&m, force);
}

static void maybe_ota(bool forced)
{
#if CONFIG_APP_OTA_ENABLE
    app_ota_info_t info;
    app_ota_get_info(&info);
    if (info.probation) {
        /* A freshly installed image must prove itself before it is kept;
         * sleeping now would leave it unconfirmed and the next reset would revert it. */
        ESP_LOGI(TAG, "new image on probation -- staying awake for its self-test");
        int64_t end = esp_timer_get_time() + (int64_t)OTA_WAIT_MS * 1000;
        do {
            vTaskDelay(pdMS_TO_TICKS(1000));
            app_ota_get_info(&info);
        } while (info.probation && esp_timer_get_time() < end);
        return;
    }
    int64_t now = time(NULL);
    if (!forced && s_rtc.last_ota_utc &&
        now - s_rtc.last_ota_utc < (int64_t)CONFIG_APP_CYCLE_OTA_EVERY_H * 3600) {
        return;
    }
    int64_t before = info.last_check_s;
    if (app_ota_check_now(false) != ESP_OK) return;
    s_rtc.last_ota_utc = now;
    int64_t end = esp_timer_get_time() + (int64_t)OTA_WAIT_MS * 1000;
    do {
        vTaskDelay(pdMS_TO_TICKS(500));
        app_ota_get_info(&info);
    } while (info.last_check_s == before && esp_timer_get_time() < end);
    ESP_LOGI(TAG, "update check: %s (%s)", app_ota_state_name(info.state), info.message);
#else
    (void)forced;
#endif
}

/* Classifies a KEY press that is already in progress. */
static bool key_long_press(void)
{
    int64_t t0 = esp_timer_get_time();
    while (app_board_key_held()) {
        if (esp_timer_get_time() - t0 >= (int64_t)KEY_LONG_MS * 1000) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

/* Stays awake until `until_us`, serving the web page and redrawing on
 * request, a calendar change or a KEY press. Returns early once `stop_when` holds. */
static void awake_window(int64_t until_us, bool (*stop_when)(void))
{
    s_awake_until_us = until_us;
    while (esp_timer_get_time() < s_awake_until_us) {
        if (stop_when && stop_when()) return;
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_REFRESH | BIT_CONFIG, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(100));
        if (app_board_key_held()) {
            bool lng = key_long_press();
            if (lng) {
                s_awake_until_us = esp_timer_get_time() +
                                   (int64_t)CONFIG_APP_CYCLE_AWAKE_WINDOW_S * 1000000;
                ESP_LOGI(TAG, "KEY held -- awake for %ds more", CONFIG_APP_CYCLE_AWAKE_WINDOW_S);
                maybe_ota(true);
            } else {
                b |= BIT_REFRESH;
            }
        }
        if (b & (BIT_REFRESH | BIT_CONFIG)) {
            update((b & BIT_REFRESH) != 0);
        }
    }
}

static bool ready_for_week(void)
{
    app_cal_source_t src[CAL_MAX_CALENDARS];
    return provisioned() && app_net_state() == APP_NET_STATE_CONNECTED &&
           app_cal_sources(src) > 0;
}

static void sleep_until(int64_t wake_utc)
{
    int64_t now = time(NULL);
    int64_t secs = clock_valid() ? wake_utc - now : (int64_t)CONFIG_APP_CYCLE_INTERVAL_MIN * 60;
    if (secs < 60) secs = 60;

    ESP_LOGI(TAG, "sleeping %llds (wake %u)", (long long)secs, (unsigned)s_rtc.wakes);
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    /* KEY pulls GPIO4 low; the internal pull-up keeps it high while the
     * digital domain is off, which needs RTC_PERIPH powered (a few uA). */
    rtc_gpio_pullup_en(APP_BOARD_KEY_GPIO);
    rtc_gpio_pulldown_dis(APP_BOARD_KEY_GPIO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io(1ULL << APP_BOARD_KEY_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

/* --------------------------------------------------------------------------- run */

void app_cycle_run(void)
{
    if (s_rtc.magic != RTC_MAGIC) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = RTC_MAGIC;
    }
    s_rtc.wakes++;
    s_events = xEventGroupCreate();

    uint32_t causes = esp_sleep_get_wakeup_causes();
    ESP_ERROR_CHECK(app_board_init());
    bool key_wake = causes & (1u << ESP_SLEEP_WAKEUP_EXT1);
    bool timer_wake = causes & (1u << ESP_SLEEP_WAKEUP_TIMER);
    bool maintenance = key_wake && key_long_press();
    bool cold = !key_wake && !timer_wake;
    ESP_LOGI(TAG, "wake %u: %s%s", (unsigned)s_rtc.wakes,
             key_wake ? "KEY" : timer_wake ? "timer" : "power-on/reset",
             maintenance ? " (held: staying awake)" : "");

    setenv("TZ", CONFIG_APP_CAL_TZ, 1);
    tzset();
    if (cal_tz_parse_posix(CONFIG_APP_CAL_TZ, &s_tz) != 0) {
        ESP_LOGE(TAG, "CONFIG_APP_CAL_TZ \"%s\" does not parse -- using UTC", CONFIG_APP_CAL_TZ);
        cal_tz_parse_posix("UTC0", &s_tz);
    }

    ESP_ERROR_CHECK(app_core_start());
    /* Not ESP_ERROR_CHECK: an abort here is a boot loop that never reaches deep sleep,
     * drains the battery and never brings up the web page or OTA that could fix it. */
    esp_err_t err = app_cal_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "app_cal_init: %s", esp_err_to_name(err));
    err = app_display_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "app_display_init: %s -- no panel this wake",
                                esp_err_to_name(err));
    app_httpd_set_panel_hooks(&s_hooks);

    int64_t window_us = (int64_t)CONFIG_APP_CYCLE_AWAKE_WINDOW_S * 1000000;

    if (!provisioned()) {
        update(false);
        awake_window(esp_timer_get_time() + window_us, ready_for_week);
    }
    if (provisioned()) {
        if (!wait_connected(CONNECT_WAIT_MS)) {
            ESP_LOGW(TAG, "no network after %ds", CONNECT_WAIT_MS / 1000);
        } else if (!clock_valid() || cold) {
            app_net_wait_time_sync(TIME_WAIT_MS);
        }
        update(key_wake && !maintenance);

        app_cal_source_t src[CAL_MAX_CALENDARS];
        if (app_cal_sources(src) == 0 && app_net_state() == APP_NET_STATE_CONNECTED) {
            awake_window(esp_timer_get_time() + window_us, ready_for_week);
            if (ready_for_week()) update(false);
        }
        if (app_net_state() == APP_NET_STATE_CONNECTED && clock_valid()) {
            maybe_ota(maintenance);
        }
    }
    if (maintenance || cold) {
        /* A power-on is someone at the device -- just plugged it in or
         * flashed it -- so the web page stays up for a while before the first sleep. */
        awake_window(esp_timer_get_time() + (maintenance ? window_us : 120 * 1000000LL), NULL);
    }

    for (;;) {
        int64_t wake = cycle_next_wake(time(NULL), &s_tz, CONFIG_APP_CYCLE_INTERVAL_MIN,
                                       CONFIG_APP_CYCLE_QUIET_START_H,
                                       CONFIG_APP_CYCLE_QUIET_END_H);
        app_board_power(&s_power);
#if !CONFIG_APP_CYCLE_SLEEP_ON_USB
        if (s_power.vbus) {
            int64_t secs = clock_valid() ? wake - time(NULL)
                                         : (int64_t)CONFIG_APP_CYCLE_INTERVAL_MIN * 60;
            ESP_LOGI(TAG, "on USB power -- staying awake, next redraw in %llds",
                     (long long)secs);
            awake_window(esp_timer_get_time() + secs * 1000000LL, NULL);
            update(false);
            maybe_ota(false);
            continue;
        }
#endif
        sleep_until(wake);
    }
}
