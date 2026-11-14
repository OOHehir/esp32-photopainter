#include "app_epd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_epd";

/* Command bytes Waveshare's PhotoPainter driver sends. Only the ones with
 * a known meaning are named; the rest are written as bare values in panel_init(). */
#define CMD_PSR   0x00   /* panel setting      */
#define CMD_PWR   0x01   /* power setting      */
#define CMD_POF   0x02   /* power off          */
#define CMD_PON   0x04   /* power on           */
#define CMD_BTST  0x06   /* booster soft start */
#define CMD_DSLP  0x07   /* deep sleep         */
#define CMD_DTM   0x10   /* start data phase   */
#define CMD_DRF   0x12   /* display refresh    */
#define CMD_TRES  0x61   /* resolution         */

/* ESP-IDF's SPI master caps one transaction at 4092 bytes unless
 * max_transfer_sz is raised, and the frame lives in PSRAM, which the SPI DMA cannot read
 * directly. Each chunk is copied through an internal bounce buffer of this size. */
#define SPI_CHUNK 4092

/* A refresh measured ~25 s per Waveshare's spec sheet. 60 s means a
 * controller that is not answering, not a slow one. */
#define BUSY_TIMEOUT_MS 60000

#if CONFIG_APP_EPD_ROTATE_180
#define ROTATE_180 true
#else
#define ROTATE_180 false
#endif

#if CONFIG_APP_EPD_ENABLE

static spi_device_handle_t s_spi;
static uint8_t *s_bounce;
static bool s_ready;
static uint32_t s_last_refresh_ms;

static esp_err_t spi_tx(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = len > SPI_CHUNK ? SPI_CHUNK : len;
        memcpy(s_bounce, data, n);
        spi_transaction_t t = { .length = n * 8, .tx_buffer = s_bounce };
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        data += n;
        len -= n;
    }
    return ESP_OK;
}

/* The same transfer with the frame read backwards and each byte's two
 * pixels swapped: a 180-degree rotation for free, with no second 192 000-byte buffer. */
static esp_err_t spi_tx_rotated(const uint8_t *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t n = (len - done) > SPI_CHUNK ? SPI_CHUNK : (len - done);
        for (size_t i = 0; i < n; i++) {
            uint8_t b = data[len - 1 - done - i];
            s_bounce[i] = (uint8_t)((b << 4) | (b >> 4));
        }
        spi_transaction_t t = { .length = n * 8, .tx_buffer = s_bounce };
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        done += n;
    }
    return ESP_OK;
}

/* CS is a plain GPIO, framed around each command and each data phase, as
 * Waveshare's driver does it. Letting the SPI peripheral toggle CS between bytes is the
 * arrangement no known-good driver for these controllers uses. */
static esp_err_t send_ex(uint8_t cmd, const uint8_t *data, size_t len, bool rotate)
{
    gpio_set_level(CONFIG_APP_EPD_DC_GPIO, 0);
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 0);
    esp_err_t err = spi_tx(&cmd, 1);
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 1);
    if (err != ESP_OK || len == 0) {
        return err;
    }
    gpio_set_level(CONFIG_APP_EPD_DC_GPIO, 1);
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 0);
    err = rotate ? spi_tx_rotated(data, len) : spi_tx(data, len);
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 1);
    return err;
}

static esp_err_t send(uint8_t cmd, const uint8_t *data, size_t len)
{
    return send_ex(cmd, data, len, false);
}

#define SEND(cmd, ...)                                                        \
    do {                                                                      \
        static const uint8_t d_[] = { __VA_ARGS__ };                          \
        esp_err_t e_ = send((cmd), d_, sizeof(d_));                           \
        if (e_ != ESP_OK) return e_;                                          \
    } while (0)

/* On this panel BUSY reads HIGH when the controller is idle -- the opposite
 * of the UC8159 in esp32-wx-station. */
