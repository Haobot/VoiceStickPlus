// voice_net：Wi-Fi STA 主流程。
//
// 设计要点：
// 1. 全部状态变更走单线程 esp_event 回调，避免与 BLE 任务竞争。
// 2. 状态结构（s_status）由 g_status_mutex 保护，因为 voice_net_publish_status 可能
//    从 BLE 控制回调线程调用。
// 3. wifi_status JSON 拼装在快照基础上做一次性 snprintf，长度上限按协议表 RFC 估算。
// 4. 错误码"首次写入保留"——last_error 非空时后续瞬态事件不能覆盖，
//    直到下次成功事件或 wifi_clear 才清零。
// 5. 不持有 mDNS / SNTP / HTTPS OTA 的资源——这些下一轮按 Doc/Plan §5 步骤 7-9 接入。
//
// 凭据 NVS 命名空间 "voicestick"，与 voice_ble 现有用法互不干扰；
// voice_net_init 假设 voice_ble_init 已经调过 nvs_flash_init。

#include "voice_net.h"
#include "voice_net_internal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/ip_addr.h"

static const char *TAG = "voice_net";

#define WIFI_CONNECT_TIMEOUT_MS   30000    // 协议 §3.3：30s 拿不到 IP 视为 timeout
#define WIFI_APPLY_DELAY_MS       800      // 协议 §3.1：让 BLE 回包先走完再 reconnect

// Wi-Fi 按需启停空闲倒计时（秒），详见 Doc/Plan/wifi-on-demand-power-management.md
#define WIFI_IDLE_TIMEOUT_SEC        60    // 通用空闲倒计时
#define WIFI_IDLE_SCAN_TIMEOUT_SEC   10    // 扫描完成后短倒计时
#define WIFI_IDLE_OTA_POST_SEC       30    // OTA 完成后额外窗口
#define WIFI_IDLE_PROVISION_SEC      60    // 配网验证窗口
#define WIFI_IDLE_DISCONNECTED_SEC   10    // 异常断连后短倒计时

// 专用 voice_net worker task 处理所有 esp_wifi 重活：esp_wifi_set_config / connect /
// disconnect 单次调用要 ~3-4 KB 局部 buffer（wpa_supplicant 链路），Tmr Svc (2048) 和
// sys_evt (2304) 都不够。专用 task 栈 6 KB，与其他任务隔离，影响最小。
ESP_EVENT_DEFINE_BASE(VOICE_NET_EVENT_BASE);
typedef enum {
    VN_CMD_APPLY_CREDENTIALS = 1,
    VN_CMD_CONNECT_TIMEOUT,
    VN_CMD_START_DISCOVERY,    // 拿到 IP 后启动 mDNS + SNTP，跑在 6KB worker 栈上
    VN_CMD_PUBLISH_STATUS,     // 状态变化异步发布，避免 sys_evt 栈溢出
    VN_CMD_DO_SCAN,            // Wi-Fi 扫描，跑在 6KB worker 栈上
    VN_CMD_IDLE_TIMEOUT,       // 空闲倒计时归零 → 关闭 Wi-Fi 射频
    VN_CMD_STOP_WIFI,          // 立即关闭 Wi-Fi 射频（录音触发 / wifi_clear）
} voice_net_cmd_t;

#define VOICE_NET_TASK_STACK_SIZE 6144
#define VOICE_NET_TASK_PRIORITY   5
#define VOICE_NET_QUEUE_LENGTH    8

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t  s_task_handle = NULL;

// 协议 §3.2 wifi_status.state 枚举。
typedef enum {
    NET_STATE_DISABLED = 0,
    NET_STATE_CONFIGURED,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_DISCONNECTED,
    NET_STATE_ERROR,
} net_state_t;

static const char *state_to_string(net_state_t s)
{
    switch (s) {
    case NET_STATE_DISABLED:     return "disabled";
    case NET_STATE_CONFIGURED:   return "configured";
    case NET_STATE_CONNECTING:   return "connecting";
    case NET_STATE_CONNECTED:    return "connected";
    case NET_STATE_DISCONNECTED: return "disconnected";
    case NET_STATE_ERROR:        return "error";
    default:                     return "disabled";
    }
}

typedef struct {
    net_state_t state;
    char        ssid[33];
    char        ip[16];
    int         rssi;
    bool        has_rssi;
    char        last_error[24];  // 协议 §3.3 错误码枚举；"" 表示无错
} wifi_snapshot_t;

static voice_net_status_publish_fn s_publish = NULL;
static voice_net_park_query_fn     s_park_query = NULL;
static voice_net_status_changed_fn s_status_changed_cb = NULL;
static SemaphoreHandle_t           s_status_mutex = NULL;
static wifi_snapshot_t             s_status;
static TimerHandle_t               s_apply_timer = NULL;
static TimerHandle_t               s_connect_timeout_timer = NULL;
static char                        s_pending_ssid[33];
static char                        s_pending_password[64];
static atomic_bool                 s_inited = ATOMIC_VAR_INIT(false);
// Wi-Fi 栈是否真正 esp_wifi_start 过——延迟到第一次需要连接时再启动，
// 避免 boot 期间 esp_wifi_init+start 与 BLE 抢 RF/modem 资源导致 BLE 反复断连
// （表现为 UI Pairing 界面反复闪烁）。
static bool                        s_wifi_started = false;

