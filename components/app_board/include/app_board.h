/* app_board -- the PhotoPainter's own parts that are not the panel: the
 * I2C bus, the AXP2101-compatible PMIC (Waveshare label it TG28), the SHTC3
 * temperature/humidity sensor and the KEY button.
 *
 * The PMIC is driven by bare register writes rather than the XPowersLib Waveshare ship:
 * the firmware needs five registers, and XPowersLib is C++ and several thousand lines.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     pmic_ok;        /* false when the PMIC did not answer on I2C */
    bool     battery;        /* a cell is connected */
    uint16_t battery_mv;     /* 0 without a cell */
    int8_t   battery_pct;    /* PMIC fuel gauge, -1 without a cell */
    bool     vbus;           /* USB power present */
    bool     charging;
} app_board_power_t;

/**
 * @brief Start the I2C bus and configure the PMIC: rail voltages as Waveshare's firmware
 *        sets them, 2 A input limit, 500 mA charge current, battery ADC on.
 * @return ESP_OK even when the PMIC does not answer (logged, and reported through
 *         app_board_power()); an error only when the I2C bus itself cannot start.
 */
esp_err_t app_board_init(void);

/**
 * @brief Read the battery and charger state.
 * @param[out] out  Filled in; pmic_ok false and the rest zeroed on an I2C failure.
 */
void app_board_power(app_board_power_t *out);

/**
 * @brief One SHTC3 measurement. Takes about 70 ms.
 * @param[out] temp_c  Degrees Celsius.
 * @param[out] rh_pct  Relative humidity, percent.
 * @return ESP_OK, or ESP_ERR_INVALID_CRC / an I2C error with outputs untouched.
 */
esp_err_t app_board_climate(float *temp_c, float *rh_pct);

/**
 * @brief KEY button level now. The button pulls GPIO4 low.
 * @return true while held.
 */
bool app_board_key_held(void);

/* KEY on the board's side edge; also the deep-sleep wake source. */
#define APP_BOARD_KEY_GPIO 4

#ifdef __cplusplus
}
#endif
