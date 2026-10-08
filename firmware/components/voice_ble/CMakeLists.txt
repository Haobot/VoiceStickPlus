idf_component_register(
    SRCS
    "voice_ble.c"
    "conn_table.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bt nvs_flash json power_log
)
