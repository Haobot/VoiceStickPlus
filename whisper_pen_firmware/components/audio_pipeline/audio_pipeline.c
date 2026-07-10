// audio_pipeline.c -- I2S 采集 + Opus 编码 + HPF + drain + 双核任务
//
// 修正点落地（对照评估结论）：
//   - complexity 5->1（评估第1点）：ESP32-S3 VOIP 用1，高complexity收益不抵 PSRAM cache miss。
//   - 24bit->16bit 用硬件 data_bit_width=16（评估第2点，源码已证实）：
//     ICS-41351 输出 24-bit 左对齐于 32-bit slot。配 slot_bit_width=32(总宽) +
//     data_bit_width=16(有效宽)，I2S 硬件从 32-bit slot 取高16位存入 DMA buffer，
//     read 出来直接是 int16_t[]，零软件右移。方案的"右移8"对左对齐数据是错的。
//   - 按需启停（评估第3点）：session 间 init/deinit，待机零功耗，与 <15uA deep sleep 兼容。
//   - 软件 HPF 90Hz（评估第7点）：去低语爆破音，比 LSB_DEPTH 关键。
//   - drain 尾音（评估第8点）：松开读 2 帧(80ms) 覆盖 60ms DMA 残留，防丢最后1-2字。
//
// 证据（ESP-IDF v5.5.1 esp_driver_i2s 源码）：
//   i2s_std.c:118   buf_size = i2s_get_buf_size(handle, slot_cfg->data_bit_width, ...)
//   i2s_common.c:423  bytes_per_sample = (data_bit_width + 7) / 8   // ESP32-S3
//   i2s_common.c:1378  memcpy(dest_byte, data_ptr, bytes_can_read)  // 无位宽转换
//   i2s_std.h:230-231  data_bit_width=valid bits, slot_bit_width=total bits per slot

#include "audio_pipeline.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "opus.h"

#include "voice_ble.h"

static const char *TAG = "audio";

#define OPUS_BITRATE        32000
#define OPUS_MAX_PACKET     220
#define OPUS_COMPLEXITY     1          // 修正：方案5偏激进，ESP32-S3 VOIP 用1（评估第1点）
#define TX_QUEUE_DEPTH      50
#define WARMUP_DROP_FRAMES  1          // 丢前1帧(40ms)等 MEMS 稳定 + DMA 预填充（评估第3点语境）

// 二阶 Butterworth HPF fc≈90Hz@16kHz（评估第7点）。
// 系数 Voice Stick 已验证频响：90Hz -3dB / 50Hz -10.6dB / 300Hz+ ~0dB。
static const double kHpfB0 = 0.975318;
static const double kHpfB1 = -1.950637;
static const double kHpfB2 = 0.975318;
static const double kHpfA1 = -1.950028;
static const double kHpfA2 = 0.951246;
static double s_hpf_z1, s_hpf_z2;
static inline int16_t hpf_process(int16_t x) {
    double in = (double)x;
    double y = kHpfB0 * in + s_hpf_z1;
    s_hpf_z1 = kHpfB1 * in - kHpfA1 * y + s_hpf_z2;
    s_hpf_z2 = kHpfB2 * in - kHpfA2 * y;
    if (y > 32767.0) y = 32767.0;
    else if (y < -32768.0) y = -32768.0;
    return (int16_t)lround(y);
}

typedef struct {
    uint32_t session_id;
    uint32_t seq;
    uint16_t len;
    uint8_t  flags;
    uint8_t  data[OPUS_MAX_PACKET];
} audio_packet_t;

static atomic_bool s_running;
static bool s_init;
static QueueHandle_t s_tx_queue;
static i2s_chan_handle_t s_rx;
static OpusEncoder *s_enc;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_tx_task;
static int64_t s_start_us;

