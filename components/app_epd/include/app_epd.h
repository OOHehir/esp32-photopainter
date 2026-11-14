/* app_epd -- the PhotoPainter's 7.3" Spectra 6 panel: SPI, reset, init,
 * frame upload, refresh.
 *
 * The register sequence is the one Waveshare's own driver for this board sends
 * (ESP32-S3-PhotoPainter, port_bsp/display_bsp.cpp); the controller is not named in
 * Waveshare's documentation, so there is no datasheet to cross-check it against.
 *
 * The board has PSRAM, so a whole 192 000-byte packed frame is uploaded in one call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_EPD_WIDTH        800
#define APP_EPD_HEIGHT       480
#define APP_EPD_FRAME_BYTES  ((size_t)APP_EPD_WIDTH * APP_EPD_HEIGHT / 2)

/**
 * @brief Bring up the SPI bus and the control pins. Does not touch the panel.
 * @return ESP_OK, or the SPI/GPIO driver's error.
 */
esp_err_t app_epd_init(void);

/**
 * @brief True when this build drives a panel at all (CONFIG_APP_EPD_ENABLE).
 * @return Whether a panel is compiled in.
 */
bool app_epd_present(void);

/**
 * @brief Reset and initialize the controller, upload one frame, refresh, then put the
 *        controller into deep sleep. Blocks for the whole refresh, about 25 seconds.
 * @param packed  Two pixels per byte, left pixel in the high nibble, rows top to bottom.
 * @param len     Must be APP_EPD_FRAME_BYTES.
 * @return ESP_OK; ESP_ERR_INVALID_SIZE for a wrong length; ESP_ERR_TIMEOUT when BUSY
 *         never released, which leaves the glass showing an unknown picture.
 */
esp_err_t app_epd_show(const uint8_t *packed, size_t len);

/**
 * @brief Fill the panel with one color code and refresh.
 * @param colour  An app_gfx_colour_t value.
 * @return As app_epd_show().
 */
esp_err_t app_epd_fill(uint8_t colour);

/**
 * @brief Six vertical bars, one per ink, straight through the driver. Bring-up only:
 *        proves each color code reaches the pigment app_gfx expects.
 * @return As app_epd_show().
 */
esp_err_t app_epd_colour_bar(void);

/**
 * @brief Duration of the last successful refresh, for the status page.
 * @return Milliseconds, or 0 before the first refresh.
 */
uint32_t app_epd_last_refresh_ms(void);

#ifdef __cplusplus
}
#endif
