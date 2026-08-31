#include "power_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stick_s3_board.h"

static const char *TAG = "power_log";

// 挂载点与 audio_pipeline 的测试回放共用同一 storage 分区（后者懒挂载，
// 对本组件先挂载导致的 ESP_ERR_INVALID_STATE 已做兼容）。逻辑流对外即 /storage/power_log.bin。
#define POWER_LOG_BASE_PATH "/spiffs"
#define POWER_LOG_FILE_PATH "/spiffs/power_log.bin"

#define STREAM_HDR_SIZE 16
#define ENTRY_SIZE 12
#define FILE_HDR_SIZE 32
#define FILE_MAX_BYTES (256 * 1024)
#define FILE_CAPACITY ((FILE_MAX_BYTES - FILE_HDR_SIZE) / ENTRY_SIZE)
#define RAM_CAPACITY 64

#define SAMPLE_PERIOD_US (60ULL * 1000000ULL)    // 60s 周期 VBAT 采样
#define FLUSH_PERIOD_US (600ULL * 1000000ULL)    // 10 分钟 flush

#define FLAG_CHARGING 0x01
#define FLAG_USB_POWERED 0x02
#define FLAG_PERIODIC 0x04
#define FLAG_SHUTDOWN_SPAN 0x08
#define FLAG_TIME_ANCHOR 0x10

#define ENTRY_MODE_TIME_ANCHOR 0xFF
#define ENTRY_MODE_UNKNOWN 0xFE

// M5PM1 RTC RAM（0xA0 起）关机锚点：魔数 + uint32 uptime_s + uint16 vbat_mv + CRC16。
#define RTC_ANCHOR_OFFSET 0
#define RTC_ANCHOR_SIZE 12
#define RTC_ANCHOR_MAGIC "PWRA"

#define NOTIFY_SAMPLE (1u << 0)
#define NOTIFY_FLUSH (1u << 1)

typedef struct __attribute__((packed)) {
    uint32_t uptime_s;
    uint16_t vbat_mv;
    uint8_t mode;
    uint8_t flags;
    uint8_t reserved[4];
} power_log_entry_t;
_Static_assert(sizeof(power_log_entry_t) == ENTRY_SIZE, "entry must be 12 bytes");

// SPIFFS 环形文件头（区别于逻辑流头）：记录写位置与有效条目数，掉电后据此恢复。
typedef struct __attribute__((packed)) {
    char magic[4];          // "PWLF"
    uint8_t version;        // 1
    uint8_t reserved[3];
    uint32_t write_index;   // 单调递增的总写入条目数（取模得槽位）
    uint32_t count;         // 当前有效条目数（<= FILE_CAPACITY）
    uint32_t wrap_count;    // 回卷/清空计数，与逻辑流头一致
    uint8_t reserved2[12];
} power_log_file_hdr_t;
_Static_assert(sizeof(power_log_file_hdr_t) == FILE_HDR_SIZE, "file hdr must be 32 bytes");

static bool s_initialized;
static bool s_fs_ok;
static bool s_flush_deferred;               // 录音/OTA 期间延迟 flush
static bool s_ram_overflow_warned;
static power_mode_t s_current_mode = POWER_MODE_COUNT;
static power_log_entry_t s_ram[RAM_CAPACITY];
static size_t s_ram_count;
static uint32_t s_file_write_index;
static uint32_t s_file_count;
static uint32_t s_wrap_count;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static esp_timer_handle_t s_sample_timer;
static esp_timer_handle_t s_flush_timer;

static uint32_t uptime_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static uint16_t vbat_now_mv(void)
{
    int mv = 0;
    if (stick_s3_board_battery_voltage_mv(&mv) != ESP_OK || mv <= 0 || mv > 0xFFFF) {
        return 0;
    }
    return (uint16_t)mv;
}

