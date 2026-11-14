#include "app_display.h"

#include <string.h>

#include "app_epd.h"
#include "app_gfx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "app_display";

#define NVS_NS   "app_display"
#define NVS_HASH "framehash"
#define HASH_LEN 32

static uint8_t *s_pixels;   /* 800 x 480, one color code per byte, PSRAM */
static uint8_t *s_packed;   /* APP_EPD_FRAME_BYTES, PSRAM */

static app_gfx_surface_t surface(void)
{
    return (app_gfx_surface_t){ .pixels = s_pixels, .y0 = 0, .rows = APP_GFX_HEIGHT };
}

esp_err_t app_display_init(void)
{
    if (!s_pixels) {
        s_pixels = heap_caps_malloc((size_t)APP_GFX_WIDTH * APP_GFX_HEIGHT, MALLOC_CAP_SPIRAM);
        s_packed = heap_caps_malloc(APP_EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_pixels || !s_packed) return ESP_ERR_NO_MEM;
    }
    return app_epd_init();
}

static bool hash_frame(uint8_t out[HASH_LEN])
{
    size_t olen = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, s_packed, APP_EPD_FRAME_BYTES, out, HASH_LEN,
                            &olen) == PSA_SUCCESS && olen == HASH_LEN;
}

static bool hash_load(uint8_t out[HASH_LEN])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = HASH_LEN;
    bool ok = nvs_get_blob(h, NVS_HASH, out, &len) == ESP_OK && len == HASH_LEN;
    nvs_close(h);
    return ok;
}

static void hash_store(const uint8_t *hash)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (hash) {
        nvs_set_blob(h, NVS_HASH, hash, HASH_LEN);
    } else {
        nvs_erase_key(h, NVS_HASH);
    }
    nvs_commit(h);
    nvs_close(h);
}

app_display_result_t app_display_show(const app_ui_model_t *model, bool force)
{
    if (!s_pixels) return APP_DISPLAY_FAILED;
    app_gfx_surface_t s = surface();

    app_ui_render(&s, model, false);
    app_gfx_pack(&s, s_packed, APP_EPD_FRAME_BYTES);
    uint8_t now[HASH_LEN], was[HASH_LEN];
    bool hashed = hash_frame(now);
    if (!force && hashed && hash_load(was) && memcmp(now, was, HASH_LEN) == 0) {
        ESP_LOGI(TAG, "frame unchanged -- no refresh");
        return APP_DISPLAY_UNCHANGED;
    }

    app_ui_render(&s, model, true);
    app_gfx_pack(&s, s_packed, APP_EPD_FRAME_BYTES);
    /* Forget the old hash before refreshing: a refresh cut short by a
     * brown-out leaves an unknown picture, and only a missing hash makes the next wake
     * redraw it. */
    hash_store(NULL);
    if (app_epd_show(s_packed, APP_EPD_FRAME_BYTES) != ESP_OK) {
        return APP_DISPLAY_FAILED;
    }
    if (hashed) hash_store(now);
    return APP_DISPLAY_REFRESHED;
}
