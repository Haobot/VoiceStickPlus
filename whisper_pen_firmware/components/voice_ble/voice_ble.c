// voice_ble.c —— NimBLE 协议栈 + GATT 服务 + Opus 帧发送
//
// 修正点落地（对照评估结论）：
//   - 低延迟头号参数是 connection interval，不是 PHY（评估第4点）：
//     连接建立即请求 itvl=6(7.5ms) 固定 + min_ce_len=8 多塞通知。
//   - 主动 MTU exchange：WinRT 等中央不主动换，默认 23 字节会丢大通知。
//   - 2M PHY：吞吐翻倍，mask 含 1M 自动回退，零兼容风险。
//   - UUID 用方案要求的 16-bit（0xFF10/0xFF11/0xFF12）。
//
// NimBLE 启动全套（实证参考 Voice Stick voice_ble.c）：
//   nimble_port_init -> ble_svc_gap/gatt_init -> ble_hs_cfg(sync/reset cb)
//   -> ble_svc_gap_device_name_set -> ble_gatts_count_cfg + add_svcs
//   -> nimble_port_freertos_init(host_task)；on_sync 里 infer_addr + start_advertising。

#include "voice_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "voice_ble";

// 连接参数（评估第4点：interval 是低延迟头号参数）
#define CONN_ITVL_FAST_MIN  6    // 7.5ms，固定不留给 central 上浮空间
#define CONN_ITVL_FAST_MAX  6
#define CONN_ITVL_SLOW_MIN  80   // 100ms（待机省电）
#define CONN_ITVL_SLOW_MAX  320
#define CONN_LATENCY        0
#define CONN_TIMEOUT        200  // 2s

static bool s_connected;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint16_t s_audio_attr_handle;
static char s_device_name[16];

// ─── GATT access 回调 ──────────────────────────────────────
static int audio_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // audio_tx 是 notify-only，主机读返回空即可。
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // 脚手架：control（ui_state 等）此处仅日志，后续扩展为状态机回调。
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char buf[128] = {0};
        if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
        uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &out);
        ESP_LOGI(TAG, "control rx(%u): %.*s", (unsigned)out, (unsigned)out, buf);
    }
    return 0;
}

// GATT 服务表：方案要求的 0xFF10 service + 0xFF11 audio(notify) + 0xFF12 control(write)
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(VOICE_BLE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(VOICE_BLE_CHR_AUDIO_TX),
                .access_cb = audio_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_audio_attr_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(VOICE_BLE_CHR_CONTROL),
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

// ─── advertising ──────────────────────────────────────────
// 前向声明：start_advertising 在 adv_start 回调里引用 gap 事件处理，后者定义在后面。
static int gap_event_cb_wrapper(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
    if (s_connected || ble_gap_adv_active()) return;

    static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(VOICE_BLE_SVC_UUID);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)s_device_name;
    rsp.name_len = strlen(s_device_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "adv rsp rc=%d", rc); return; }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(20);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(30);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event_cb_wrapper, NULL);
    if (rc != 0) { ESP_LOGW(TAG, "adv start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as %s", s_device_name);
}

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer addr rc=%d", rc); return; }
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "ble_hs reset reason=%d", reason);
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ─── GAP event ─────────────────────────────────────────────
static int gap_event_cb_wrapper(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected handle=%u", s_conn_handle);

            // 主动 MTU exchange（评估第4点）：WinRT 等中央不主动换，默认 23 字节丢大通知。
            int rc = ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
            if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "mtu exch rc=%d", rc);

            // 2M PHY（评估第4点）：吞吐翻倍，mask 含 1M 自动回退。
            rc = ble_gap_set_prefered_le_phy(s_conn_handle,
                BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK,
                BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK, 0);
            if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "phy rc=%d", rc);

            // 快 interval（低延迟头号参数）：7.5ms 固定。
            voice_ble_request_fast_interval();
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "phy tx=%u rx=%u",
                 event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn updated interval=%u latency=%u timeout=%u",
                     desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        }
        return 0;
    }

    default:
        return 0;
    }
}

// ─── 公共接口 ──────────────────────────────────────────────
esp_err_t voice_ble_init(void) {
    // 设备名：WP-XXXX（MAC 末两字节）
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02X%02X",
             VOICE_BLE_DEVICE_NAME_PREFIX, mac[4], mac[5]);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // 脚手架不配对绑定（评估方案未要求加密）；如需配对再加 sm_* 字段。

    int rc = ble_svc_gap_device_name_set(s_device_name);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "set name rc=%d", rc);

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "count gatt rc=%d", rc);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "add gatt rc=%d", rc);

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE initialized as %s", s_device_name);
    return ESP_OK;
}

bool voice_ble_is_connected(void) {
    return s_connected;
}

esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                                const uint8_t *opus_payload, size_t len) {
    (void)session_id;
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    // 包结构 [SeqNum u16][Timestamp u32][Flags u8][Opus payload]（方案要求）
    // 用 os_mbuf 拼接，避免与 OPUS_MAX_PACKET_SIZE 耦合。
    uint8_t hdr[7];
    hdr[0] = seq & 0xFF;
    hdr[1] = (seq >> 8) & 0xFF;
    uint32_t ts = seq * VOICE_BLE_AUDIO_FRAME_MS;   // ms
    hdr[2] = ts & 0xFF;
    hdr[3] = (ts >> 8) & 0xFF;
    hdr[4] = (ts >> 16) & 0xFF;
    hdr[5] = (ts >> 24) & 0xFF;
    hdr[6] = flags;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, sizeof(hdr));
    if (om == NULL) return ESP_ERR_NO_MEM;
    if (len > 0) {
        int rc = os_mbuf_append(om, opus_payload, len);
        if (rc != 0) { os_mbuf_free_chain(om); return ESP_FAIL; }
    }

    int rc = ble_gattc_notify_custom(s_conn_handle, s_audio_attr_handle, om);
    if (rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t voice_ble_request_fast_interval(void) {
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    struct ble_gap_upd_params upd = {
        .itvl_min = CONN_ITVL_FAST_MIN,
        .itvl_max = CONN_ITVL_FAST_MAX,
        .latency = CONN_LATENCY,
        .supervision_timeout = CONN_TIMEOUT,
        .min_ce_len = 8,   // 每 connection event 至少 5ms，多塞音频通知
        .max_ce_len = 8,
    };
    return ble_gap_update_params(s_conn_handle, &upd) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_ble_request_slow_interval(void) {
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    struct ble_gap_upd_params upd = {
        .itvl_min = CONN_ITVL_SLOW_MIN,
        .itvl_max = CONN_ITVL_SLOW_MAX,
        .latency = CONN_LATENCY,
        .supervision_timeout = CONN_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    return ble_gap_update_params(s_conn_handle, &upd) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_ble_send_button_event(const char *button, bool pressed, uint32_t session_id) {
    // 脚手架：state 通道待扩展。方案核心任务是音频传输，按键事件此处仅日志。
    ESP_LOGI(TAG, "button %s %s session=%lu",
             button, pressed ? "down" : "up", (unsigned long)session_id);
    return ESP_OK;
}
