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
         "fun_happy.c"
         "koyoda_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
         XPowersLib
         esp_wifi
         esp_event
         esp_netif
         nvs_flash
)
