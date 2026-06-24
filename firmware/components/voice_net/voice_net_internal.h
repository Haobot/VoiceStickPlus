#pragma once

// voice_net 内部模块共享头：只在 voice_net.c / voice_net_nvs.c / voice_net_discovery.c
// / voice_net_ota.c 之间使用，不对外。

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "voice_net.h"

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

// HTTPS OTA pull：见 voice_net_ota.c。
typedef enum {
    VOICE_NET_OTA_STATE_IDLE = 0,
    VOICE_NET_OTA_STATE_DOWNLOADING,
    VOICE_NET_OTA_STATE_FINISHING,
    VOICE_NET_OTA_STATE_SUCCESS,
    VOICE_NET_OTA_STATE_FAILED,
} voice_net_ota_state_t;

// 真正的 OTA 启动入口（voice_net.c 转发用，注入 Park gate 回调）。
void voice_net_start_ota_pull_internal(const char *url, const char *sha256_hex,
                                       voice_net_park_query_fn park_cb);
bool voice_net_ota_is_active(void);
voice_net_ota_state_t voice_net_ota_get_state(void);
int                   voice_net_ota_get_progress_pct(void);
const char           *voice_net_ota_get_url(void);
const char           *voice_net_ota_get_last_error(void);
const char           *voice_net_ota_state_string(voice_net_ota_state_t s);
