idf_component_register(
    SRCS "app_cycle.c" "cycle_sched.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES app_board app_cal app_cfg app_core app_diag app_display app_epd app_httpd
                  app_net app_ota app_ui esp_driver_gpio esp_timer esp_app_format)