static uint8_t power_flags_now(void)
{
    uint8_t flags = 0;
    bool state = false;
    if (stick_s3_board_battery_charging(&state) == ESP_OK && state) {
        flags |= FLAG_CHARGING;
    }
    if (stick_s3_board_usb_powered(&state) == ESP_OK && state) {
        flags |= FLAG_USB_POWERED;
    }
    return flags;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// 以下 file_* 与 flush_locked 调用时必须已持有 s_lock。

static void file_write_hdr_locked(FILE *fp)
{
    power_log_file_hdr_t hdr = {0};
    memcpy(hdr.magic, "PWLF", 4);
    hdr.version = 1;
    hdr.write_index = s_file_write_index;
    hdr.count = s_file_count;
    hdr.wrap_count = s_wrap_count;
    fseek(fp, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, fp);
}

static void file_create_locked(void)
{
    FILE *fp = fopen(POWER_LOG_FILE_PATH, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "create %s failed", POWER_LOG_FILE_PATH);
        s_fs_ok = false;
        return;
    }
    file_write_hdr_locked(fp);
    fclose(fp);
}

static void file_load_locked(void)
{
    s_file_write_index = 0;
    s_file_count = 0;
    s_wrap_count = 0;

    FILE *fp = fopen(POWER_LOG_FILE_PATH, "rb");
    if (!fp) {
        file_create_locked();
        return;
    }
    power_log_file_hdr_t hdr = {0};
    const size_t got = fread(&hdr, 1, sizeof(hdr), fp);
    fclose(fp);

    const bool valid = got == sizeof(hdr) && memcmp(hdr.magic, "PWLF", 4) == 0 &&
                       hdr.version == 1 && hdr.count <= FILE_CAPACITY &&
                       hdr.write_index >= hdr.count;
    if (!valid) {
        ESP_LOGW(TAG, "power_log.bin header invalid, starting fresh");
        file_create_locked();
        return;
    }
    s_file_write_index = hdr.write_index;
    s_file_count = hdr.count;
    s_wrap_count = hdr.wrap_count;
    ESP_LOGI(TAG, "loaded log file: count=%" PRIu32 " wrap=%" PRIu32,
             s_file_count, s_wrap_count);
}

// 把 RAM 缓冲追加进环形文件。只写新增条目段与文件头（flash 磨损最小化）。
// 写满 FILE_CAPACITY 后回卷覆盖最旧条目，回卷计数递增。
static void flush_locked(bool force)
{
    if (!s_fs_ok || s_ram_count == 0) {
        return;
    }
    if (s_flush_deferred && !force) {
        return;
    }

    FILE *fp = fopen(POWER_LOG_FILE_PATH, "r+b");
    if (!fp) {
        file_create_locked();
        fp = fopen(POWER_LOG_FILE_PATH, "r+b");
        if (!fp) {
            ESP_LOGE(TAG, "flush open failed, %u entries kept in RAM", (unsigned)s_ram_count);
            return;
        }
    }

    bool wrapped = false;
    for (size_t i = 0; i < s_ram_count; ++i) {
        if (s_file_count >= FILE_CAPACITY) {
            wrapped = true;     // 本 flush 起开始覆盖最旧条目
        }
        const uint32_t slot = s_file_write_index % FILE_CAPACITY;
        fseek(fp, (long)(FILE_HDR_SIZE + slot * ENTRY_SIZE), SEEK_SET);
        fwrite(&s_ram[i], ENTRY_SIZE, 1, fp);
        ++s_file_write_index;
        if (s_file_count < FILE_CAPACITY) {
            ++s_file_count;
        }
    }
    if (wrapped) {
        ++s_wrap_count;
        ESP_LOGI(TAG, "log file wrapped, overwriting oldest (wrap=%" PRIu32 ")", s_wrap_count);
    }
    file_write_hdr_locked(fp);
    fclose(fp);
    ESP_LOGD(TAG, "flushed %u entries (total=%" PRIu32 ")", (unsigned)s_ram_count, s_file_count);
    s_ram_count = 0;
}

