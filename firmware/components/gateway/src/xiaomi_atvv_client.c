// xiaomi_atvv_client.c — ATVV 语音链路 NimBLE 薄壳实现
#include "xiaomi_atvv_client.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gateway_atvv_session.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"

static const char *TAG = "gw_atvv";

// ATVV 服务与特征 UUID（Google ATVV profile；字节序经 Phase 0 spike 真机验证）
static const ble_uuid128_t UUID_ATVV_SVC = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x01, 0x00, 0x5E, 0xAB);  // AB5E0001
static const ble_uuid128_t UUID_ATVV_TX = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x02, 0x00, 0x5E, 0xAB);  // AB5E0002 write
static const ble_uuid128_t UUID_ATVV_AUDIO = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x03, 0x00, 0x5E, 0xAB);  // AB5E0003 notify
static const ble_uuid128_t UUID_ATVV_CTRL = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x04, 0x00, 0x5E, 0xAB);  // AB5E0004 notify

#define CHR_PROP_WRITE 0x08
#define CHR_PROP_NOTIFY 0x10

// 发现启动延迟：HID 订阅的 CCCD 写还在飞（无回调可等），稍后再发起 ATVV 发现，
// EBUSY 时由重试 callout 兜底
#define DISC_DELAY_TICKS pdMS_TO_TICKS(400)
#define DISC_RETRY_TICKS pdMS_TO_TICKS(500)
// tick 周期：驱动 caps 超时与尾包宽限（150ms 宽限 → 250ms 周期最坏晚一拍，可接受）
#define TICK_PERIOD_TICKS pdMS_TO_TICKS(250)

static xiaomi_atvv_press_cb_t s_on_press;
static xiaomi_atvv_pcm_cb_t s_on_pcm;
static bool s_running;
static bool s_link_up;  // 连接在且发现流程完成（caps 握手已发起）
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint16_t s_audio_handle;
static uint16_t s_ctrl_handle;
static gateway_atvv_session_t s_session;
static struct ble_npl_callout s_disc_callout;
static bool s_disc_callout_inited;
static struct ble_npl_callout s_tick_callout;
static bool s_tick_callout_inited;

// ---- 会话动作消费 ----

static void consume_actions(const gateway_atvv_action_t *actions, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const gateway_atvv_action_t *a = &actions[i];
        switch (a->kind) {
            case GATEWAY_ATVV_ACTION_WRITE_TX:
                if (s_tx_handle != 0) {
                    (void)ble_gattc_write_flat(s_conn, s_tx_handle, a->tx, a->tx_len,
                                               NULL, NULL);
                }
                break;
            case GATEWAY_ATVV_ACTION_PRESS_DOWN:
                if (s_on_press != NULL) {
                    s_on_press(true);
                }
                break;
            case GATEWAY_ATVV_ACTION_PRESS_UP:
                if (s_on_press != NULL) {
                    s_on_press(false);
                }
                break;
            case GATEWAY_ATVV_ACTION_PCM_FRAME:
                if (s_on_pcm != NULL) {
                    s_on_pcm(a->pcm, a->pcm_len);
                }
                break;
            case GATEWAY_ATVV_ACTION_ERROR:
                ESP_LOGW(TAG, "ATVV 会话错误：%s", a->error_code);
                break;
            default:
                break;
        }
    }
}

// ---- 时钟：esp_timer 微秒转毫秒（会话时序常数均为毫秒级） ----

static int64_t now_ms(void) {
    return (int64_t)(esp_timer_get_time() / 1000);
}

// ---- notify 路由（经 gateway_hid_host_set_notify_router 注册） ----

