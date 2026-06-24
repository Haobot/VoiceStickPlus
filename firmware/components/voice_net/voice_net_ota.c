// voice_net HTTPS OTA pull 实现。
//
// 设计：
// - 桌面端通过 BLE 下发 {"event":"ota_pull","url":"https://..."} 触发
//   esp_https_ota 拉取固件镜像并写入下一个 OTA 槽位。
// - Park gate 由 main.c 注入的 voice_net_park_query_fn 决定；未锁时拒绝
//   并把 last_error 置为 ota_park_required。
// - 启动一个独立的 ota_pull 任务跑下载循环（栈 8KB，独立于 voice_net_task
//   避免重活之间互相阻塞）；下载期间通过原子标志位 s_ota_in_progress
//   暴露给 main.c 的 Park gate 让录音不抢资源。
// - 进度上报：esp_https_ota_perform 每个 chunk 后比较 progress_pct，
//   每变化 5% 就触发一次 voice_net_publish_status 把整个快照推到桌面端。
// - sha256_hex 字段当前只记录到 ota_pull.url，不做校验；esp_ota_end 自带
//   bin 内的 SHA256 校验，已经能拦下损坏的镜像。pinning 留给后续接入。
// - 注意：本期 *不* 调 esp_ota_mark_app_valid_cancel_rollback——rollback
//   配置（CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE）也没开。下一轮 §9 一起做。

#include "voice_net_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_net_ota";

#define OTA_URL_MAX_LEN       256
#define OTA_TASK_STACK_SIZE   8192
#define OTA_TASK_PRIORITY     4         // 比 voice_net_task (5) 低，OTA 阻塞 perform 时不挡 BLE 协议
#define OTA_PROGRESS_STEP_PCT 5         // 进度每变化 5% 推一次 wifi_status

static atomic_bool s_ota_in_progress = ATOMIC_VAR_INIT(false);

// 上报字段，由 voice_net.c 的快照拼装函数读。
// 简单起见用原子标志位 + 小字符串（互斥保护见 voice_net.c 的 s_status_mutex）。
static voice_net_ota_state_t s_ota_state = VOICE_NET_OTA_STATE_IDLE;
static int                   s_ota_progress_pct = 0;
static char                  s_ota_url[OTA_URL_MAX_LEN + 1];
static char                  s_ota_last_error[24];

// 上报：调 voice_net_publish_status 让快照推到 BLE state_tx。
// 由 voice_net.c 实现，这里 forward declare。
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
    if (s_ota_last_error[0] != '\0') return;  // 首次写入保留
    strncpy(s_ota_last_error, code, sizeof(s_ota_last_error) - 1);
    s_ota_last_error[sizeof(s_ota_last_error) - 1] = '\0';
}

static void set_ota_state(voice_net_ota_state_t st)
{
    s_ota_state = st;
    voice_net_publish_status();
}

typedef struct {
    char url[OTA_URL_MAX_LEN + 1];
    char sha256_hex[65];                  // 暂未校验，记录占位
} ota_task_arg_t;

static void ota_task(void *arg)
{
    ota_task_arg_t *p = (ota_task_arg_t *)arg;

    // 准备 HTTP client config：阿里云 OSS 走标准 TLS，用 IDF 内置 cert bundle 即可。
    // 不开 keep-alive：单次下载就够，省内存。
    esp_http_client_config_t http_cfg = {
        .url = p->url,
        .timeout_ms = 20000,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .bulk_flash_erase = false,         // 按需擦除 OTA 槽位（chunk 写时擦），避免一次性 ~3MB erase 阻塞 ~5s
        .partial_http_download = true,
        .max_http_request_size = 16 * 1024,
    };

    s_ota_progress_pct = 0;
    set_ota_state(VOICE_NET_OTA_STATE_DOWNLOADING);

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    // 拿到 http header 后可以获取镜像描述；这里不做强约束（避免 size==0 时直接 fail）。
    int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "ota download started: url=%s expected_size=%d", p->url, image_size);

    int last_reported_pct = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = (image_size > 0) ? (int)((int64_t)read * 100 / image_size) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (pct - last_reported_pct >= OTA_PROGRESS_STEP_PCT) {
            s_ota_progress_pct = pct;
            voice_net_publish_status();
            last_reported_pct = pct;
        }
        // 让出 CPU 给 Wi-Fi / BLE：esp_https_ota_perform 内部已经会让出，但留一个保险。
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        esp_https_ota_abort(handle);
        goto cleanup;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "incomplete data received");
        set_ota_error("ota_http_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        esp_https_ota_abort(handle);
        goto cleanup;
    }

    set_ota_state(VOICE_NET_OTA_STATE_FINISHING);

    err = esp_https_ota_finish(handle);
    handle = NULL;                          // finish 后 handle 不再有效
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        // sha256 / app_desc 校验失败也走这里；用 validate_failed 更精确。
        set_ota_error("ota_validate_failed");
        set_ota_state(VOICE_NET_OTA_STATE_FAILED);
        goto cleanup;
    }

    s_ota_progress_pct = 100;
    set_ota_state(VOICE_NET_OTA_STATE_SUCCESS);
    ESP_LOGI(TAG, "ota success, restarting in 1s");

    // 让 BLE state_tx 把 success 帧先发出去，再重启
    atomic_store(&s_ota_in_progress, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

cleanup:
    atomic_store(&s_ota_in_progress, false);
    free(p);
    vTaskDelete(NULL);
}

void voice_net_start_ota_pull_internal(const char *url, const char *sha256_hex,
                                       voice_net_park_query_fn park_cb)
{
    if (!url || url[0] == '\0') {
        ESP_LOGW(TAG, "ota_pull: empty url");
        set_ota_error("ota_url_invalid");
        voice_net_publish_status();
        return;
    }
    if (strlen(url) > OTA_URL_MAX_LEN) {
        ESP_LOGW(TAG, "ota_pull: url too long");
        set_ota_error("ota_url_invalid");
        voice_net_publish_status();
        return;
    }
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "ota_pull: not https");
        set_ota_error("ota_url_invalid");
        voice_net_publish_status();
        return;
    }
    if (atomic_load(&s_ota_in_progress)) {
        ESP_LOGW(TAG, "ota_pull: already in progress");
        return;                              // 不报错，幂等
    }
    if (park_cb && !park_cb()) {
        ESP_LOGW(TAG, "ota_pull: park not locked, refused");
        set_ota_error("ota_park_required");
        voice_net_publish_status();
        return;
    }

    // 清掉上次的错误码（新的一轮尝试）
    s_ota_last_error[0] = '\0';
    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    ota_task_arg_t *p = calloc(1, sizeof(*p));
    if (!p) {
        set_ota_error("ota_http_failed");
        voice_net_publish_status();
        return;
    }
    strncpy(p->url, url, sizeof(p->url) - 1);
    if (sha256_hex) {
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
        voice_net_publish_status();
    }
}