// ─── I2S 初始化 ─────────────────────────────────────────────
static esp_err_t init_i2s(void) {
    // 【修正·核心】24->16 用硬件，删方案"软件右移8"（评估第2点，源码已证实）。
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // DMA 描述符走内部 SRAM（驱动默认），严禁 PSRAM：cache 一致性致爆音。
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 120;       // 4×120≈60ms 缓冲，drain 依据
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx), TAG, "new channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AP_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            // ICS-41351 数字 MEMS：只需 BCLK/WS/DIN，无 MCLK（用 BCLK 自运行）。
            .bclk = AP_PIN_BCLK,
            .ws   = AP_PIN_WS,
            .din  = AP_PIN_DIN,
            .mclk = -1,            // MEMS 无需 MCLK
        },
    };
    // slot_bit_width=32BIT(总宽) + data_bit_width=16(有效宽)：硬件从 32-bit slot 取高16位。
    // ICS-41351 输出 24-bit 左对齐，取高16位=有效 MSB 部分，丢低8位精度（对16-bit输出无损）。
    // MONO 采左声道(LEFT)；若 ICS-41351 L/R 引脚配成输出右声道，改 slot_mask 为 RIGHT。
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "init std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable rx");
    return ESP_OK;
}

static void deinit_i2s(void) {
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

// ─── Opus 初始化 ────────────────────────────────────────────
static esp_err_t init_opus(void) {
    int error = 0;
    // encoder 实例随 CONFIG_SPIRAM_USE_MALLOC 倾向 PSRAM（方案"encoder 在 PSRAM"的落地）。
    // 不手动 heap_caps 包裹：opus 内部分散分配，手动指定 caps 会破坏其内部布局。
    s_enc = opus_encoder_create(AP_SAMPLE_RATE, AP_CHANNELS, OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_enc != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "opus create error=%d", error);
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(0));             // CBR，低延迟稳定
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));             // 关不连续传输
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // LSB_DEPTH(16)：诚实标注输入位深，影响 dither/noise shaping 决策。
    //   方案把它当"低语SNR关键"是理解偏差（评估第6点）：不改变动态范围也不增益。
    //   低语SNR 靠模拟增益+AGC（MEMS 无 codec 层，靠固件 ALC 后续扩展）。
    opus_encoder_ctl(s_enc, OPUS_SET_LSB_DEPTH(16));
    return ESP_OK;
}

static void deinit_opus(void) {
    if (s_enc) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
}

// ─── Core 1：采集 + 编码 ───────────────────────────────────
static void audio_task(void *arg) {
    (void)arg;
    int16_t frame[AP_FRAME_SAMPLES];       // read 出来直接是 int16_t[]（硬件已取16位）
    uint8_t obuf[OPUS_MAX_PACKET];
    uint32_t warmup = WARMUP_DROP_FRAMES;
    uint32_t enqueued = 0;

    while (atomic_load(&s_running)) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, frame, sizeof(frame), &got, portMAX_DELAY) != ESP_OK) {
            ESP_LOGW(TAG, "i2s read failed");
            continue;
        }
        if (warmup > 0) { warmup--; continue; }   // 丢 warm-up 帧，等 MEMS 稳定

        for (int i = 0; i < AP_FRAME_SAMPLES; ++i) frame[i] = hpf_process(frame[i]);

        opus_int32 n = opus_encode(s_enc, frame, AP_FRAME_SAMPLES, obuf, sizeof(obuf));
        if (n < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)n);
            continue;
        }

        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = (s_seq == 0) ? VOICE_BLE_FLAG_START : 0x00,
            .len = (uint16_t)n,
        };
        memcpy(pkt.data, obuf, n);

        if (xQueueSend(s_tx_queue, &pkt, 0) != pdTRUE) {
            // 队列满：丢最旧帧腾位，保实时性
            audio_packet_t discard;
            xQueueReceive(s_tx_queue, &discard, 0);
            xQueueSend(s_tx_queue, &pkt, 0);
        }
        s_seq++;
        enqueued++;
        if (enqueued == 1) {
            ESP_LOGI(TAG, "first frame %lldus after start",
                     (esp_timer_get_time() - s_start_us));
        }
    }

    // drain（评估第8点）：松开时 DMA 缓冲(60ms)里仍有尾音 PCM，
    // 不读出编码发出会丢最后1-2字。固定读2帧(80ms)覆盖60ms残留+余量。
    for (int d = 0; d < 2; ++d) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, frame, sizeof(frame), &got, 0) != ESP_OK) break;
        for (int i = 0; i < AP_FRAME_SAMPLES; ++i) frame[i] = hpf_process(frame[i]);
        opus_int32 n = opus_encode(s_enc, frame, AP_FRAME_SAMPLES, obuf, sizeof(obuf));
        if (n < 0) break;
        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .len = (uint16_t)n,
        };
        memcpy(pkt.data, obuf, n);
        xQueueSend(s_tx_queue, &pkt, 0);
        s_seq++;
    }

    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32, enqueued);
    s_audio_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// ─── Core 0：BLE 发送 + drain + 资源释放 ────────────────────
