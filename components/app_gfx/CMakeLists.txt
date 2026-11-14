# No REQUIRES at all: app_gfx touches no ESP-IDF API, which is what lets test/host
# compile it with plain gcc.
idf_component_register(
    SRCS "app_gfx.c" "font_big.c" "font_head.c" "font_label.c" "font_body.c" "font_small.c"
    INCLUDE_DIRS "include")