void xiaomi_atvv_client_notify_router(uint16_t attr_handle, const uint8_t *data,
                                      size_t len) {
    if (!s_link_up || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    gateway_atvv_action_t actions[GATEWAY_ATVV_MAX_ACTIONS];
    if (attr_handle == s_ctrl_handle && s_ctrl_handle != 0) {
        size_t n = gateway_atvv_session_control(&s_session, data, len, now_ms(), actions,
                                                GATEWAY_ATVV_MAX_ACTIONS);
        consume_actions(actions, n);
    } else if (attr_handle == s_audio_handle && s_audio_handle != 0) {
        size_t n = gateway_atvv_session_audio(&s_session, data, len, now_ms(), actions,
                                              GATEWAY_ATVV_MAX_ACTIONS);
        consume_actions(actions, n);
    }
}

// ---- GATT 发现链 ----

struct atvv_range {
    uint16_t start;
    uint16_t end;
};

static int on_atvv_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);

// CCCD 枚举回调：找到 0x2902 写使能 notify（与 hid_host 同模式）
static int on_cccd_dsc(uint16_t conn, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)chr_val_handle;
    (void)arg;
    if (error->status != 0) {
        return 0;
    }
    if (dsc != NULL && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        static const uint16_t enable_notify = 0x0001;  // 静态生命周期：write_flat 异步引用
        (void)ble_gattc_write_flat(conn, dsc->handle, &enable_notify,
                                   sizeof(enable_notify), NULL, NULL);
    }
    return 0;
}

static void write_cccd(uint16_t conn, uint16_t chr_val_handle) {
    int rc = ble_gattc_disc_all_dscs(conn, chr_val_handle + 1, chr_val_handle + 5,
                                     on_cccd_dsc, NULL);
    if (rc != 0) {
        ESP_LOGI(TAG, "ATVV CCCD 枚举启动失败 rc=%d（小米可免 CCCD，继续）", rc);
    }
}

static int on_atvv_svc(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg) {
    struct atvv_range *range = arg;
    if (error->status == BLE_HS_EDONE) {
        if (range->start == 0) {
            ESP_LOGW(TAG, "小米侧未发现 ATVV 服务（语音不可用，按键不受影响）");
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn, range->start, range->end, on_atvv_chr, range);
        if (rc != 0) {
            ESP_LOGE(TAG, "ATVV disc_all_chrs 失败 rc=%d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "ATVV 服务枚举失败 rc=%d", error->status);
        return 0;
    }
    if (ble_uuid_cmp(&svc->uuid.u, &UUID_ATVV_SVC.u) == 0) {
        range->start = svc->start_handle;
        range->end = svc->end_handle;
    }
    return 0;
}

static int on_atvv_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == BLE_HS_EDONE) {
        if (s_tx_handle == 0 || s_audio_handle == 0 || s_ctrl_handle == 0) {
            ESP_LOGW(TAG, "ATVV 特征不全 tx=%u audio=%u ctrl=%u（语音不可用）",
                     s_tx_handle, s_audio_handle, s_ctrl_handle);
            return 0;
        }
        write_cccd(conn, s_audio_handle);
        write_cccd(conn, s_ctrl_handle);
        // 握手启动：GET_CAPS 写 TX（caps 超时由 tick 兜底）
        gateway_atvv_action_t actions[GATEWAY_ATVV_MAX_ACTIONS];
        size_t n = gateway_atvv_session_start(&s_session, now_ms(), actions,
                                              GATEWAY_ATVV_MAX_ACTIONS);
        consume_actions(actions, n);
        s_link_up = true;
        ESP_LOGI(TAG, "ATVV 链路就绪 tx=0x%02x audio=0x%02x ctrl=0x%02x",
                 s_tx_handle, s_audio_handle, s_ctrl_handle);
        return 0;
    }
    if (error->status != 0 || chr == NULL) {
        return 0;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_TX.u) == 0 &&
        (chr->properties & CHR_PROP_WRITE)) {
        s_tx_handle = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_AUDIO.u) == 0 &&
               (chr->properties & CHR_PROP_NOTIFY)) {
        s_audio_handle = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_CTRL.u) == 0 &&
               (chr->properties & CHR_PROP_NOTIFY)) {
        s_ctrl_handle = chr->val_handle;
    }
    return 0;
}

