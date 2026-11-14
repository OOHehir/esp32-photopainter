idf_component_register(
    SRCS "app_display.c"
    INCLUDE_DIRS "include"
    REQUIRES app_ui
    PRIV_REQUIRES app_epd app_gfx mbedtls nvs_flash esp_timer)
