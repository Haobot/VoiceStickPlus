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
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/ip_addr.h"

static const char *TAG = "voice_net";

#define WIFI_CONNECT_TIMEOUT_MS 30000      // 协议 §3.3：30s 拿不到 IP 视为 timeout
#define WIFI_APPLY_DELAY_MS     800        // 协议 §3.1：让 BLE 回包先走完再 reconnect

// 专用 voice_net worker task 处理所有 esp_wifi 重活：esp_wifi_set_config / connect /
// disconnect 单次调用要 ~3-4 KB 局部 buffer（wpa_supplicant 链路），Tmr Svc (2048) 和
// sys_evt (2304) 都不够。专用 task 栈 6 KB，与其他任务隔离，影响最小。
ESP_EVENT_DEFINE_BASE(VOICE_NET_EVENT_BASE);
typedef enum {
    VN_CMD_APPLY_CREDENTIALS = 1,
    VN_CMD_CONNECT_TIMEOUT,
    VN_CMD_START_DISCOVERY,    // 拿到 IP 后启动 mDNS + SNTP，跑在 6KB worker 栈上
    VN_CMD_PUBLISH_STATUS,     // 拼装 wifi_status JSON 并推送 BLE state_tx
                               // ——build_status_json 局部 buffer ~1.7KB，必须跑专用栈
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

    // ota_pull 子对象从 voice_net_ota 模块读真实状态；ota_pending_verify 暂留 false
    // （rollback 配置未启用，下一轮 §9 与 mark_app_valid 一起接入）。
    // park_locked 通过注入的 query 回调实时计算：录音空闲且 BLE OTA 不在跑就 true。
    const char *ota_state_str = voice_net_ota_state_string(voice_net_ota_get_state());
    const int   ota_pct = voice_net_ota_get_progress_pct();
    char        ota_url_esc[2 * 256 + 1] = {0};
    char        ota_err_esc[2 * 24 + 1] = {0};
    json_escape_into(ota_url_esc, sizeof(ota_url_esc), voice_net_ota_get_url());
    json_escape_into(ota_err_esc, sizeof(ota_err_esc), voice_net_ota_get_last_error());
    const bool park_locked = s_park_query ? s_park_query() : true;

    if (has_rssi) {
        snprintf(dst, cap,
            "{\"event\":\"wifi_status\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
            "\"rssi\":%d,\"last_error\":\"%s\","
            "\"ota_pull\":{\"state\":\"%s\",\"progress_pct\":%d,\"url\":\"%s\",\"last_error\":\"%s\"},"
            "\"ota_pending_verify\":false,\"park_locked\":%s}",
            state_to_string(state), ssid_esc, ip_local, rssi, err_esc,
            ota_state_str, ota_pct, ota_url_esc, ota_err_esc,
            park_locked ? "true" : "false");
    } else {
        snprintf(dst, cap,
            "{\"event\":\"wifi_status\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
            "\"last_error\":\"%s\","
            "\"ota_pull\":{\"state\":\"%s\",\"progress_pct\":%d,\"url\":\"%s\",\"last_error\":\"%s\"},"
            "\"ota_pending_verify\":false,\"park_locked\":%s}",
            state_to_string(state), ssid_esc, ip_local, err_esc,
            ota_state_str, ota_pct, ota_url_esc, ota_err_esc,
            park_locked ? "true" : "false");
    }
}

static void do_publish_locked_snapshot(void)
{
    if (!s_publish) return;
    char buf[1024];                          // url ≤256 + 转义 + 其他字段，1KB 足够
    build_status_json(buf, sizeof(buf));
    esp_err_t err = s_publish(buf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // BLE 未连接时 send_state_json 会返 INVALID_STATE，不算异常。
        ESP_LOGD(TAG, "publish failed: %s", esp_err_to_name(err));
    }
}

// 投递 publish 命令到 worker task：build_status_json 局部 buffer ~1.7KB，
// 不能在 sys_evt (2304B) 或 BLE 协议任务栈上直接跑。
static void publish_locked_snapshot(void)
{
    if (!s_cmd_queue) {
        // init 阶段队列还没建：少数路径会到这里，调直接版本（init 上下文栈足够）。
        do_publish_locked_snapshot();
        return;
    }
    voice_net_cmd_t cmd = VN_CMD_PUBLISH_STATUS;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
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
                set_last_error_once(code);
                set_state(NET_STATE_ERROR, false);
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

    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) return ESP_ERR_NO_MEM;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NET_STATE_DISABLED;

    s_apply_timer = xTimerCreate("vn_apply", pdMS_TO_TICKS(WIFI_APPLY_DELAY_MS),
                                 pdFALSE, NULL, apply_pending_credentials);
    s_connect_timeout_timer = xTimerCreate("vn_conn_to",
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS),
                                           pdFALSE, NULL, connect_timeout_cb);
    if (!s_apply_timer || !s_connect_timeout_timer) return ESP_ERR_NO_MEM;

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
    esp_wifi_disconnect();

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
    publish_locked_snapshot();
}

void voice_net_set_park_query(voice_net_park_query_fn cb)
{
    s_park_query = cb;
}

void voice_net_start_ota_pull(const char *url, const char *sha256_hex)
{
    if (!atomic_load(&s_inited)) {
        ESP_LOGW(TAG, "ota_pull: voice_net not inited");
        return;
    }
    voice_net_start_ota_pull_internal(url, sha256_hex, s_park_query);
}
