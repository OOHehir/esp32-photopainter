/* app_cal -- which calendars to show, fetching them, and the offline cache.
 *
 * A calendar is a Google "Secret address in iCal format" (any iCalendar URL works). The
 * URL is a credential: anyone holding it can read the calendar. It is kept in NVS, never
 * logged in full, and the status API returns only its host.
 *
 * The parsing itself is cal_parse.c, which is pure C and tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cal_event.h"
#include "cal_tz.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CAL_URL_MAX   400
#define APP_CAL_NAME_MAX  24
/* Events kept for the week across all calendars. 128 x 88 bytes is the
 * 11 KB cache blob in NVS. */
#define APP_CAL_MAX_EVENTS 128

typedef struct {
    char    name[APP_CAL_NAME_MAX];
    uint8_t colour;                 /* app_gfx_colour_t */
    char    url[APP_CAL_URL_MAX];
} app_cal_source_t;

typedef struct {
    const cal_event_t *events;      /* owned by app_cal, valid until next fetch */
    size_t   count;
    bool     stale;                 /* at least one calendar failed: cached or missing */
    int64_t  stale_since_utc;       /* last good fetch; 0 when there is no cache */
    uint8_t  failed_mask;           /* bit i set = calendar i failed */
    uint32_t fetch_ms;
} app_cal_week_t;

/**
 * @brief Load the calendar list from NVS; on first boot, seed it from
 *        CONFIG_APP_CAL_SEED (set in sdkconfig.local).
 * @return ESP_OK, or an NVS error.
 */
esp_err_t app_cal_init(void);

/**
 * @brief Copy out the configured calendars.
 * @param[out] out  Up to CAL_MAX_CALENDARS entries.
 * @return How many are configured.
 */
size_t app_cal_sources(app_cal_source_t *out);

/**
 * @brief Replace calendar `idx`. An empty URL removes it and closes the gap.
 * @return ESP_ERR_INVALID_ARG for an index past the end, a URL that is not
 *         https:// or webcal://, a name that is empty or too long, or an unknown
 *         color; otherwise ESP_OK or an NVS error.
 */
esp_err_t app_cal_set_source(size_t idx, const char *name, uint8_t colour, const char *url);

/**
 * @brief Color name ("blue", "red", "green", "yellow", "black") to an app_gfx code.
 * @return The code, or -1 for an unknown name.
 */
int app_cal_colour_from_name(const char *name);

/**
 * @brief Color code to its name.
 * @return The name, or "?" for a code with none.
 */
const char *app_cal_colour_name(uint8_t colour);

/**
 * @brief Fetch every calendar over HTTPS and expand events overlapping [from, to).
 *        A calendar that fails is filled from the cache of the last good fetch, and the
 *        result is marked stale -- also when no cache exists and that calendar is simply
 *        missing. The cache is written only when every calendar succeeded and the list
 *        was not edited during the fetch. At most APP_CAL_MAX_EVENTS are returned, the
 *        earliest across all calendars.
 * @param local   The device's zone; event days and minutes are expressed in it.
 * @param online  false skips the network and draws every calendar from the cache.
 * @param[out] out  Valid until the next call.
 * @return ESP_OK when something usable came back, fresh or cached; ESP_ERR_NOT_FOUND when
 *         no calendar is configured; ESP_FAIL when every fetch failed and there is no
 *         cache.
 */
esp_err_t app_cal_fetch(const cal_tz_t *local, int64_t from_utc, int64_t to_utc, bool online,
                        app_cal_week_t *out);

/**
 * @brief The host of a calendar URL, for display without revealing the secret path.
 * @param[out] out  "calendar.google.com", or "" for an unparsable URL.
 */
void app_cal_url_host(const char *url, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
