idf_component_register(
    SRCS "mini_encoder_c.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c stick_s3_board
)
