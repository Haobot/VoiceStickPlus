// mDNS 设备发现 + SNTP 时间同步。
//
// 设计：
// - 都是 lazy-init：第一次拿到 IP 时启动一次，之后即使 Wi-Fi 断了也不主动关闭
//   （mdns/sntp 都是 idempotent，重连后会自动恢复）。
// - mDNS hostname 规则：voicestick-<mac4>，与 BLE 设备名 VS-XXXX 中的 mac4 对齐。
//   局域网用户 ping voicestick-d010.local 就能找到设备。
// - mDNS 服务：注册 _voicestick._tcp 占位服务，端口 80（即使本期不开 HTTP 也占着，
//   方便用 dns-sd / Bonjour Browser / `avahi-browse -art` 一眼看到设备）。
// - SNTP 走阿里云 NTP 池（国内访问稳定），失败也无所谓——voicestick 主链路不依赖
//   绝对时间，时间戳只用于调试音频文件名。

#include "voice_net_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "voice_ble.h"

static const char *TAG = "voice_net_disc";

static bool s_mdns_inited = false;
static bool s_sntp_inited = false;

static void make_hostname(char *out, size_t cap)
{
    // 与 voice_ble 设备名同源：BLE 用 base MAC 的 mac[4]mac[5] 派生 VS-XXXX，
    // 我们直接复用 voice_ble_device_id()（如 "D010"）保证设备在局域网和 BLE
    // 上的标识完全一致——避免出现 BLE 名 VS-D010 但 mDNS voicestick-d012 的情况。
    const char *id = voice_ble_device_id();
    char id_lower[8] = {0};
    for (int i = 0; i < (int)sizeof(id_lower) - 1 && id[i]; ++i) {
        id_lower[i] = (char)tolower((unsigned char)id[i]);
    }
    snprintf(out, cap, "voicestick-%s", id_lower);
}

esp_err_t voice_net_discovery_start_mdns(void)
{
    if (s_mdns_inited) return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    char hostname[32];
    make_hostname(hostname, sizeof(hostname));
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    char instance[64];
    snprintf(instance, sizeof(instance), "VoiceStick %s", voice_ble_device_id());
    mdns_instance_name_set(instance);

    // 注册占位服务，让 dns-sd / Bonjour Browser 能看到设备。
    // 端口 80 是惯例占位；本期不开 HTTP 服务器，未来 §8 HTTPS OTA 也是 pull 模式
    // 不在设备端开 server——这条记录仅做发现用。
    mdns_service_add(NULL, "_voicestick", "_tcp", 80, NULL, 0);

    s_mdns_inited = true;
    ESP_LOGI(TAG, "mdns up: %s.local instance=\"%s\"", hostname, instance);
    return ESP_OK;
}

esp_err_t voice_net_discovery_start_sntp(void)
{
    if (s_sntp_inited) return ESP_OK;

    // 阿里云 NTP 池：国内访问稳定，不需要 VPN。
    // 受 CONFIG_LWIP_SNTP_MAX_SERVERS=1 限制只能用单个服务器；要 fallback 需要先
    // 提栈 sdkconfig 改成 ≥2，本期不动。
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.start = true;
    cfg.sync_cb = NULL;              // 不订阅 sync 完成回调；lwip 内部自动 settimeofday
    cfg.wait_for_sync = false;       // 不阻塞 worker task：sync 是后台行为，时间晚一会也无所谓

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sntp_inited = true;
    ESP_LOGI(TAG, "sntp started: ntp.aliyun.com");
    return ESP_OK;
}
