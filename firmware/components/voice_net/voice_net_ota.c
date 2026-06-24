// voice_net OTA pull 实现。
//
// 设计：
// - 桌面端通过 BLE 下发 {"event":"ota_pull","url":"..."} 触发固件主动拉取。
// - HTTPS 与局域网 HTTP 共用 esp_http_client + esp_ota_* streaming 路径。
// - https:// 继续走 IDF 证书包校验；sha256_hex 可选。
// - http:// 只允许私有 IPv4，且必须带 64 位 sha256_hex。
// - 下载时逐 chunk esp_ota_write，并同步 mbedtls_sha256_update，最后可比对外部
//   sha256_hex；不匹配时不 set_boot_partition。
// - Park gate 由 main.c 注入的 voice_net_park_query_fn 决定；未锁时拒绝并把
//   last_error 置为 ota_park_required。
// - 独立 voice_net_ota task（8KB 栈）跑 HTTP/TLS/OTA 重活，避免拖垮 BLE 和
//   voice_net worker task。

#include "voice_net_internal.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "voice_net_ota";

#define OTA_URL_MAX_LEN       256
#define OTA_TASK_STACK_SIZE   8192
#define OTA_TASK_PRIORITY     4
#define OTA_PROGRESS_STEP_PCT 5
#define OTA_READ_BUF_SIZE     (16 * 1024)

static atomic_bool s_ota_in_progress = ATOMIC_VAR_INIT(false);

// pending_verify 健康签到状态。
#define MARK_VALID_MIN_UPTIME_MS  10000
static atomic_bool s_pending_verify     = ATOMIC_VAR_INIT(false);
static atomic_bool s_app_valid_marked   = ATOMIC_VAR_INIT(false);
static atomic_bool s_ble_seen_connected = ATOMIC_VAR_INIT(false);
static int64_t     s_boot_uptime_anchor_us = 0;

// 上报字段，由 voice_net.c 的快照拼装函数读。
static voice_net_ota_state_t s_ota_state = VOICE_NET_OTA_STATE_IDLE;
static int                   s_ota_progress_pct = 0;
static char                  s_ota_url[OTA_URL_MAX_LEN + 1];
static char                  s_ota_last_error[24];

void voice_net_publish_status(void);

bool voice_net_ota_is_active(void) { return atomic_load(&s_ota_in_progress); }

voice_net_ota_state_t voice_net_ota_get_state(void) { return s_ota_state; }
int                   voice_net_ota_get_progress_pct(void) { return s_ota_progress_pct; }
const char           *voice_net_ota_get_url(void) { return s_ota_url; }
const char           *voice_net_ota_get_last_error(void) { return s_ota_last_error; }

static const char *state_to_string_ota(voice_net_ota_state_t s)
{
    switch (s) {
    case VOICE_NET_OTA_STATE_IDLE:        return "idle";
    case VOICE_NET_OTA_STATE_DOWNLOADING: return "downloading";
    case VOICE_NET_OTA_STATE_FINISHING:   return "finishing";
    case VOICE_NET_OTA_STATE_SUCCESS:     return "success";
    case VOICE_NET_OTA_STATE_FAILED:      return "failed";
    default:                              return "idle";
    }
}

const char *voice_net_ota_state_string(voice_net_ota_state_t s) { return state_to_string_ota(s); }

static void set_ota_error(const char *code)
{
    if (s_ota_last_error[0] != '\0') return;
    strncpy(s_ota_last_error, code, sizeof(s_ota_last_error) - 1);
    s_ota_last_error[sizeof(s_ota_last_error) - 1] = '\0';
}

static void set_ota_state(voice_net_ota_state_t st)
{
    s_ota_state = st;
    voice_net_publish_status();
}

typedef enum {
    OTA_SCHEME_INVALID = 0,
    OTA_SCHEME_HTTPS,
    OTA_SCHEME_LAN_HTTP,
} ota_scheme_t;

typedef struct {
    ota_scheme_t scheme;
    bool sha_required;
} ota_url_policy_t;

typedef struct {
    char url[OTA_URL_MAX_LEN + 1];
    char sha256_hex[65];
    ota_scheme_t scheme;
    bool has_sha256;
} ota_task_arg_t;

static bool is_hex_string(const char *s, size_t len)
{
    if (!s) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[len] == '\0';
}

static bool is_valid_sha256_hex(const char *s)
{
    return s && strlen(s) == 64 && is_hex_string(s, 64);
}