static esp_err_t busy_wait(const char *what)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO) == 0) {
        if ((esp_timer_get_time() - t0) / 1000 > BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck low after %s (%d ms)", what, BUSY_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static void panel_reset(void)
{
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t panel_init(void)
{
    panel_reset();
    esp_err_t err = busy_wait("reset");
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    SEND(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);
    SEND(CMD_PWR, 0x3F);
    SEND(CMD_PSR, 0x5F, 0x69);
    SEND(0x03, 0x00, 0x54, 0x00, 0x44);
    SEND(0x05, 0x40, 0x1F, 0x1F, 0x2C);
    SEND(CMD_BTST, 0x6F, 0x1F, 0x17, 0x49);
    SEND(0x08, 0x6F, 0x1F, 0x1F, 0x22);
    SEND(0x30, 0x03);
    SEND(0x50, 0x3F);
    SEND(0x60, 0x02, 0x00);
    SEND(CMD_TRES, 0x03, 0x20, 0x01, 0xE0);   /* 800 x 480 */
    SEND(0x84, 0x01);
    SEND(0xE3, 0x2F);

    err = send(CMD_PON, NULL, 0);
    return err != ESP_OK ? err : busy_wait("power on");
}

static esp_err_t panel_refresh_and_sleep(void)
{
    esp_err_t err = send(CMD_PON, NULL, 0);
    if (err == ESP_OK) err = busy_wait("power on");
    if (err != ESP_OK) return err;

    SEND(CMD_BTST, 0x6F, 0x1F, 0x17, 0x49);

    int64_t t0 = esp_timer_get_time();
    SEND(CMD_DRF, 0x00);
    err = busy_wait("refresh");
    if (err != ESP_OK) return err;
    s_last_refresh_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    SEND(CMD_POF, 0x00);
    err = busy_wait("power off");
    if (err != ESP_OK) return err;

    /* 0xA5 is the deep-sleep check code; only a hardware reset wakes the
     * controller again, which panel_init() always does first. */
    SEND(CMD_DSLP, 0xA5);
    return ESP_OK;
}

esp_err_t app_epd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_bounce = heap_caps_malloc(SPI_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bounce) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << CONFIG_APP_EPD_RST_GPIO) | (1ULL << CONFIG_APP_EPD_DC_GPIO) |
                        (1ULL << CONFIG_APP_EPD_CS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(CONFIG_APP_EPD_CS_GPIO, 1);
    gpio_set_level(CONFIG_APP_EPD_RST_GPIO, 1);

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_APP_EPD_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_APP_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_APP_EPD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = CONFIG_APP_EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "SPI up at %d Hz, BUSY reads %d", CONFIG_APP_EPD_SPI_HZ,
             gpio_get_level(CONFIG_APP_EPD_BUSY_GPIO));
    return ESP_OK;
}

bool app_epd_present(void) { return true; }

esp_err_t app_epd_show(const uint8_t *packed, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (packed == NULL || len != APP_EPD_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = panel_init();
    if (err == ESP_OK) err = send_ex(CMD_DTM, packed, len, ROTATE_180);
    if (err == ESP_OK) err = panel_refresh_and_sleep();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "frame shown: refresh %u ms, total %lld ms", (unsigned)s_last_refresh_ms,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGE(TAG, "frame failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Built row by row in PSRAM rather than streamed, so fill and colour bar
 * go through exactly the path a rendered frame does. */
static esp_err_t show_generated(uint8_t (*colour_at)(int x))
{
    uint8_t *frame = heap_caps_malloc(APP_EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!frame) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t row[APP_EPD_WIDTH / 2];
    for (int x = 0; x < APP_EPD_WIDTH; x += 2) {
        row[x / 2] = (uint8_t)((colour_at(x) << 4) | colour_at(x + 1));
    }
    for (int y = 0; y < APP_EPD_HEIGHT; y++) {
        memcpy(frame + (size_t)y * sizeof(row), row, sizeof(row));
    }
    esp_err_t err = app_epd_show(frame, APP_EPD_FRAME_BYTES);
    heap_caps_free(frame);
    return err;
}

static uint8_t s_fill;
static uint8_t fill_at(int x) { (void)x; return s_fill; }

esp_err_t app_epd_fill(uint8_t colour)
{
    s_fill = colour & 0x07;
    return show_generated(fill_at);
}

static uint8_t bar_at(int x)
{
    static const uint8_t inks[] = { 0, 1, 2, 3, 5, 6 };
    return inks[x * 6 / APP_EPD_WIDTH];
}

esp_err_t app_epd_colour_bar(void) { return show_generated(bar_at); }

uint32_t app_epd_last_refresh_ms(void) { return s_last_refresh_ms; }

#else /* !CONFIG_APP_EPD_ENABLE */

esp_err_t app_epd_init(void) { return ESP_OK; }
bool app_epd_present(void) { return false; }
esp_err_t app_epd_show(const uint8_t *packed, size_t len) { (void)packed; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_fill(uint8_t colour) { (void)colour; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_epd_colour_bar(void) { return ESP_ERR_NOT_SUPPORTED; }
uint32_t app_epd_last_refresh_ms(void) { return 0; }

#endif
