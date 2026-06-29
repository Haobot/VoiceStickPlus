# Wi-Fi 子系统：定义 VOICE_NET_DISABLE（在 main/CMakeLists.txt 里 set）后，
# 组件不注册任何源码，esp_wifi/esp_http_client/mdns/mbedtls 等不被链接，
# 释放内部 RAM 与 2.4GHz 射频给 BLE 音频。voice_net.h 提供 stub 让 main.c 无感编译。
if(VOICE_NET_DISABLE)
    idf_component_register(
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "voice_net.c" "voice_net_nvs.c" "voice_net_discovery.c" "voice_net_ota.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_hw_support json lwip
        PRIV_REQUIRES espressif__mdns voice_ble app_update esp_http_client mbedtls esp-tls
    )
endif()