static bool parse_ipv4_host(const char *host, uint8_t out[4])
{
    if (!host || !host[0]) return false;
    const char *p = host;
    for (int part = 0; part < 4; ++part) {
        if (!isdigit((unsigned char)*p)) return false;
        int value = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            if (value > 255) return false;
            ++digits;
            ++p;
        }
        if (digits == 0) return false;
        out[part] = (uint8_t)value;
        if (part < 3) {
            if (*p != '.') return false;
            ++p;
        }
    }
    return *p == '\0';
}

static bool is_private_ipv4(const uint8_t ip[4])
{
    if (ip[0] == 10) return true;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
    if (ip[0] == 192 && ip[1] == 168) return true;
    return false;
}

static bool extract_http_host(const char *url, char *host, size_t host_size)
{
    const char *p = url + strlen("http://");
    const char *end = p;
    while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#') ++end;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= host_size) return false;
    memcpy(host, p, len);
    host[len] = '\0';
    return true;
}

static ota_url_policy_t validate_url_policy(const char *url)
{
    ota_url_policy_t policy = { .scheme = OTA_SCHEME_INVALID, .sha_required = false };
    if (!url || url[0] == '\0' || strlen(url) > OTA_URL_MAX_LEN) return policy;

    if (strncmp(url, "https://", 8) == 0) {
        policy.scheme = OTA_SCHEME_HTTPS;
        return policy;
    }

    if (strncmp(url, "http://", 7) == 0) {
        char host[64];
        uint8_t ip[4];
        if (!extract_http_host(url, host, sizeof(host))) return policy;
        if (!parse_ipv4_host(host, ip)) return policy;
        if (!is_private_ipv4(ip)) return policy;
        policy.scheme = OTA_SCHEME_LAN_HTTP;
        policy.sha_required = true;
        return policy;
    }

    return policy;
}