// Wi-Fi 按需启停：空闲倒计时租约类型。
typedef enum {
    LEASE_NONE = 0,       // Wi-Fi 未启动或已关闭
    LEASE_SCAN,           // 扫描完成后，短租约
    LEASE_PROVISION,      // 配网验证期
    LEASE_OTA_POST,       // OTA 下载完成后
    LEASE_DEFAULT,        // 通用
} wifi_lease_reason_t;

static TimerHandle_t               s_idle_timer = NULL;
static wifi_lease_reason_t         s_lease_reason = LEASE_NONE;
static atomic_bool                 s_wifi_stopping = ATOMIC_VAR_INIT(false);

// JSON 字符串字段转义（与桌面端 BleProtocol::JsonEscape 一致：
// 双引号 / 反斜杠 / 控制字符）。
static void json_escape_into(char *dst, size_t cap, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src && src[i] && di + 2 < cap; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (di + 3 >= cap) break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if (c == '\n') {
            if (di + 3 >= cap) break;
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c == '\r') {
            if (di + 3 >= cap) break;
            dst[di++] = '\\';
            dst[di++] = 'r';
        } else if (c == '\t') {
            if (di + 3 >= cap) break;
            dst[di++] = '\\';
            dst[di++] = 't';
        } else if ((unsigned char)c < 0x20) {
            // 其他控制字符直接丢，避免 JSON 解析端炸掉
            continue;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

// 协议 §3.2 完整快照。本 MVP 不接 HTTPS OTA / pending_verify，
// 但仍按完整字段输出，保持桌面端 ParseStateEvent 一次接通。
static void build_status_json(char *dst, size_t cap)
{
    char ssid_esc[2 * sizeof(s_status.ssid) + 1] = {0};
    char err_esc[2 * sizeof(s_status.last_error) + 1] = {0};

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    const net_state_t state = s_status.state;
    json_escape_into(ssid_esc, sizeof(ssid_esc), s_status.ssid);
    json_escape_into(err_esc, sizeof(err_esc), s_status.last_error);
    const bool has_rssi = s_status.has_rssi;
    const int rssi = s_status.rssi;
    char ip_local[sizeof(s_status.ip)];
    memcpy(ip_local, s_status.ip, sizeof(s_status.ip));
    xSemaphoreGive(s_status_mutex);

    // ota_pull 子对象从 voice_net_ota 模块读真实状态；
    // park_locked 通过注入的 query 回调实时计算：录音空闲且 BLE OTA 不在跑就 true。
    const char *ota_state_str = voice_net_ota_state_string(voice_net_ota_get_state());
    const int   ota_pct = voice_net_ota_get_progress_pct();
    // 状态帧中不再携带 ota_pull.url：完整 URL 可能很长，加上 Wi-Fi 字段后很容易超过
    // BLE MTU（Windows 协商后约 244 字节），导致桌面端 ParseStateEvent 失败、UI 不更新。
    // OTA 进度/错误只需要 state、progress_pct、last_error，URL 由桌面端自己持有。
    char        ota_err_esc[2 * 24 + 1] = {0};
    json_escape_into(ota_err_esc, sizeof(ota_err_esc), voice_net_ota_get_last_error());
    const bool park_locked = s_park_query ? s_park_query() : true;
    const bool pending_verify = voice_net_is_pending_verify();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *running_partition = running ? running->label : "unknown";

    const bool radio_on = s_wifi_started;

    if (has_rssi) {
        snprintf(dst, cap,
            "{\"event\":\"wifi_status\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
            "\"rssi\":%d,\"last_error\":\"%s\",\"radio_on\":%s,"
            "\"ota_pull\":{\"state\":\"%s\",\"progress_pct\":%d,\"last_error\":\"%s\"},"
            "\"pending\":%s,\"partition\":\"%s\",\"park\":%s}",
            state_to_string(state), ssid_esc, ip_local, rssi, err_esc,
            radio_on ? "true" : "false",
            ota_state_str, ota_pct, ota_err_esc,
            pending_verify ? "true" : "false", running_partition,
            park_locked ? "true" : "false");
    } else {
        snprintf(dst, cap,
            "{\"event\":\"wifi_status\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
            "\"last_error\":\"%s\",\"radio_on\":%s,"
            "\"ota_pull\":{\"state\":\"%s\",\"progress_pct\":%d,\"last_error\":\"%s\"},"
            "\"pending\":%s,\"partition\":\"%s\",\"park\":%s}",
            state_to_string(state), ssid_esc, ip_local, err_esc,
            radio_on ? "true" : "false",
            ota_state_str, ota_pct, ota_err_esc,
            pending_verify ? "true" : "false", running_partition,
            park_locked ? "true" : "false");
    }
}

static void do_publish_locked_snapshot(void)
{
    if (!s_publish) return;
    char buf[768];
    build_status_json(buf, sizeof(buf));
    esp_err_t err = s_publish(buf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // BLE 未连接时 send_state_json 会返 INVALID_STATE，不算异常。
        ESP_LOGD(TAG, "publish failed: %s", esp_err_to_name(err));
    }
}

// 同步发布：仅用于 BLE 主动请求（wifi_status_request），避免客户端订阅后立刻 request
// 却因 worker 排队而错过。调用方必须确保栈够。
static void publish_locked_snapshot_sync(void)
{
    do_publish_locked_snapshot();
}

// 异步发布：用于 sys_evt / timer / OTA task 等内部状态变化，统一转到 voice_net_task
// 6KB 栈上拼 JSON，避免 sys_evt 栈溢出。
static void publish_locked_snapshot_async(void)
{
    if (!s_cmd_queue) {
        do_publish_locked_snapshot();
        return;
    }
    voice_net_cmd_t cmd = VN_CMD_PUBLISH_STATUS;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
}

static void publish_locked_snapshot(void)
{
    publish_locked_snapshot_async();
}

// ── Wi-Fi 按需启停：空闲倒计时与射频关闭 ──
// 详见 Doc/Plan/wifi-on-demand-power-management.md

// 前向声明（新函数块中的内部函数在此之后才定义，但被前面的函数调用）
static void set_state(net_state_t new_state, bool clear_error);
static void wifi_idle_timer_reset(wifi_lease_reason_t reason);

// 条件检查：是否可以安全关闭 WiFi 射频。
// 有 HTTPS OTA 进行中或 Park gate 未锁定（录音/BLE OTA 中）时拒绝关闭。
static bool wifi_stop_is_safe(void)
{
    if (voice_net_ota_is_active()) return false;
    if (s_park_query && !s_park_query()) return false;
    return true;
}

// 将 lease_reason 映射为倒计时秒数。
static int idle_timeout_for_reason(wifi_lease_reason_t reason)
{
    switch (reason) {
    case LEASE_SCAN:       return WIFI_IDLE_SCAN_TIMEOUT_SEC;
    case LEASE_PROVISION:  return WIFI_IDLE_PROVISION_SEC;
    case LEASE_OTA_POST:   return WIFI_IDLE_OTA_POST_SEC;
    case LEASE_DEFAULT:    return WIFI_IDLE_TIMEOUT_SEC;
    default:               return WIFI_IDLE_TIMEOUT_SEC;
    }
}

// 执行真正的 esp_wifi_stop。跑在 voice_net_task 上（6KB 栈），
// 避免在 timer callback (Tmr Svc 2KB) 或 sys_evt (2.3KB) 中调用大栈 API。
static void do_wifi_stop(void)
{
    if (!s_wifi_started) return;
    if (atomic_exchange(&s_wifi_stopping, true)) {
        ESP_LOGD(TAG, "wifi_stop already in progress");
        return;
    }

    if (!wifi_stop_is_safe()) {
        ESP_LOGW(TAG, "wifi_stop blocked: not safe (OTA active or park unlocked)");
        atomic_store(&s_wifi_stopping, false);
        // 重置倒计时，下次再试
        if (s_lease_reason != LEASE_NONE) {
            wifi_idle_timer_reset(s_lease_reason);
        }
        return;
    }

    ESP_LOGI(TAG, "stopping Wi-Fi radio (lease=%d)", (int)s_lease_reason);
    s_lease_reason = LEASE_NONE;
    xTimerStop(s_idle_timer, 0);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    s_wifi_started = false;
    atomic_store(&s_wifi_stopping, false);

    // 清掉 IP/RSSI 等动态字段，保留 ssid（凭据还在 NVS，下次 resume 还能用）
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.ip[0] = '\0';
    s_status.has_rssi = false;
    xSemaphoreGive(s_status_mutex);

    set_state(NET_STATE_DISABLED, true);
    ESP_LOGI(TAG, "Wi-Fi radio stopped");
}

// 空闲倒计时到期回调（跑在 Tmr Svc 上下文）。投递到 voice_net_task 执行实际关闭。
static void wifi_idle_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    voice_net_cmd_t cmd = VN_CMD_IDLE_TIMEOUT;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

// 处理空闲超时命令（跑在 voice_net_task 上）。
static void do_idle_timeout(void)
{
    if (!wifi_stop_is_safe()) {
        // 当前不安全（OTA 刚启动或录音中），延长倒计时再试
        ESP_LOGD(TAG, "idle timeout deferred: not safe to stop");
        if (s_lease_reason != LEASE_NONE) {
            wifi_idle_timer_reset(s_lease_reason);
        }
        return;
    }
    do_wifi_stop();
}

// 重置/启动空闲倒计时。根据 reason 选择时长。
static void wifi_idle_timer_reset(wifi_lease_reason_t reason)
{
    if (!s_wifi_started) return;
    if (voice_net_ota_is_active()) {
        ESP_LOGD(TAG, "idle timer skipped: OTA in progress");
        return;  // OTA 期间不启动倒计时
    }

    s_lease_reason = reason;
    int timeout_sec = idle_timeout_for_reason(reason);
    xTimerStop(s_idle_timer, 0);
    xTimerChangePeriod(s_idle_timer, pdMS_TO_TICKS(timeout_sec * 1000), 0);
    xTimerStart(s_idle_timer, 0);
    ESP_LOGD(TAG, "idle timer reset: reason=%d timeout=%ds", (int)reason, timeout_sec);
}

// 进入新状态。clear_error=true 时把 last_error 清空（成功路径用），
// false 时保留首次错误（瞬态事件不能覆盖原因）。
static void set_state(net_state_t new_state, bool clear_error)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = new_state;
    if (clear_error) {
        s_status.last_error[0] = '\0';
    }
    // disabled / error / disconnected 是"无目标"状态：清掉 IP/RSSI 避免 UI 显示陈旧数据。
    // connecting 保留 ssid（提前写入的目标 SSID）和 ip（重连场景下沿用上一次的 IP 直到拿到新值），
    // connected 由 GOT_IP 自己写值。
    if (new_state == NET_STATE_DISABLED || new_state == NET_STATE_ERROR
        || new_state == NET_STATE_DISCONNECTED) {
        s_status.ip[0] = '\0';
        s_status.has_rssi = false;
    }
    xSemaphoreGive(s_status_mutex);

    if (s_status_changed_cb) {
        char ssid[33] = {0};
        char ip[16] = {0};
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        strlcpy(ssid, s_status.ssid, sizeof(ssid));
        strlcpy(ip, s_status.ip, sizeof(ip));
        xSemaphoreGive(s_status_mutex);
        s_status_changed_cb(ssid, ip, state_to_string(new_state));
    }

    publish_locked_snapshot();
}

// 首次写入保留：last_error 已有值时不覆盖。
static void set_last_error_once(const char *code)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool empty = s_status.last_error[0] == '\0';
    if (empty) {
        strncpy(s_status.last_error, code, sizeof(s_status.last_error) - 1);
        s_status.last_error[sizeof(s_status.last_error) - 1] = '\0';
    }
    xSemaphoreGive(s_status_mutex);
}

