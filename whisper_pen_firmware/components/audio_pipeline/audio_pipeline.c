// audio_pipeline.c -- ES8311 采集 + Opus 编码 + HPF + drain + 双核任务
//
// 采集链路（复用 StickS3 硬件，移植自 firmware/ 已踩坑实现）：
//   - ES8311 codec 经 I2C 配寄存器（PGA/ALC），I2S STEREO slot 16-bit 采集
//   - esp_codec_dev 封装 codec 寄存器 + I2S 读写 + MCLK，替代裸 driver/i2s_std
//   - esp_codec_dev_read 读立体声 PCM，mono[i]=stereo[i*2] 取左声道
//
// 原评估第2点（ICS-41351 24->16 硬件取高16）已作废：
//   ES8311 直接配 16-bit 输出，无需 slot_bit_width=32 技巧。保留此说明以记录差异。
//
// 踩坑点（移植自 firmware，详见 memory）：
//   - I2S channel 必须在 esp_codec_dev_open 前 enable：open 会 reconfig(disable->init->enable)，
//     未 enable 则 reconfig 的 disable 报 "the channel has not been enabled yet"。
//   - 停止侧 esp_codec_dev_close 已 disable 两通道，deinit_i2s 只 del_channel 不 disable，
//     重复 disable 会报 ERROR。
//   - ES8311 ALC 位域以 Linux 主线 es8311.h 为准（曾按 es8311_reg.h 注释写反致 ALC 未生效）。
//   - PGA=18dB：原 36dB 近场削波致 ASR 变差，18dB 留 headroom，远场靠 ALC 拉起。
//
// 保留自脚手架：HPF 90Hz、Opus complexity 1、drain 2帧、按需启停、双核任务、PSRAM 栈。

#include "audio_pipeline.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "opus.h"

#include "stick_s3_board.h"
#include "voice_ble.h"

static const char *TAG = "audio";

#define OPUS_BITRATE        32000
#define OPUS_MAX_PACKET     220
#define OPUS_COMPLEXITY     1          // ESP32-S3 VOIP 用1，高complexity收益不抵 PSRAM cache miss
#define TX_QUEUE_DEPTH      50
#define TX_RETRY_DELAY_MS   10
#define TX_MAX_RETRIES      30
#define TX_DRAIN_TIMEOUT_MS 500
#define WARMUP_DROP_FRAMES  1          // 丢前1帧等 codec 稳定 + DMA 预填充
#define DIAG_LOG_FRAMES    25          // 每 25 帧(1s) 打印一次 PCM peak + Opus 帧大小

// 二阶 Butterworth HPF fc≈90Hz@16kHz，去近距离说话气流爆破音。
// 系数频响已验证：90Hz -3dB / 50Hz -10.6dB / 300Hz+ ~0dB（语音中高频无损）。
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

// I2S + ES8311 codec 句柄（session 间创建/释放）
static i2s_chan_handle_t s_rx_handle;
static i2s_chan_handle_t s_tx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;

static OpusEncoder *s_enc;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_tx_task;
static int64_t s_start_us;

// ─── I2S 初始化（移植自 firmware/audio_pipeline.c:114-155）──────────
static esp_err_t init_i2s(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 120;       // 4×120≈60ms 缓冲，drain 依据
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                        TAG, "create i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AP_SAMPLE_RATE),
        // STEREO slot：esp_codec_dev_read 读立体声，再取左声道转 mono。
        // ES8311 直接 16-bit 输出，无需原 ICS-41351 的 slot_bit_width=32 技巧（评估第2点已作废）。
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            // 引脚取自 stick_s3_board.h（StickS3 板级定义）。
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws   = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,   // codec 数据入（喇叭路径，本场景不用但需配）
            .din  = STICK_S3_PIN_ES8311_DOUT,  // codec 数据出（麦克风路径）
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "init i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "init i2s tx");
    // 必须 enable 再 open codec：esp_codec_dev_open 会 reconfig(disable->init->enable)，
    // 通道此刻未 enable 则 reconfig 的 disable 报 "not enabled" ERROR。
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx");
    return ESP_OK;
}

static void deinit_i2s(void) {
    // 只 del_channel 不 disable：esp_codec_dev_close 已 disable 两通道，
    // 重复 disable 会报 "the channel has not been enabled yet" ERROR。
    if (s_rx_handle) {
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
}

// ─── ES8311 codec 初始化（移植自 firmware/audio_pipeline.c:157-240）──
static esp_err_t init_codec(void) {
    i2c_master_bus_handle_t i2c_bus = stick_s3_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2s data");

    s_gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,                    // STEREO，read 后取左声道转 mono
        .channel_mask = 0,
        .sample_rate = AP_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");

    // PGA=18dB：原 36dB 近场削波致 ASR 变差，18dB 留 headroom，远场靠 ALC 拉起。
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 18.0) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set mic gain");

    // ES8311 硬件 ALC：位域以 Linux 主线 es8311.h 为准（曾按 es8311_reg.h 注释写反致未生效）。
    // REG18: bit7=ALC_EN, bit[3:0]=WINSIZE(3 短响应)；REG19: bit[7:4]=MAXLEVEL(8~-11dBFS);
    // REG1A: bit[7:4]=MINLEVEL(0~-30dBFS)，bit[6]=AUTOMUTE(0 关)。不开 automute 防误判静音。
    esp_codec_dev_write_reg(s_codec, 0x18, 0x83);
    esp_codec_dev_write_reg(s_codec, 0x19, 0x80);
    esp_codec_dev_write_reg(s_codec, 0x1A, 0x00);
    return ESP_OK;
}

