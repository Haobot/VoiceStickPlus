idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES audio_pipeline voice_ble ptt esp_timer esp_hw_support
)