static void append_locked(const power_log_entry_t *entry)
{
    if (s_ram_count >= RAM_CAPACITY) {
        // 不在调用方上下文（可能是 esp_timer 回调）同步写 SPIFFS：先请求异步
        // flush，缓冲仍满才丢最旧，不阻塞调用方。
        if (!s_flush_deferred && s_task) {
            xTaskNotify(s_task, NOTIFY_FLUSH, eSetBits);
        }
        if (s_ram_count >= RAM_CAPACITY) {
            memmove(&s_ram[0], &s_ram[1], sizeof(s_ram[0]) * (RAM_CAPACITY - 1));
            --s_ram_count;
            if (!s_ram_overflow_warned) {
                s_ram_overflow_warned = true;
                ESP_LOGW(TAG, "RAM buffer full, dropping oldest entries");
            }
        }
    }
    s_ram[s_ram_count++] = *entry;
}

static void rtc_anchor_write_locked(void)
{
    uint8_t buf[RTC_ANCHOR_SIZE] = {0};
    memcpy(buf, RTC_ANCHOR_MAGIC, 4);
    const uint32_t uptime = uptime_now_s();
    const uint16_t vbat = vbat_now_mv();
    memcpy(buf + 4, &uptime, sizeof(uptime));
    memcpy(buf + 8, &vbat, sizeof(vbat));
    const uint16_t crc = crc16_ccitt(buf, 10);
    memcpy(buf + 10, &crc, sizeof(crc));

    esp_err_t err = stick_s3_board_pmic_rtc_ram_write(RTC_ANCHOR_OFFSET, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC RAM anchor write failed: %s", esp_err_to_name(err));
    }
}

// 冷启动恢复关机段：校验魔数+CRC，补一条 flags.bit3 记录；随后失效化锚点避免重复恢复。
static void rtc_anchor_recover_locked(void)
{
    uint8_t buf[RTC_ANCHOR_SIZE] = {0};
    if (stick_s3_board_pmic_rtc_ram_read(RTC_ANCHOR_OFFSET, buf, sizeof(buf)) != ESP_OK) {
        return;
    }
    const uint16_t crc = crc16_ccitt(buf, 10);
    uint16_t stored_crc = 0;
    memcpy(&stored_crc, buf + 10, sizeof(stored_crc));
    if (memcmp(buf, RTC_ANCHOR_MAGIC, 4) != 0 || crc != stored_crc) {
        return;
    }

    uint32_t shutdown_uptime = 0;
    uint16_t shutdown_vbat = 0;
    memcpy(&shutdown_uptime, buf + 4, sizeof(shutdown_uptime));
    memcpy(&shutdown_vbat, buf + 8, sizeof(shutdown_vbat));

    const power_log_entry_t entry = {
        .uptime_s = shutdown_uptime,    // 上次会话的关机时刻 uptime，供分析配对
        .vbat_mv = shutdown_vbat,
        .mode = POWER_MODE_S3_POWER_OFF,
        .flags = FLAG_SHUTDOWN_SPAN | power_flags_now(),
        .reserved = {0},
    };
    append_locked(&entry);
    ESP_LOGI(TAG, "recovered shutdown span anchor: uptime=%" PRIu32 " vbat=%u",
             shutdown_uptime, shutdown_vbat);

    uint8_t zeros[4] = {0};
    stick_s3_board_pmic_rtc_ram_write(RTC_ANCHOR_OFFSET, zeros, sizeof(zeros));
}

static void do_sample(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const power_log_entry_t entry = {
        .uptime_s = uptime_now_s(),
        .vbat_mv = vbat_now_mv(),
        .mode = s_current_mode < POWER_MODE_COUNT ? (uint8_t)s_current_mode : ENTRY_MODE_UNKNOWN,
        .flags = FLAG_PERIODIC | power_flags_now(),
        .reserved = {0},
    };
    append_locked(&entry);
    xSemaphoreGive(s_lock);
}