// 把 disconnect reason 映射到协议错误码。
static const char *reason_to_error_code(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "no_ssid";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "auth_failed";
    default:
        return NULL;  // 其他 reason 不算用户可读错误
    }
}

static void connect_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    // 投递到专用 worker task，避免在 Tmr Svc 栈里调 esp_wifi_disconnect。
    voice_net_cmd_t cmd = VN_CMD_CONNECT_TIMEOUT;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

static void do_connect_timeout(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    const bool still_connecting = s_status.state == NET_STATE_CONNECTING;
    xSemaphoreGive(s_status_mutex);
    if (!still_connecting) return;

    ESP_LOGW(TAG, "connect timeout");
    set_last_error_once("timeout");
    set_state(NET_STATE_ERROR, false);
    esp_wifi_disconnect();
}

static void start_connect_attempt(void)
{
    set_state(NET_STATE_CONNECTING, true);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        set_last_error_once("timeout");  // 找不到更精确的错误码就归到 timeout
        set_state(NET_STATE_ERROR, false);
        return;
    }
    xTimerStop(s_connect_timeout_timer, 0);
    xTimerChangePeriod(s_connect_timeout_timer,
                       pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS), 0);
    xTimerStart(s_connect_timeout_timer, 0);
}

// 首次需要拨号时才真正启动 Wi-Fi 栈。esp_wifi_init 已经在 voice_net_init 做过，
// 这里只 set_mode + start；esp_wifi_start 会触发 WIFI_EVENT_STA_START，
// 同时也是 BLE 短暂受冲击的瞬间——把它推迟到用户已经下发凭据时才发生。
static esp_err_t ensure_wifi_started(void)
{
    if (s_wifi_started) return ESP_OK;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;
    // 新启动的 Wi-Fi 栈进入默认租约：60s 内无操作自动关闭。
    wifi_idle_timer_reset(LEASE_DEFAULT);
    ESP_LOGI(TAG, "wifi stack started (lazy)");
    return ESP_OK;
}

