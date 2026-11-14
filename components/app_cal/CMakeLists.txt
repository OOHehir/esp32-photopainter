# cal_*.c are pure C and also compiled by test/host; only app_cal.c touches
# ESP-IDF.
idf_component_register(
    SRCS "app_cal.c" "cal_parse.c" "cal_rrule.c" "cal_tz.c" "cal_value.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES app_trust esp_http_client esp_timer nvs_flash)
