/* app_display -- owns the panel: render, decide whether the picture
 * changed, refresh.
 *
 * Synchronous: the wake cycle calls it once and then sleeps, so there is no task to hand
 * requests to. A refresh blocks the caller for about 28 seconds.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ui.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_DISPLAY_UNCHANGED,   /* same picture as the glass; not refreshed */
    APP_DISPLAY_REFRESHED,
    APP_DISPLAY_FAILED,
} app_display_result_t;

/**
 * @brief Bring up the panel and allocate the frame in PSRAM.
 * @return ESP_OK, ESP_ERR_NO_MEM, or the panel driver's error.
 */
esp_err_t app_display_init(void);

/**
 * @brief Render `model` and refresh the panel if the picture differs from the one on the
 *        glass. Climate and battery readings do not count as a difference, or every wake
 *        would refresh for a tenth of a degree.
 * @param force  Refresh even when unchanged; the KEY button uses it.
 * @return What happened. On APP_DISPLAY_FAILED the stored hash is cleared, so the next
 *         wake refreshes whatever it renders.
 */
app_display_result_t app_display_show(const app_ui_model_t *model, bool force);

#ifdef __cplusplus
}
#endif