static void apply_pending_credentials(TimerHandle_t timer)
{
    (void)timer;
    // 投递到专用 worker task，由它在 6KB 栈上跑 esp_wifi_set_config / connect。
    voice_net_cmd_t cmd = VN_CMD_APPLY_CREDENTIALS;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

// 真正干 esp_wifi 重活的函数，跑在专用 voice_net_task 上（栈 6KB）。
static void do_apply_pending_credentials(void)
{
    char ssid[33];
    char password[64];
    strncpy(ssid, s_pending_ssid, sizeof(ssid));
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(password, s_pending_password, sizeof(password));
    password[sizeof(password) - 1] = '\0';

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "apply skipped: empty ssid in pending slot");
        return;
    }

    if (ensure_wifi_started() != ESP_OK) {
        set_last_error_once("timeout");
        set_state(NET_STATE_ERROR, false);
        return;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    // 让 wpa_supplicant 自动选择最强 AP；阈值不卡，避免边缘信号被拒。
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        set_last_error_once("timeout");
        set_state(NET_STATE_ERROR, false);
        return;
    }

    // 缓存到 snapshot：UI 在 connecting 阶段先看到 SSID。
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    s_status.ssid[sizeof(s_status.ssid) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);

    esp_wifi_disconnect();   // 如果已连别的 AP，先断开；新 connect 在下面发起
    start_connect_attempt();
}

