idf_component_register(
    SRCS "voice_net.c" "voice_net_nvs.c" "voice_net_discovery.c" "voice_net_ota.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_hw_support json lwip
    PRIV_REQUIRES espressif__mdns voice_ble app_update esp_http_client esp_https_ota mbedtls esp-tls
)
