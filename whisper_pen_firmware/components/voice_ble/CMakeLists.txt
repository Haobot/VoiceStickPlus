idf_component_register(
    SRCS "voice_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_hw_support esp_timer
)