// Wi-Fi 扫描：投递到 voice_net_task 后在此执行，确保栈够。
// 扫描结果由 WIFI_EVENT_SCAN_DONE 事件异步回报。
static void do_wifi_scan(void)
{
    if (ensure_wifi_started() != ESP_OK) {
        ESP_LOGE(TAG, "scan: wifi not started");
        // 推送空结果告知桌面端扫描失败，避免 UI 卡死
        if (s_publish) {
            s_publish("{\"event\":\"wifi_scan_result\",\"aps\":[]}");
        }
        return;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,        // 全频道扫描
        .show_hidden = true,  // 也显示隐藏 SSID 的 AP
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,   // 每频道最少 100ms
                .max = 300,   // 每频道最多 300ms
            }
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // true = 阻塞直到扫描完成
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        // 推送空 scan result 告知桌面端扫描失败
        if (s_publish) {
            s_publish("{\"event\":\"wifi_scan_result\",\"aps\":[]}");
        }
        return;
    }

    // 扫描完成，读取结果
    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK || ap_count == 0) {
        ESP_LOGI(TAG, "scan: no APs found");
        if (s_publish) {
            s_publish("{\"event\":\"wifi_scan_result\",\"aps\":[]}");
        }
        return;
    }

    // 限制读取数量，wifi_ap_record_t 每个约 196 字节
    uint16_t max_read = ap_count > 30 ? 30 : ap_count;
    wifi_ap_record_t *ap_records = calloc(max_read, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "scan: OOM for %u records", max_read);
        if (s_publish) {
            s_publish("{\"event\":\"wifi_scan_result\",\"aps\":[]}");
        }
        return;
    }

    err = esp_wifi_scan_get_ap_records(&max_read, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan get records failed: %s", esp_err_to_name(err));
        free(ap_records);
        if (s_publish) {
            s_publish("{\"event\":\"wifi_scan_result\",\"aps\":[]}");
        }
        return;
    }

    // 过滤：仅 2.4GHz、非空 SSID，按 RSSI 降序。
    // 2.4GHz 信道范围 1-14。
    // BLE 通知 MTU 通常 512 字节，扣除 ATT 头(3) + state 帧头(4) 后
    // JSON 安全上限约 470 字节。每个 AP 条目约 40-70 字节，
    // MAX_SCAN_RESULTS=8 保证典型 SSID 不超限，但仍以 JSON 字节数
    // 为最终截断依据（见下方 MAX_SCAN_JSON_BYTES）。
    #define MAX_SCAN_RESULTS 8
    #define MAX_SCAN_JSON_BYTES 470

    // 先筛选有效 AP
    wifi_ap_record_t filtered[MAX_SCAN_RESULTS];
    int filtered_count = 0;

    for (uint16_t i = 0; i < max_read && filtered_count < MAX_SCAN_RESULTS; i++) {
        wifi_ap_record_t *ap = &ap_records[i];
        // 跳过非 2.4GHz（primary channel 不在 1-14 范围）和空 SSID
        if (ap->primary < 1 || ap->primary > 14) continue;
        if (ap->ssid[0] == '\0') continue;
        filtered[filtered_count++] = *ap;
    }

    // 按 RSSI 降序冒泡排序
    for (int i = 0; i < filtered_count - 1; i++) {
        for (int j = i + 1; j < filtered_count; j++) {
            if (filtered[j].rssi > filtered[i].rssi) {
                wifi_ap_record_t tmp = filtered[i];
                filtered[i] = filtered[j];
                filtered[j] = tmp;
            }
        }
    }

    // 构建 scan_result JSON。auth 映射到 ESP Wi-Fi 标准枚举值：
    // 0=OPEN, 1=WEP, 2=WPA_PSK, 3=WPA2_PSK, 4=WPA_WPA2_PSK,
    // 5=WPA2_ENTERPRISE, 6=WPA3_PSK, 7=WPA2_WPA3_PSK
    // JSON 受 BLE MTU 约束：超过 MAX_SCAN_JSON_BYTES 的 AP 被截断。
    // 不使用 snprintf(NULL,0,…) 预估——ESP32 newlib 对此行为不一。
    // 改为：用固定上限预检，snprintf 时通过 remain 控制写入量，
    // 返回 >remain 表示被截断，立即停止。
    char buf[768];
    int off = snprintf(buf, sizeof(buf), "{\"event\":\"wifi_scan_result\",\"aps\":[");
    int published_count = 0;
    for (int i = 0; i < filtered_count; i++) {
        char ssid_esc[2 * 33] = {0};
        json_escape_into(ssid_esc, sizeof(ssid_esc), (const char *)filtered[i].ssid);

        // 预留 4 字节给结尾 "]}"、"\0" 和 ',' 前导（最坏 1 字节额外）
        int remain = MAX_SCAN_JSON_BYTES - off - 3;
        if (remain <= 0) {
            ESP_LOGI(TAG, "scan: capped at %d APs (json limit %d bytes)",
                     i, MAX_SCAN_JSON_BYTES);
            break;
        }
        // 写入时留 1 字节给 '\0'；snprintf 返回 >= remain+1 表示被截断。
        int written = snprintf(buf + off, remain + 1,
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                               i > 0 ? "," : "",
                               ssid_esc,
                               filtered[i].rssi,
                               (int)filtered[i].authmode);
        if (written < 0 || written > remain) {
            ESP_LOGI(TAG, "scan: capped at %d APs (chunk too large)", i);
            break;
        }
        off += written;
        published_count = i + 1;
    }
    // 安全关闭 JSON（buf 有 768 字节，结尾足够）
    snprintf(buf + off, sizeof(buf) - off, "]}");

    int final_json_len = (int)strlen(buf);
    ESP_LOGI(TAG, "scan done: %d scanned, %d published, json_len=%d",
             filtered_count, published_count, final_json_len);

    if (s_publish) {
        esp_err_t pub_err = s_publish(buf);
        if (pub_err != ESP_OK) {
            ESP_LOGE(TAG, "scan publish failed: %s (json_len=%d)",
                     esp_err_to_name(pub_err), final_json_len);
            // 如果因 JSON 太大失败，尝试只发信号最强的 AP
            if (published_count > 1) {
                ESP_LOGW(TAG, "scan retry with single AP");
                int retry_off = snprintf(buf, sizeof(buf),
                                         "{\"event\":\"wifi_scan_result\",\"aps\":[");
                char retry_esc[2 * 33] = {0};
                json_escape_into(retry_esc, sizeof(retry_esc),
                                 (const char *)filtered[0].ssid);
                retry_off += snprintf(buf + retry_off, sizeof(buf) - retry_off,
                                      "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                                      retry_esc, filtered[0].rssi,
                                      (int)filtered[0].authmode);
                snprintf(buf + retry_off, sizeof(buf) - retry_off, "]}");
                ESP_LOGI(TAG, "scan retry json_len=%d", (int)strlen(buf));
                pub_err = s_publish(buf);
                if (pub_err != ESP_OK) {
                    ESP_LOGE(TAG, "scan retry publish also failed: %s",
                             esp_err_to_name(pub_err));
                }
            }
        }
    }

    free(ap_records);
    #undef MAX_SCAN_RESULTS
    #undef MAX_SCAN_JSON_BYTES

    // 扫描完成后重置空闲倒计时为短租约（10s），超时自动关闭 Wi-Fi 射频。
    wifi_idle_timer_reset(LEASE_SCAN);
}

