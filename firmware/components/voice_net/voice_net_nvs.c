// Wi-Fi STA 凭据持久化，命名空间 "voicestick"。
// 键名严格 ≤15 字符（NVS 限制）：sta_ssid / sta_pass / sta_en。
// 与 voice_ble 内的 NVS 命名空间互不干扰；voice_ble_init 已经做过 nvs_flash_init。

#include "voice_net_internal.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "voice_net_nvs";
static const char *NS = "voicestick";

esp_err_t voice_net_nvs_load(char *ssid, size_t ssid_size,
                             char *password, size_t password_size,
                             bool *out_enabled)
{
    if (!ssid || !password || !out_enabled || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid[0] = '\0';
    password[0] = '\0';
    *out_enabled = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 命名空间从未写入过，按"未配置"处理而不是错误。
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = ssid_size;
    err = nvs_get_str(handle, "sta_ssid", ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read sta_ssid failed: %s", esp_err_to_name(err));
    }

    len = password_size;
    err = nvs_get_str(handle, "sta_pass", password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read sta_pass failed: %s", esp_err_to_name(err));
    }

    uint8_t enabled = 0;
    err = nvs_get_u8(handle, "sta_en", &enabled);
    if (err == ESP_OK) {
        *out_enabled = enabled != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read sta_en failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t voice_net_nvs_save(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, "sta_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "sta_pass", password);
    if (err == ESP_OK) err = nvs_set_u8(handle, "sta_en", 1);
    if (err == ESP_OK) err = nvs_commit(handle);

    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved sta_ssid=%s", ssid);  // 密码日志侧脱敏：永远不打印 sta_pass
    }
    return err;
}

esp_err_t voice_net_nvs_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed: %s", esp_err_to_name(err));
        return err;
    }

    // 三个键独立删除，遇到 NOT_FOUND 不算错——可能是只写过 ssid 没 commit 完成的情况。
    esp_err_t e1 = nvs_erase_key(handle, "sta_ssid");
    esp_err_t e2 = nvs_erase_key(handle, "sta_pass");
    esp_err_t e3 = nvs_erase_key(handle, "sta_en");
    err = nvs_commit(handle);
    nvs_close(handle);

    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) return e1;
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) return e2;
    if (e3 != ESP_OK && e3 != ESP_ERR_NVS_NOT_FOUND) return e3;
    return err;
}
