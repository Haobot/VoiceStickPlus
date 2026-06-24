#pragma once

// voice_net 内部模块共享头：只在 voice_net.c / voice_net_nvs.c 之间使用，不对外。

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// NVS 凭据读写：见 voice_net_nvs.c。
esp_err_t voice_net_nvs_load(char *ssid, size_t ssid_size,
                             char *password, size_t password_size,
                             bool *out_enabled);
esp_err_t voice_net_nvs_save(const char *ssid, const char *password);
esp_err_t voice_net_nvs_clear(void);

// mDNS 与 SNTP：见 voice_net_discovery.c。第一次 GOT_IP 时由 voice_net.c 调用。
// 内部 idempotent，重复调用直接返回 ESP_OK；不主动 stop（Wi-Fi 重连后自动恢复）。
esp_err_t voice_net_discovery_start_mdns(void);
esp_err_t voice_net_discovery_start_sntp(void);
