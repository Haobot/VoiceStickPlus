// gateway_mode.c — 模式状态 NVS 持久化
#include "gateway_mode.h"

#include "nvs.h"

#define NVS_NS "gateway"
#define NVS_KEY_MODE "mode"

static gateway_mode_t s_mode = GATEWAY_MODE_NORMAL;

esp_err_t gateway_mode_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;  // 无记录按默认普通模式
    }
    uint8_t value = GATEWAY_MODE_NORMAL;
    (void)nvs_get_u8(h, NVS_KEY_MODE, &value);
    nvs_close(h);
    s_mode = (value == GATEWAY_MODE_GATEWAY) ? GATEWAY_MODE_GATEWAY : GATEWAY_MODE_NORMAL;
    return ESP_OK;
}

gateway_mode_t gateway_mode_get(void) { return s_mode; }

esp_err_t gateway_mode_toggle(void) {
    s_mode = (s_mode == GATEWAY_MODE_GATEWAY) ? GATEWAY_MODE_NORMAL : GATEWAY_MODE_GATEWAY;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)s_mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

const char *gateway_mode_name(gateway_mode_t mode) {
    return mode == GATEWAY_MODE_GATEWAY ? "网关模式" : "普通模式";
}