// 专用 worker task：从队列消费 cmd，独立 6KB 栈安全调用 esp_wifi / mdns / sntp API。
static void voice_net_task(void *arg)
{
    (void)arg;
    voice_net_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        switch (cmd) {
        case VN_CMD_APPLY_CREDENTIALS:
            do_apply_pending_credentials();
            break;
        case VN_CMD_CONNECT_TIMEOUT:
            do_connect_timeout();
            break;
        case VN_CMD_START_DISCOVERY:
            // mdns_init / esp_netif_sntp_init 内部都会调 lwip 链路 API，
            // 跑在专用 task 上避免 sys_evt 栈不够。
            voice_net_discovery_start_mdns();
            voice_net_discovery_start_sntp();
            break;
        case VN_CMD_PUBLISH_STATUS:
            do_publish_locked_snapshot();
            break;
        case VN_CMD_DO_SCAN:
            do_wifi_scan();
            break;
        case VN_CMD_IDLE_TIMEOUT:
            do_idle_timeout();
            break;
        case VN_CMD_STOP_WIFI:
            do_wifi_stop();
            break;
        default:
            ESP_LOGW(TAG, "unknown cmd %d", cmd);
            break;
        }
    }
}

// Wi-Fi/IP event 路由。注意 esp_event 在专用 task 中调用，与 BLE 任务并发安全。
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
            // 启动后是否立即 connect 由 voice_net_init / apply_pending_credentials 决定。
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "STA_DISCONNECTED reason=%u", e ? e->reason : 0);
            const char *code = e ? reason_to_error_code(e->reason) : NULL;
            if (code) {
                // 终端错误（auth_failed / no_ssid）：自动重连无望，启动短倒计时关闭 Wi-Fi。
                set_last_error_once(code);
                set_state(NET_STATE_ERROR, false);
                wifi_idle_timer_reset(LEASE_SCAN);  // 复用 10s 短租约
            } else {
                // 已连接后被踢，回到 disconnected；不视为用户可见错误。
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                bool was_connected = s_status.state == NET_STATE_CONNECTED;
                xSemaphoreGive(s_status_mutex);
                set_state(was_connected ? NET_STATE_DISCONNECTED : NET_STATE_CONNECTING,
                          false);
                if (!was_connected) {
                    // 仍在尝试阶段，等 connect_timeout 兜底，不主动重试避免抖动
                }
                // 非终端断连：保持当前租约（Wi-Fi 栈会自动重连）。
                // 若长时间无法恢复，由现有租约倒计时兜底关闭。
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        xTimerStop(s_connect_timeout_timer, 0);

        wifi_ap_record_t ap = {0};
        bool have_rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        if (have_rssi) {
            s_status.rssi = ap.rssi;
            s_status.has_rssi = true;
        } else {
            s_status.has_rssi = false;
        }
        xSemaphoreGive(s_status_mutex);

        ESP_LOGI(TAG, "GOT_IP ip=" IPSTR " rssi=%d", IP2STR(&e->ip_info.ip),
                 have_rssi ? ap.rssi : 0);
        set_state(NET_STATE_CONNECTED, true);  // 成功路径清掉 last_error

        // 连接成功后重置空闲倒计时为配网/连接验证窗口（60s）。
        // 此后若没有 scan / ota_pull 等操作刷新租约，Wi-Fi 会在倒计时归零后自动关闭。
        wifi_idle_timer_reset(LEASE_PROVISION);

        // 首次拿到 IP 后启动 mDNS + SNTP，跑在 voice_net_task 上避免 sys_evt 栈不够。
        // 内部 idempotent；后续断开重连不会重复启动。
        voice_net_cmd_t disc_cmd = VN_CMD_START_DISCOVERY;
        xQueueSend(s_cmd_queue, &disc_cmd, pdMS_TO_TICKS(100));
    }
}

