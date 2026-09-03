idf_component_register(
    SRCS "main.c"
         "pmu_bridge.cpp"
         "koyoda_idle.c"
         "koyoda_half.c"
         "koyoda_closed.c"
    INCLUDE_DIRS "."
    REQUIRES XPowersLib
)
