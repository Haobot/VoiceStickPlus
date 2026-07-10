idf_component_register(
    SRCS "ptt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_hw_support esp_timer
)