esp_err_t voice_net_init(voice_net_status_publish_fn publish)
{
    if (!publish) return ESP_ERR_INVALID_ARG;
    if (atomic_exchange(&s_inited, true)) {
        ESP_LOGW(TAG, "already inited");
        return ESP_OK;
    }

    s_publish = publish;

    // 最早期检测 PENDING_VERIFY：必须在任何耗时初始化前调，固定 boot uptime 基准。
    voice_net_ota_detect_pending_verify();

    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) return ESP_ERR_NO_MEM;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NET_STATE_DISABLED;

    s_apply_timer = xTimerCreate("vn_apply", pdMS_TO_TICKS(WIFI_APPLY_DELAY_MS),
                                 pdFALSE, NULL, apply_pending_credentials);
    s_connect_timeout_timer = xTimerCreate("vn_conn_to",
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS),
                                           pdFALSE, NULL, connect_timeout_cb);
    s_idle_timer = xTimerCreate("vn_idle", pdMS_TO_TICKS(WIFI_IDLE_TIMEOUT_SEC * 1000),
                                pdFALSE, NULL, wifi_idle_timeout_cb);
    if (!s_apply_timer || !s_connect_timeout_timer || !s_idle_timer) return ESP_ERR_NO_MEM;

    // esp_netif / esp_event：voicestick 主链路从未启用过 Wi-Fi，这里第一次初始化。
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;

    // 已经有人创建 default loop（NimBLE 在某些场景需要）也无所谓。
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // 专用 worker task，6KB 栈，安全调用 esp_wifi_set_config 等大栈 API。
    s_cmd_queue = xQueueCreate(VOICE_NET_QUEUE_LENGTH, sizeof(voice_net_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(voice_net_task, "voice_net", VOICE_NET_TASK_STACK_SIZE, NULL,
                    VOICE_NET_TASK_PRIORITY, &s_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // 关键：不在这里 esp_wifi_set_mode/start——延迟到第一次拨号（apply_pending_credentials）。
    // 避免 boot 阶段 Wi-Fi 栈与 BLE 抢资源造成 UI 反复闪 Pairing。

    // 加载持久化凭据到 pending 槽，但 *不立即拨号*。
    // main.c 在 BLE 连上后调 voice_net_resume_if_configured() 才真正发起连接。
    char ssid[33] = {0};
    char password[64] = {0};
    bool enabled = false;
    if (voice_net_nvs_load(ssid, sizeof(ssid), password, sizeof(password), &enabled) == ESP_OK
        && enabled && ssid[0] != '\0') {
        ESP_LOGI(TAG, "load credentials ssid=%s, deferred until BLE ready", ssid);
        strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
        s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
        strncpy(s_pending_password, password, sizeof(s_pending_password));
        s_pending_password[sizeof(s_pending_password) - 1] = '\0';
        // 预先把 SSID 写到 snapshot，UI 能看到已配置目标
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
        s_status.ssid[sizeof(s_status.ssid) - 1] = '\0';
        xSemaphoreGive(s_status_mutex);
        set_state(NET_STATE_CONFIGURED, true);
    } else {
        set_state(NET_STATE_DISABLED, true);
    }

    ESP_LOGI(TAG, "init ok");
    return ESP_OK;
}

void voice_net_resume_if_configured(void)
{
    if (!atomic_load(&s_inited)) return;
    if (s_pending_ssid[0] == '\0') return;  // 没有持久化凭据，留在 disabled
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool already_running = s_status.state == NET_STATE_CONNECTING
                        || s_status.state == NET_STATE_CONNECTED;
    xSemaphoreGive(s_status_mutex);
    if (already_running) return;

    ESP_LOGI(TAG, "resume from NVS: ssid=%s", s_pending_ssid);
    set_state(NET_STATE_CONNECTING, true);
    xTimerStop(s_apply_timer, 0);
    xTimerChangePeriod(s_apply_timer, pdMS_TO_TICKS(WIFI_APPLY_DELAY_MS), 0);
    xTimerStart(s_apply_timer, 0);
}

void voice_net_apply_credentials(const char *ssid, const char *password)
{
    if (!atomic_load(&s_inited)) {
        ESP_LOGW(TAG, "apply before init");
        return;
    }
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "apply: empty ssid");
        set_last_error_once("payload_too_large");  // 复用最接近的错误码
        publish_locked_snapshot();
        return;
    }

    // 按协议 §3.1 长度校验：超长丢弃并报 payload_too_large。
    if (strlen(ssid) > 32 || strlen(password ? password : "") > 63) {
        ESP_LOGW(TAG, "apply: payload too large ssid_len=%u pass_len=%u",
                 (unsigned)strlen(ssid),
                 (unsigned)(password ? strlen(password) : 0));
        set_last_error_once("payload_too_large");
        publish_locked_snapshot();
        return;
    }

    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_pending_password, password, sizeof(s_pending_password) - 1);
        s_pending_password[sizeof(s_pending_password) - 1] = '\0';
    } else {
        s_pending_password[0] = '\0';
    }

    esp_err_t err = voice_net_nvs_save(s_pending_ssid, s_pending_password);
    if (err != ESP_OK) {
        set_last_error_once("payload_too_large");  // NVS 写失败时也用此码（fallback）
        set_state(NET_STATE_ERROR, false);
        return;
    }

    // 提前把目标 SSID 写到 snapshot：让桌面端在 800ms 延迟 + 真正 connect 阶段就能
    // 看到"正在连接 <SSID>"，而不是 connecting+空 SSID 持续到 GOT_IP。
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.ssid, s_pending_ssid, sizeof(s_status.ssid) - 1);
    s_status.ssid[sizeof(s_status.ssid) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);

    // 立即进入 connecting 视觉态，但真正的 set_config + connect 延后 800ms 让 BLE 回包先走完。
    set_state(NET_STATE_CONNECTING, true);  // 新一轮尝试：清掉旧错误码
    xTimerStop(s_apply_timer, 0);
    xTimerChangePeriod(s_apply_timer, pdMS_TO_TICKS(WIFI_APPLY_DELAY_MS), 0);
    xTimerStart(s_apply_timer, 0);
}

