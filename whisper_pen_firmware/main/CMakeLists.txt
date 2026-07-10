idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES audio_pipeline voice_ble ptt stick_s3_board ui_status esp_timer esp_hw_support json
)