static void power_log_task(void *arg)
{
    uint32_t bits = 0;
    for (;;) {
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (bits & NOTIFY_SAMPLE) {
            do_sample();
        }
        if (bits & NOTIFY_FLUSH) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            flush_locked(false);
            xSemaphoreGive(s_lock);
        }
    }
}

static void sample_timer_cb(void *arg)
{
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_SAMPLE, eSetBits);
    }
}

static void flush_timer_cb(void *arg)
{
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_FLUSH, eSetBits);
    }
}

esp_err_t power_log_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = POWER_LOG_BASE_PATH,
        .partition_label = "storage",
        .max_files = 4,
        // 挂载失败（如分区内容非 SPIFFS：SPIFFS_ERR_NOT_A_FS）时自动格式化
        // 重挂：功耗遥测可自愈，避免永久退化为 64 条 RAM 环形区导致导出
        // 数据 10 分钟写满。true。
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE：分区已被挂载（如测试回放先挂载），直接复用。
        s_fs_ok = true;
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s, RAM-only logging", esp_err_to_name(err));
        s_fs_ok = false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_fs_ok) {
        file_load_locked();
    }
    rtc_anchor_recover_locked();
    xSemaphoreGive(s_lock);

    if (xTaskCreate(power_log_task, "power_log", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t sample_args = {
        .callback = sample_timer_cb,
        .name = "power_log_sample",
    };
    const esp_timer_create_args_t flush_args = {
        .callback = flush_timer_cb,
        .name = "power_log_flush",
    };
    if (esp_timer_create(&sample_args, &s_sample_timer) != ESP_OK ||
        esp_timer_create(&flush_args, &s_flush_timer) != ESP_OK ||
        esp_timer_start_periodic(s_sample_timer, SAMPLE_PERIOD_US) != ESP_OK ||
        esp_timer_start_periodic(s_flush_timer, FLUSH_PERIOD_US) != ESP_OK) {
        ESP_LOGE(TAG, "timer setup failed");
        vTaskDelete(s_task);
        s_task = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "power_log started (fs=%d, ram=%d entries, file cap=%u entries)",
             s_fs_ok, RAM_CAPACITY, (unsigned)FILE_CAPACITY);
    return ESP_OK;
}

void power_log_note_mode(power_mode_t mode)
{
    if (!s_initialized || mode >= POWER_MODE_COUNT) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mode == s_current_mode) {
        xSemaphoreGive(s_lock);
        return;
    }

    const bool was_deferred = s_flush_deferred;
    const power_log_entry_t entry = {
        .uptime_s = uptime_now_s(),
        .vbat_mv = vbat_now_mv(),
        .mode = (uint8_t)mode,
        .flags = power_flags_now(),
        .reserved = {0},
    };
    append_locked(&entry);
    s_current_mode = mode;
    s_flush_deferred = (mode == POWER_MODE_RECORDING || mode == POWER_MODE_OTA);

    if (mode == POWER_MODE_S3_POWER_OFF) {
        // 进 S3 前：写 RTC RAM 关机锚点并强制 flush，确保关机段可恢复。
        // 该路径由 enter_power_off() 在 app 事件任务上下文调用（非 esp_timer 回调），
        // 且即将断电必须同步落盘，是唯一的同步 flush 点。
        rtc_anchor_write_locked();
        flush_locked(true);
    } else if (was_deferred && !s_flush_deferred && s_task) {
        // 录音/OTA 会话结束，补 flush 延迟期间积累的条目。本函数可能运行于
        // esp_timer 回调上下文（S0→S1/S1→S2 转移），SPIFFS 写一律异步到
        // power_log_task 执行，避免阻塞整个 esp_timer 任务。
        xTaskNotify(s_task, NOTIFY_FLUSH, eSetBits);
    }
    xSemaphoreGive(s_lock);
}