void voice_net_clear_credentials(void)
{
    if (!atomic_load(&s_inited)) return;

    voice_net_nvs_clear();
    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';

    // 停止空闲倒计时并投递关闭命令（在 voice_net_task 上安全执行 esp_wifi_stop）。
    s_lease_reason = LEASE_NONE;
    xTimerStop(s_idle_timer, 0);
    esp_wifi_disconnect();
    voice_net_cmd_t cmd = VN_CMD_STOP_WIFI;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.ssid[0] = '\0';
    s_status.ip[0] = '\0';
    s_status.has_rssi = false;
    s_status.last_error[0] = '\0';
    xSemaphoreGive(s_status_mutex);

    set_state(NET_STATE_DISABLED, true);
}

void voice_net_publish_status(void)
{
    if (!atomic_load(&s_inited)) return;
    publish_locked_snapshot_sync();
}

void voice_net_set_park_query(voice_net_park_query_fn cb)
{
    s_park_query = cb;
}

void voice_net_set_status_changed_callback(voice_net_status_changed_fn cb)
{
    s_status_changed_cb = cb;
}

void voice_net_get_status(char *ssid, size_t ssid_size, char *ip, size_t ip_size, const char **state)
{
    if (ssid && ssid_size > 0) {
        ssid[0] = '\0';
    }
    if (ip && ip_size > 0) {
        ip[0] = '\0';
    }
    if (state) {
        *state = state_to_string(NET_STATE_DISABLED);
    }
    if (!atomic_load(&s_inited)) {
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    if (ssid && ssid_size > 0) {
        strlcpy(ssid, s_status.ssid, ssid_size);
    }
    if (ip && ip_size > 0) {
        strlcpy(ip, s_status.ip, ip_size);
    }
    if (state) {
        *state = state_to_string(s_status.state);
    }
    xSemaphoreGive(s_status_mutex);
}

void voice_net_start_scan(void)
{
    if (!atomic_load(&s_inited)) {
        ESP_LOGW(TAG, "scan: voice_net not inited");
        return;
    }
    if (!s_cmd_queue) {
        ESP_LOGW(TAG, "scan: cmd queue not ready");
        return;
    }
    voice_net_cmd_t cmd = VN_CMD_DO_SCAN;
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "scan: cmd queue full, dropping scan request");
    }
}

void voice_net_start_ota_pull(const char *url, const char *sha256_hex)
{
    if (!atomic_load(&s_inited)) {
        ESP_LOGW(TAG, "ota_pull: voice_net not inited");
        return;
    }
    // OTA 需要 Wi-Fi 射频已启动。如果之前因空闲倒计时或录音被关闭，
    // 这里重新启动。
    ensure_wifi_started();
    // 停止空闲倒计时——OTA 期间 Wi-Fi 由 voice_net_ota 完全控制。
    s_lease_reason = LEASE_NONE;
    xTimerStop(s_idle_timer, 0);
    // 如果 NVS 有凭据但当前未连接，触发快速重连（OTA 需要 IP）。
    if (s_pending_ssid[0] != '\0') {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        bool needs_connect = (s_status.state != NET_STATE_CONNECTED
                           && s_status.state != NET_STATE_CONNECTING);
        xSemaphoreGive(s_status_mutex);
        if (needs_connect) {
            ESP_LOGI(TAG, "ota_pull: triggering reconnect for %s", s_pending_ssid);
            set_state(NET_STATE_CONNECTING, true);
            xTimerStop(s_apply_timer, 0);
            xTimerChangePeriod(s_apply_timer, pdMS_TO_TICKS(100), 0);
            xTimerStart(s_apply_timer, 0);
        }
    }
    voice_net_start_ota_pull_internal(url, sha256_hex, s_park_query);
}

// ── 录音联动：录音开始时关闭 Wi-Fi 省电 + 避免 2.4GHz 同频干扰 ──

void voice_net_on_recording_started(void)
{
    if (!atomic_load(&s_inited)) return;
    if (!s_wifi_started) return;
    if (voice_net_ota_is_active()) {
        ESP_LOGD(TAG, "recording start: keep Wi-Fi for active OTA");
        return;
    }
    ESP_LOGI(TAG, "recording start: stopping Wi-Fi radio");
    // 立即停止空闲倒计时，投递关闭命令到 voice_net_task。
    s_lease_reason = LEASE_NONE;
    xTimerStop(s_idle_timer, 0);
    voice_net_cmd_t cmd = VN_CMD_STOP_WIFI;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

void voice_net_on_recording_stopped(void)
{
    // 录音结束不自动恢复 Wi-Fi——下次需要时由操作命令（scan / ota_pull / wifi_set）触发。
    // 保留为 hook，便于未来策略变化。
    (void)0;
}

// OTA 结束通知：由 voice_net_ota.c 在 OTA SUCCESS / FAILED 时调用。
// 重置空闲倒计时为 OTA 后短窗口（30s），给桌面端 commit 留时间，超时自动关闭 Wi-Fi。
void voice_net_notify_ota_ended(void)
{
    if (!atomic_load(&s_inited)) return;
    if (!s_wifi_started) return;
    ESP_LOGI(TAG, "OTA ended, restarting idle timer for post-OTA window");
    wifi_idle_timer_reset(LEASE_OTA_POST);
}
