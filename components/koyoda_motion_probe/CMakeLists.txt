idf_component_register(
    SRCS
        "motion_probe.c"
        "fun_excited.c"
        "fun_tilt.c"
        "fun_drop.c"
        "fun_peak_thrill.c"
        "fun_dizzy.c"
        "fun_happy.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        waveshare__qmi8658
        waveshare__esp32_s3_touch_amoled_1_75
        lvgl__lvgl
    WHOLE_ARCHIVE
)