void power_log_set_time_anchor(uint32_t epoch_s)
{
    if (!s_initialized) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    power_log_entry_t entry = {
        .uptime_s = uptime_now_s(),
        .vbat_mv = vbat_now_mv(),
        .mode = ENTRY_MODE_TIME_ANCHOR,
        .flags = FLAG_TIME_ANCHOR | power_flags_now(),
        .reserved = {0},
    };
    memcpy(entry.reserved, &epoch_s, sizeof(epoch_s));
    append_locked(&entry);
    xSemaphoreGive(s_lock);
}

size_t power_log_size(void)
{
    if (!s_initialized) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t total = STREAM_HDR_SIZE + (s_file_count + s_ram_count) * ENTRY_SIZE;
    xSemaphoreGive(s_lock);
    return total;
}

static void stream_header_locked(uint8_t out[STREAM_HDR_SIZE])
{
    out[0] = 'P';
    out[1] = 'W';
    out[2] = 'R';
    out[3] = 'L';
    out[4] = 1;             // 版本
    out[5] = ENTRY_SIZE;
    out[6] = 0;
    out[7] = 0;
    const uint32_t count = s_file_count + (uint32_t)s_ram_count;
    memcpy(out + 8, &count, sizeof(count));
    memcpy(out + 12, &s_wrap_count, sizeof(s_wrap_count));
}

size_t power_log_read(size_t offset, uint8_t *buf, size_t max)
{
    if (!s_initialized || !buf) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t total = STREAM_HDR_SIZE + (s_file_count + s_ram_count) * ENTRY_SIZE;
    if (offset >= total) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    size_t want = total - offset;
    if (want > max) {
        want = max;
    }

    size_t done = 0;
    FILE *fp = NULL;
    while (done < want) {
        const size_t pos = offset + done;
        if (pos < STREAM_HDR_SIZE) {
            uint8_t hdr[STREAM_HDR_SIZE];
            stream_header_locked(hdr);
            size_t chunk = STREAM_HDR_SIZE - pos;
            if (chunk > want - done) {
                chunk = want - done;
            }
            memcpy(buf + done, hdr + pos, chunk);
            done += chunk;
            continue;
        }

        const uint32_t k = (uint32_t)((pos - STREAM_HDR_SIZE) / ENTRY_SIZE);
        const size_t byte_in_entry = (pos - STREAM_HDR_SIZE) % ENTRY_SIZE;
        power_log_entry_t entry = {0};
        if (k < s_file_count) {
            if (!fp) {
                fp = fopen(POWER_LOG_FILE_PATH, "rb");
                if (!fp) {
                    break;
                }
            }
            const uint32_t oldest = (s_file_write_index - s_file_count) % FILE_CAPACITY;
            const uint32_t slot = (oldest + k) % FILE_CAPACITY;
            fseek(fp, (long)(FILE_HDR_SIZE + slot * ENTRY_SIZE), SEEK_SET);
            if (fread(&entry, ENTRY_SIZE, 1, fp) != 1) {
                break;
            }
        } else {
            entry = s_ram[k - s_file_count];
        }

        size_t chunk = ENTRY_SIZE - byte_in_entry;
        if (chunk > want - done) {
            chunk = want - done;
        }
        memcpy(buf + done, (const uint8_t *)&entry + byte_in_entry, chunk);
        done += chunk;
    }
    if (fp) {
        fclose(fp);
    }
    xSemaphoreGive(s_lock);
    return done;
}

void power_log_clear(void)
{
    if (!s_initialized) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ram_count = 0;
    s_file_write_index = 0;
    s_file_count = 0;
    ++s_wrap_count;
    if (s_fs_ok) {
        file_create_locked();
    }
    ESP_LOGI(TAG, "log cleared (wrap=%" PRIu32 ")", s_wrap_count);
    xSemaphoreGive(s_lock);
}
