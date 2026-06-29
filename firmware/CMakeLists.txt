cmake_minimum_required(VERSION 3.16)

# Wi-Fi 禁用开关：置 ON 则 voice_net 组件退化为 stub，不链接 esp_wifi/esp_http_client/
# mdns/mbedtls，释放内部 RAM 与 2.4GHz 射频给 BLE 音频传输。
# 必须在 project() 之前设，保证所有组件 CMakeLists 都能读到该 cache 变量。
# 回退：注释掉下一行即恢复 Wi-Fi 功能。
set(VOICE_NET_DISABLE ON CACHE BOOL "Disable voice_net Wi-Fi subsystem" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_stick)