static void digest_to_hex(const uint8_t digest[32], char out[65])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool hex_equal_case_insensitive(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (int i = 0; i < 64; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return a[64] == '\0' && b[64] == '\0';
}

static void ota_task(void *arg)
{
    ota_task_arg_t *p = (ota_task_arg_t *)arg;
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *target = NULL;
    uint8_t *buf = NULL;
    bool ota_begun = false;
    bool ota_ended = false;

    esp_http_client_config_t http_cfg = {
        .url = p->url,
        .timeout_ms = 20000,
        .keep_alive_enable = false,
    };
    if (p->scheme == OTA_SCHEME_HTTPS) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_ota_progress_pct = 0;
    set_ota_state(VOICE_NET_OTA_STATE_DOWNLOADING);

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "http status=%d", status_code);
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no next OTA partition");
        set_ota_error("ota_validate_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        set_ota_error("ota_validate_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }
    ota_begun = true;

    buf = (uint8_t *)malloc(OTA_READ_BUF_SIZE);
    if (!buf) {
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int64_t written = 0;
    int last_reported_pct = -1;
    ESP_LOGI(TAG, "ota download started: url=%s status=%d content_length=%d partition=%s",
             p->url, status_code, content_length, target->label);

    while (true) {
        int read = esp_http_client_read(client, (char *)buf, OTA_READ_BUF_SIZE);
        if (read < 0) {
            ESP_LOGE(TAG, "http read failed: %d", read);
            set_ota_error("ota_http_failed");
            set_ota_state(VOICE_NET_OTA_STATE_FAILED);
            mbedtls_sha256_free(&sha);
            goto cleanup;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        err = esp_ota_write(ota_handle, buf, read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            set_ota_error("ota_validate_failed");
            set_ota_state(VOICE_NET_OTA_STATE_FAILED);
            mbedtls_sha256_free(&sha);
            goto cleanup;
        }

        mbedtls_sha256_update(&sha, buf, (size_t)read);
        written += read;

        int pct = (content_length > 0) ? (int)(written * 100 / content_length) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (pct - last_reported_pct >= OTA_PROGRESS_STEP_PCT) {
            s_ota_progress_pct = pct;
            voice_net_publish_status();
            last_reported_pct = pct;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t digest[32];
    char digest_hex[65];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    digest_to_hex(digest, digest_hex);

    set_ota_state(VOICE_NET_OTA_STATE_FINISHING);

    err = esp_ota_end(ota_handle);
    ota_ended = true;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        set_ota_error("ota_validate_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    if (p->has_sha256 && !hex_equal_case_insensitive(digest_hex, p->sha256_hex)) {
        ESP_LOGE(TAG, "sha256 mismatch expected=%s actual=%s", p->sha256_hex, digest_hex);
        set_ota_error("ota_sha256_mismatch");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        set_ota_error("ota_validate_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    s_ota_progress_pct = 100;
    set_ota_state(VOICE_NET_OTA_STATE_SUCCESS);
    ESP_LOGI(TAG, "ota success bytes=%lld sha256=%s, restarting in 1s",
             written, digest_hex);

    atomic_store(&s_ota_in_progress, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

cleanup:
    if (ota_begun && !ota_ended && ota_handle) {
        esp_ota_abort(ota_handle);
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(buf);
    atomic_store(&s_ota_in_progress, false);
    free(p);
    vTaskDelete(NULL);
}

void voice_net_start_ota_pull_internal(const char *url, const char *sha256_hex,
                                       voice_net_park_query_fn park_cb)
{
    // 清掉上次错误，让本轮校验能输出准确错误码。
    s_ota_last_error[0] = '\0';

    ota_url_policy_t policy = validate_url_policy(url);
    if (policy.scheme == OTA_SCHEME_INVALID) {
        ESP_LOGW(TAG, "ota_pull: invalid url=%s", url ? url : "<null>");
        set_ota_error("ota_url_invalid");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        return;
    }

    const bool has_sha = sha256_hex && sha256_hex[0] != '\0';
    if (policy.sha_required && !has_sha) {
        ESP_LOGW(TAG, "ota_pull: http url requires sha256_hex");
        set_ota_error("ota_url_invalid");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        return;
    }
    if (has_sha && !is_valid_sha256_hex(sha256_hex)) {
        ESP_LOGW(TAG, "ota_pull: invalid sha256_hex");
        set_ota_error("ota_sha256_invalid");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        return;
    }
    if (atomic_load(&s_ota_in_progress)) {
        ESP_LOGW(TAG, "ota_pull: already in progress");
        return;
    }
    if (park_cb && !park_cb()) {
        ESP_LOGW(TAG, "ota_pull: park not locked, refused");
        set_ota_error("ota_park_required");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        return;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    ota_task_arg_t *p = calloc(1, sizeof(*p));
    if (!p) {
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        return;
    }
    strncpy(p->url, url, sizeof(p->url) - 1);
    p->scheme = policy.scheme;
    p->has_sha256 = has_sha;
    if (has_sha) {
        strncpy(p->sha256_hex, sha256_hex, sizeof(p->sha256_hex) - 1);
    }

    atomic_store(&s_ota_in_progress, true);
    BaseType_t ok = xTaskCreate(ota_task, "voice_net_ota", OTA_TASK_STACK_SIZE, p,
                                OTA_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        atomic_store(&s_ota_in_progress, false);
        free(p);
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
    }
}

// ---- pending_verify 健康签到 ----
//
// 在 boot 早期由 voice_net_init 调一次：检测当前运行槽位是否处于
// PENDING_VERIFY 状态（新固件首次启动）。如果是，记下来等满足双条件
// （uptime ≥ N + BLE 至少连过一次）后调 esp_ota_mark_app_valid_cancel_rollback。
//
// 没开 rollback 配置时（CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE not set），
// esp_ota_get_state_partition 通常返回 ESP_OTA_IMG_VALID 或 UNDEFINED，
// 永远不会进 PENDING_VERIFY 分支——这些函数都是 no-op 安全的，可以
// 先于 sdkconfig 改动落地。

void voice_net_ota_detect_pending_verify(void)
{
    s_boot_uptime_anchor_us = esp_timer_get_time();

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGW(TAG, "get_state_partition failed");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        atomic_store(&s_pending_verify, true);
        ESP_LOGW(TAG, "pending_verify: running partition %s waiting for health confirmation",
                 running->label);
    } else {
        ESP_LOGI(TAG, "running partition %s state=%d (no pending verify)",
                 running->label, (int)state);
    }
}

bool voice_net_is_pending_verify(void)
{
    return atomic_load(&s_pending_verify);
}

void voice_net_notify_ble_connected(void)
{
    atomic_store(&s_ble_seen_connected, true);
    // 看是否满足自动签到条件
    if (atomic_load(&s_app_valid_marked)) return;
    if (!atomic_load(&s_pending_verify)) return;
    int64_t uptime_ms = (esp_timer_get_time() - s_boot_uptime_anchor_us) / 1000;
    if (uptime_ms >= MARK_VALID_MIN_UPTIME_MS) {
        voice_net_mark_app_valid();
    }
}

void voice_net_mark_app_valid(void)
{
    if (atomic_exchange(&s_app_valid_marked, true)) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        atomic_store(&s_pending_verify, false);
        ESP_LOGI(TAG, "mark_app_valid_cancel_rollback ok");
    } else if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "mark_valid no-op: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "mark_valid failed: %s", esp_err_to_name(err));
        atomic_store(&s_app_valid_marked, false);
    }
    voice_net_publish_status();
}
