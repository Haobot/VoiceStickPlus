idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES stick_s3_board voice_ble audio_pipeline ui_status button esp_pm esp_timer json bmi270
)
