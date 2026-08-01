idf_component_register(
    SRCS "power_log.c"
    INCLUDE_DIRS "include"
    REQUIRES stick_s3_board esp_timer spiffs
)
