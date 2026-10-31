// gateway_mode.c — 模式状态（恒定网关，NVS 覆写保证跨版本一致）
#include "gateway_mode.h"

#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS "gateway"
#define NVS_KEY_MODE "mode"

static gateway_mode_t s_mode = GATEWAY_MODE_GATEWAY;

esp_err_t gateway_mode_init(void) {
    // 模式初始化在 voice_ble_init 之前（main.c NimBLE 注册窗口约束），NVS 需在此自举；
    // nvs_flash_init 对已初始化分区重复调用返回 ESP_OK，voice_ble 的兜底初始化不受影响。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_mode = GATEWAY_MODE_GATEWAY;
    // 把持久值覆写为网关：清掉历史上可能写入的普通模式标志，旧版本固件回滚时
    // 读到 1 仍是网关，不会再次出现「OTA 后配对功能消失」的状态回退。
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_OK;  // 命名空间不存在等写入失败不阻塞启动（内存态已是网关）
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)s_mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return ESP_OK;
}

gateway_mode_t gateway_mode_get(void) { return s_mode; }

const char *gateway_mode_name(gateway_mode_t mode) {
    return mode == GATEWAY_MODE_GATEWAY ? "网关模式" : "普通模式";
}