static void disc_cb(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    // 连接可能已被对端掐断而 DISCONNECT 未处理（竞态窗），二次确认
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) {
        return;
    }
    s_tx_handle = s_audio_handle = s_ctrl_handle = 0;
    static struct atvv_range range;  // 静态：异步回调链携带
    range.start = range.end = 0;
    int rc = ble_gattc_disc_all_svcs(s_conn, on_atvv_svc, &range);
    if (rc == BLE_HS_EBUSY) {
        // HID 侧 GATT 过程（CCCD 写）尚未收尾，稍后重试
        ble_npl_callout_reset(&s_disc_callout, DISC_RETRY_TICKS);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ATVV disc_all_svcs 失败 rc=%d，重试", rc);
        ble_npl_callout_reset(&s_disc_callout, DISC_RETRY_TICKS);
    }
}

static void tick_cb(struct ble_npl_event *ev) {
    (void)ev;
    if (!s_running || !s_link_up) {
        return;
    }
    gateway_atvv_action_t actions[GATEWAY_ATVV_MAX_ACTIONS];
    size_t n = gateway_atvv_session_tick(&s_session, now_ms(), actions,
                                         GATEWAY_ATVV_MAX_ACTIONS);
    consume_actions(actions, n);
    // 网关模式常开周期 tick：驱动 caps 超时/尾包宽限/重开拒绝窗到期
    ble_npl_callout_reset(&s_tick_callout, TICK_PERIOD_TICKS);
}

// ---- 对外接口 ----

esp_err_t xiaomi_atvv_client_start(xiaomi_atvv_press_cb_t on_press,
                                   xiaomi_atvv_pcm_cb_t on_pcm) {
    if (on_press == NULL || on_pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_on_press = on_press;
    s_on_pcm = on_pcm;
    s_running = true;
    gateway_atvv_session_reset(&s_session);
    if (!s_disc_callout_inited) {
        ble_npl_callout_init(&s_disc_callout, nimble_port_get_dflt_eventq(), disc_cb, NULL);
        s_disc_callout_inited = true;
    }
    if (!s_tick_callout_inited) {
        ble_npl_callout_init(&s_tick_callout, nimble_port_get_dflt_eventq(), tick_cb, NULL);
        s_tick_callout_inited = true;
    }
    ble_npl_callout_reset(&s_tick_callout, TICK_PERIOD_TICKS);
    ESP_LOGI(TAG, "ATVV 语音链路模块启动");
    return ESP_OK;
}

void xiaomi_atvv_client_stop(void) {
    s_running = false;
    s_link_up = false;
    ble_npl_callout_stop(&s_disc_callout);
    ble_npl_callout_stop(&s_tick_callout);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        gateway_atvv_action_t actions[GATEWAY_ATVV_MAX_ACTIONS];
        size_t n = gateway_atvv_session_stop(&s_session, actions,
                                             GATEWAY_ATVV_MAX_ACTIONS);
        consume_actions(actions, n);
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_tx_handle = s_audio_handle = s_ctrl_handle = 0;
    gateway_atvv_session_reset(&s_session);
    ESP_LOGI(TAG, "ATVV 语音链路模块停止");
}

void xiaomi_atvv_client_on_link_ready(uint16_t conn) {
    if (!s_running) {
        return;
    }
    s_conn = conn;
    s_link_up = false;
    // 延迟发起发现：HID 订阅 CCCD 写尚未收尾（NimBLE 单连接单 GATT 过程）
    ble_npl_callout_reset(&s_disc_callout, DISC_DELAY_TICKS);
}

void xiaomi_atvv_client_on_link_down(void) {
    s_link_up = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_tx_handle = s_audio_handle = s_ctrl_handle = 0;
    ble_npl_callout_stop(&s_disc_callout);
    // 连接已断，MIC_CLOSE 不可发；直接复位（重连后重新握手）
    gateway_atvv_session_reset(&s_session);
    ESP_LOGI(TAG, "ATVV 链路断开，会话复位");
}

bool xiaomi_atvv_client_voice_pressed(void) {
    // 仅 STREAMING 算按住：DRAINING 时 STOP 已到（键已松开，尾包宽限中），
    // 否则 hold 阈值定时器会把短击误判成长按。
    return s_session.state == GATEWAY_ATVV_STATE_STREAMING;
}