static void deinit_codec(void) {
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) { audio_codec_delete_codec_if(s_codec_if); s_codec_if = NULL; }
    if (s_data_if)  { audio_codec_delete_data_if(s_data_if);  s_data_if = NULL; }
    if (s_gpio_if)  { audio_codec_delete_gpio_if(s_gpio_if);  s_gpio_if = NULL; }
    if (s_ctrl_if)  { audio_codec_delete_ctrl_if(s_ctrl_if);  s_ctrl_if = NULL; }
}

// ─── Opus 初始化（保留自脚手架）──────────────────────────────────────
static esp_err_t init_opus(void) {
    int error = 0;
    s_enc = opus_encoder_create(AP_SAMPLE_RATE, AP_CHANNELS, OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_enc != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "opus create error=%d", error);
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    // 不设 OPUS_SET_LSB_DEPTH：对齐 firmware（firmware 不设此参数，用默认 24）。
    // whisper_pen 曾设 16（评估第6点），但实测与 firmware 行为不一致，移除以对齐。
    return ESP_OK;
}

static void deinit_opus(void) {
    if (s_enc) { opus_encoder_destroy(s_enc); s_enc = NULL; }
}

// ─── Core 1：采集 + 编码 ──────────────────────────────────────────────
static void audio_task(void *arg) {
    (void)arg;
    int16_t stereo[AP_FRAME_SAMPLES * 2];   // esp_codec_dev_read 读立体声
    int16_t mono[AP_FRAME_SAMPLES];          // 取左声道转 mono
    uint8_t obuf[OPUS_MAX_PACKET];
    uint32_t warmup = WARMUP_DROP_FRAMES;
    uint32_t enqueued = 0;
    uint32_t overflow_drops = 0;   // 队列满丢帧计数（诊断 BLE 吞吐是否跟上）

    // 诊断：每 DIAG_LOG_FRAMES 帧(1s) 打印 PCM peak + Opus 平均帧大小
    uint32_t diag_cnt = 0;
    int16_t diag_peak = 0;
    uint32_t diag_opus_bytes = 0;
    uint32_t diag_opus_frames = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = esp_codec_dev_read(s_codec, stereo, sizeof(stereo));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read failed: %s", esp_err_to_name(err));
            continue;
        }
        if (warmup > 0) { warmup--; continue; }   // 丢 warm-up 帧，等 codec 稳定

        int16_t peak = 0;
        for (int i = 0; i < AP_FRAME_SAMPLES; ++i) {
            mono[i] = stereo[i * 2];              // 取左声道
            mono[i] = hpf_process(mono[i]);
            int16_t a = mono[i] < 0 ? (int16_t)(-(int)mono[i]) : mono[i];
            if (a > peak) peak = a;
        }

        opus_int32 n = opus_encode(s_enc, mono, AP_FRAME_SAMPLES, obuf, sizeof(obuf));
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
            audio_packet_t discard;            // 队列满丢最旧帧保实时性
            xQueueReceive(s_tx_queue, &discard, 0);
            xQueueSend(s_tx_queue, &pkt, 0);
            overflow_drops++;
            if (overflow_drops == 1 || (overflow_drops % 20) == 0) {
                ESP_LOGW(TAG, "tx queue overflow, dropped oldest (total=%" PRIu32 ")", overflow_drops);
            }
        }
        s_seq++;
        enqueued++;

        // 诊断累计
        diag_cnt++;
        if (peak > diag_peak) diag_peak = peak;
        diag_opus_bytes += (uint32_t)n;
        diag_opus_frames++;
        if (diag_cnt >= DIAG_LOG_FRAMES) {
            ESP_LOGI(TAG, "diag: pcm_peak=%d opus_avg=%ubytes (%u frames)",
                     diag_peak, diag_opus_bytes / diag_opus_frames, diag_opus_frames);
            diag_cnt = 0; diag_peak = 0; diag_opus_bytes = 0; diag_opus_frames = 0;
        }

        if (enqueued == 1) {
            ESP_LOGI(TAG, "first frame %lldus after start",
                     (esp_timer_get_time() - s_start_us));
        }
    }

    // drain：松开时 DMA 缓冲(60ms)里仍有尾音 PCM，不读出编码发出会丢最后1-2字。
    // 固定读2帧(80ms)覆盖60ms残留+余量。
    for (int d = 0; d < 2; ++d) {
        esp_err_t err = esp_codec_dev_read(s_codec, stereo, sizeof(stereo));
        if (err != ESP_OK) break;
        for (int i = 0; i < AP_FRAME_SAMPLES; ++i) {
            mono[i] = stereo[i * 2];
            mono[i] = hpf_process(mono[i]);
        }
        opus_int32 n = opus_encode(s_enc, mono, AP_FRAME_SAMPLES, obuf, sizeof(obuf));
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

    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32 " overflow_drops=%" PRIu32 " stack_hwm=%u",
             enqueued, overflow_drops, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_audio_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// ─── Core 0：BLE 发送 + drain + 资源释放 ──────────────────────────────
// 遇 sentinel(FLAG_END+len=0) 跳 drain 排空队列剩余帧（含 audio_task drain 帧），
// 再发 audio_end。若遇 sentinel 直接 break 会丢 drain 帧致尾音丢失（曾踩此坑）。
static void tx_task(void *arg) {
    (void)arg;
    audio_packet_t pkt;
    uint32_t sent = 0;
    uint32_t tx_dropped = 0;   // BLE 发送失败丢弃计数

    while (true) {
        if (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) != pdTRUE) continue;

        // Sentinel：END flag 无 payload，触发 drain 模式
        if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
            goto drain;
        }

        int retries = 0;
        while (true) {
            esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                 pkt.flags, pkt.data, pkt.len);
            if (err == ESP_OK) { sent++; break; }
            if (++retries >= TX_MAX_RETRIES) {
                tx_dropped++;
                if (tx_dropped == 1 || (tx_dropped % 20) == 0) {
                    ESP_LOGW(TAG, "tx send failed after %d retries seq=%" PRIu32
                             " (total_dropped=%" PRIu32 ")",
                             TX_MAX_RETRIES, pkt.seq, tx_dropped);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
        }
    }

drain:
    // 排空队列剩余帧（audio_task 的 drain 帧排在此），超时兜底防极端积压卡死
    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_DRAIN_TIMEOUT_MS);
        while (xQueueReceive(s_tx_queue, &pkt, 0) == pdTRUE) {
            if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) break;
            if (xTaskGetTickCount() >= deadline) {
                ESP_LOGW(TAG, "drain timeout, discarding %u packets",
                         (unsigned)uxQueueMessagesWaiting(s_tx_queue));
                xQueueReset(s_tx_queue);
                break;
            }
            int retries = 0;
            while (true) {
                esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                     pkt.flags, pkt.data, pkt.len);
                if (err == ESP_OK) { sent++; break; }
                if (++retries >= TX_MAX_RETRIES || xTaskGetTickCount() >= deadline) break;
                vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
            }
        }

        // 发 audio_end 标记，通知主机 session 结束
        voice_ble_send_audio(s_session_id, s_seq, VOICE_BLE_FLAG_END, NULL, 0);

        ESP_LOGI(TAG, "tx task exit: sent=%" PRIu32 " dropped=%" PRIu32, sent, tx_dropped);

        // 等 audio_task 完成 drain
        while (s_audio_task != NULL) vTaskDelay(pdMS_TO_TICKS(10));

        deinit_opus();
        deinit_codec();
        deinit_i2s();
        ESP_LOGI(TAG, "session %" PRIu32 " drained & released", s_session_id);
        s_tx_task = NULL;
        vTaskDelete(NULL);
    }
}

