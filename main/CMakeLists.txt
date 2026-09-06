idf_component_register(
    SRCS "main.c"
         "pmu_bridge.cpp"
         "koyoda_idle.c"
         "koyoda_half.c"
         "koyoda_closed.c"
         "koyoda_charge_1.c"
         "koyoda_charge_2.c"
         "koyoda_charge_3.c"
         "koyoda_charge_4.c"
         "koyoda_charge_5.c"
         "koyoda_charge_6.c"
         "koyoda_sleep_1.c"
         "koyoda_sleep_2.c"
         "koyoda_sleep_3.c"
         "koyoda_motion_inline.c"
         "fun_excited.c"
         "fun_tilt.c"
         "fun_drop.c"
         "fun_peak_thrill.c"
         "fun_dizzy.c"
         "fun_happy.c"
    INCLUDE_DIRS "."
    REQUIRES
         XPowersLib
         waveshare__qmi8658
)
