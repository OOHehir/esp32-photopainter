#include "app_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_board";

#define I2C_SDA_GPIO 47
#define I2C_SCL_GPIO 48
#define I2C_HZ       100000
#define I2C_TIMEOUT_MS 50

#define PMIC_ADDR   0x34
#define SHTC3_ADDR  0x70

/* AXP2101 registers, numbering from XPowersLib's AXP2101Constants.h. */
#define REG_STATUS1      0x00   /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2      0x01   /* bits6:5 01 = charging */
#define REG_INPUT_LIMIT  0x16   /* bits2:0, 5 = 2000 mA */
#define REG_ADC_CTRL     0x30   /* bit0 battery voltage channel */
#define REG_VBAT_H       0x34   /* 13-bit mV, high 5 bits here */
#define REG_VBAT_L       0x35
#define REG_ICC          0x62   /* bits4:0, 11 = 500 mA */
#define REG_BAT_DET      0x68   /* bit0 battery detection */
#define REG_DC1_VOL      0x82   /* (mV - 1500) / 100 */
#define REG_ALDO1_VOL    0x92   /* ALDO1..4 at 0x92..0x95, (mV - 500) / 100 */
#define REG_BAT_PCT      0xA4

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmic;
static i2c_master_dev_handle_t s_shtc3;
static bool s_pmic_ok;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pmic, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pmic, buf, 2, I2C_TIMEOUT_MS);
}

/* Read-modify-write that skips the write when the field already holds the
 * value, so a normal boot changes nothing in the PMIC. */
static esp_err_t reg_field(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur;
    esp_err_t err = reg_read(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t next = (uint8_t)((cur & ~mask) | (val & mask));
    if (next == cur) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "pmic reg 0x%02x: 0x%02x -> 0x%02x", reg, cur, next);
    return reg_write(reg, next);
}

static esp_err_t pmic_configure(void)
{
    esp_err_t err = ESP_OK;
    /* Rails at 3.3 V as Waveshare's firmware sets them. Which rail feeds
     * which part is not documented; all of them at 3.3 V is the known-good state. */
    if (err == ESP_OK) err = reg_field(REG_DC1_VOL, 0x1F, (3300 - 1500) / 100);
    for (uint8_t r = REG_ALDO1_VOL; err == ESP_OK && r <= REG_ALDO1_VOL + 3; r++) {
        err = reg_field(r, 0x1F, (3300 - 500) / 100);
    }
    if (err == ESP_OK) err = reg_field(REG_INPUT_LIMIT, 0x07, 5);
    if (err == ESP_OK) err = reg_field(REG_ICC, 0x1F, 11);
    if (err == ESP_OK) err = reg_field(REG_BAT_DET, 0x01, 0x01);
    if (err == ESP_OK) err = reg_field(REG_ADC_CTRL, 0x01, 0x01);
    return err;
}

esp_err_t app_board_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = I2C_HZ,
    };
    dev.device_address = PMIC_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_pmic));
    dev.device_address = SHTC3_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_shtc3));

    err = pmic_configure();
    s_pmic_ok = (err == ESP_OK);
    if (!s_pmic_ok) {
        /* Not fatal: the rails come up at their OTP defaults without
         * software, so the board may still run; only the battery readout is lost. */
        ESP_LOGE(TAG, "PMIC at 0x%02x not answering: %s", PMIC_ADDR, esp_err_to_name(err));
    }

    gpio_config_t key = {
        .pin_bit_mask = 1ULL << APP_BOARD_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key));
    return ESP_OK;
}

void app_board_power(app_board_power_t *out)
{
    memset(out, 0, sizeof(*out));
    out->battery_pct = -1;
    uint8_t s1, s2, vh, vl, pct;
    if (!s_pmic || reg_read(REG_STATUS1, &s1) != ESP_OK || reg_read(REG_STATUS2, &s2) != ESP_OK) {
        return;
    }
    out->pmic_ok = true;
    out->vbus = (s1 & 0x20) != 0;
    out->battery = (s1 & 0x08) != 0;
    out->charging = ((s2 >> 5) & 0x03) == 0x01;
    if (out->battery && reg_read(REG_VBAT_H, &vh) == ESP_OK && reg_read(REG_VBAT_L, &vl) == ESP_OK) {
        out->battery_mv = (uint16_t)(((vh & 0x1F) << 8) | vl);
    }
    if (out->battery && reg_read(REG_BAT_PCT, &pct) == ESP_OK && pct <= 100) {
        out->battery_pct = (int8_t)pct;
    }
}

/* CRC-8, polynomial 0x31, init 0xFF -- SHTC3 datasheet section 5.10. */
static uint8_t shtc3_crc(const uint8_t *d)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t app_board_climate(float *temp_c, float *rh_pct)
{
    if (!s_shtc3) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t wake[] = { 0x35, 0x17 };
    /* Normal mode, T first, clock stretching. The no-stretch command
     * (0x7866) never returned data on the bench board within 15 ms; this one did. */
    static const uint8_t measure[] = { 0x7C, 0xA2 };
    static const uint8_t sleep[] = { 0xB0, 0x98 };
    esp_err_t err = i2c_master_transmit(s_shtc3, wake, 2, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 wake: %s", esp_err_to_name(err));
        return err;
    }
    /* The datasheet's wake time is 240 us, but the measure command is NACKed
     * on this board until well after that; Waveshare's driver waits 50 ms. */
    vTaskDelay(pdMS_TO_TICKS(50));
    err = i2c_master_transmit(s_shtc3, measure, 2, I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t d[6];
        err = i2c_master_receive(s_shtc3, d, sizeof(d), 100);
        if (err == ESP_OK && (shtc3_crc(d) != d[2] || shtc3_crc(d + 3) != d[5])) {
            err = ESP_ERR_INVALID_CRC;
        }
        if (err == ESP_OK) {
            *temp_c = -45.0f + 175.0f * (float)((d[0] << 8) | d[1]) / 65536.0f;
            *rh_pct = 100.0f * (float)((d[3] << 8) | d[4]) / 65536.0f;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 measure: %s", esp_err_to_name(err));
    }
    i2c_master_transmit(s_shtc3, sleep, 2, I2C_TIMEOUT_MS);
    return err;
}

bool app_board_key_held(void)
{
    return gpio_get_level(APP_BOARD_KEY_GPIO) == 0;
}