// ─── 公共接口 ──────────────────────────────────────────────────────────
esp_err_t audio_pipeline_init(void) {
    if (s_init) return ESP_OK;
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create tx queue");
    s_init = true;
    ESP_LOGI(TAG, "audio pipeline ready (ES8311, session resources on demand)");
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
    err = init_codec();
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "codec init: %s", esp_err_to_name(err));
        return err;
    }
    err = init_opus();
    if (err != ESP_OK) {
        deinit_codec();
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    voice_ble_request_fast_interval();
    s_hpf_z1 = 0.0;
    s_hpf_z2 = 0.0;
    opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
    s_session_id = session_id;
    s_seq = 0;
    xQueueReset(s_tx_queue);
    atomic_store(&s_running, true);

    // tx_task pin Core 0：BLE 协议栈在 Core 0，notify 就近发送
    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096, NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        deinit_opus();
        deinit_codec();
        deinit_i2s();
        return ESP_ERR_NO_MEM;
    }
    // audio_task pin Core 1 + 栈放 PSRAM（WithCaps）：内部 RAM 留给 DMA/BLE。
    // 栈 32768 对齐 firmware：24KB 在加 LVGL 后会栈溢出，32KB 给足余量。
    ok = xTaskCreatePinnedToCoreWithCaps(audio_task, "audio_pipeline", 32768,
                                         NULL, 5, &s_audio_task, 1, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create audio task failed, free_internal=%u free_spiram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        atomic_store(&s_running, false);
        audio_packet_t sentinel = { .flags = VOICE_BLE_FLAG_END, .len = 0 };
        xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
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

    // 同步等 drain 完成：松开 button_up 抢在 drain 帧前到主机会提前结束会话丢尾音。
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (s_tx_task != NULL && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_tx_task != NULL) {
        ESP_LOGW(TAG, "drain timeout, tx_task still running");
    }
    return ESP_OK;
}
