idf_component_register(
    SRCS "voice_net.c" "voice_net_nvs.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event json
)