static void tx_task(void *arg) {
    (void)arg;
    audio_packet_t pkt;
    while (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) == pdTRUE) {
        if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) break;   // sentinel
        voice_ble_send_audio(pkt.session_id, pkt.seq, pkt.flags, pkt.data, pkt.len);
    }
    // 发 audio_end 标记，通知主机 session 结束
    voice_ble_send_audio(s_session_id, s_seq, VOICE_BLE_FLAG_END, NULL, 0);

    // 等 audio_task 完成 drain
    while (s_audio_task != NULL) vTaskDelay(pdMS_TO_TICKS(10));

    // 按需启停（评估第3点）：session 间释放 I2S/Opus，待机零功耗
    deinit_opus();
    deinit_i2s();
    ESP_LOGI(TAG, "session %" PRIu32 " drained & released", s_session_id);
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

// ─── 公共接口 ──────────────────────────────────────────────
esp_err_t audio_pipeline_init(void) {
    if (s_init) return ESP_OK;
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create tx queue");
    s_init = true;
    ESP_LOGI(TAG, "audio pipeline ready (session resources allocated on demand)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(uint32_t session_id) {
    if (atomic_load(&s_running)) return ESP_OK;
    s_start_us = esp_timer_get_time();

    esp_err_t err = init_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init: %s", esp_err_to_name(err));
        return err;
    }
    err = init_opus();
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    voice_ble_request_fast_interval();   // 录音期强制 7.5ms 快 interval
    s_hpf_z1 = 0.0;
    s_hpf_z2 = 0.0;
    opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
    s_session_id = session_id;
    s_seq = 0;
    xQueueReset(s_tx_queue);
    atomic_store(&s_running, true);

    // tx_task pin Core 0：BLE 协议栈在 Core 0，notify 就近发送
    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096,
                                            NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        deinit_opus();
        deinit_i2s();
        return ESP_ERR_NO_MEM;
    }
    // audio_task pin Core 1 + 栈放 PSRAM（WithCaps）：
    //   内部 RAM 留给 DMA/BLE，大栈走 PSRAM 不挤占。
    //   只在非 cache 禁用期运行（OTA 与录音互斥），PSRAM 栈安全。
    ok = xTaskCreatePinnedToCoreWithCaps(audio_task, "audio_pipeline", 24576,
                                         NULL, 5, &s_audio_task, 1, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create audio task failed, free_internal=%u free_spiram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        atomic_store(&s_running, false);
        audio_packet_t sentinel = { .flags = VOICE_BLE_FLAG_END, .len = 0 };
        xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
        // tx_task 退出时会清理资源
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "start session %" PRIu32, session_id);
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void) {
    if (!atomic_load(&s_running)) return ESP_OK;
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "stop session %" PRIu32, s_session_id);

    audio_packet_t sentinel = { .flags = VOICE_BLE_FLAG_END, .len = 0 };
    xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);

    // 同步等 drain 完成（评估第8点）：否则松开的 button_up 会抢在 drain 帧/audio_end
    // 前到主机、提前结束会话丢尾音。超时兜底避免极端积压卡死。
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (s_tx_task != NULL && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_tx_task != NULL) {
        ESP_LOGW(TAG, "drain timeout, tx_task still running");
    }
    return ESP_OK;
}
