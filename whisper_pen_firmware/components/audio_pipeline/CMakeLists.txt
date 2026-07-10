idf_component_register(
    SRCS "audio_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES voice_ble esp_driver_i2s esp_timer
)
