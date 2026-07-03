idf_component_register(
    SRCS "bmi270.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c stick_s3_board
    PRIV_REQUIRES esp_timer nvs_flash
)
