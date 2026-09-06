idf_component_register(
    SRCS "motion_probe.c"
    PRIV_REQUIRES
        waveshare__qmi8658
        waveshare__esp32_s3_touch_amoled_1_75
    WHOLE_ARCHIVE
)
