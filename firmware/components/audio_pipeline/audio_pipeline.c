#include "audio_pipeline.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "opus.h"

#include "stick_s3_board.h"
#include "voice_ble.h"

static const char *TAG = "audio_pipeline";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MS 40
#define AUDIO_FRAME_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define OPUS_BITRATE 32000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY 1

#define TX_QUEUE_DEPTH 50
#define TX_RETRY_DELAY_MS 10
#define TX_MAX_RETRIES 30
#define TX_DRAIN_TIMEOUT_MS 500
#define TASK_EXIT_WAIT_MS 800

/* 二阶 Butterworth 高通滤波器（transposed direct form II），截止 fc≈90Hz @ 16kHz，
 * 抑制近距离说话时呼吸气流冲击麦克风的低频爆破音（plosive，能量集中在 20-100Hz）。
 * 硬件 ES8311 ADC HPF 截止太低（去 DC 级）无法去爆破音，故在 PCM 进入 Opus 前软件再过一道。
 * 系数（Q=0.707 Butterworth，fc=90Hz，fs=16000Hz）离线计算并验证频响：
 * 90Hz -3.0dB / 50Hz -10.6dB / 30Hz -19.1dB / 150Hz -0.5dB / 300Hz+ ~0dB（语音中高频无损）。
 * 截止频率若需调整，重算系数即可（scripts/ 下可复现）。状态每会话开始时清零。 */
static const double kHpfB0 = 0.975318;
static const double kHpfB1 = -1.950637;
static const double kHpfB2 = 0.975318;
static const double kHpfA1 = -1.950028;
static const double kHpfA2 = 0.951246;
static double s_hpf_z1 = 0.0;
static double s_hpf_z2 = 0.0;

static inline int16_t hpf_process(int16_t x) {
    double in = (double)x;
    double y = kHpfB0 * in + s_hpf_z1;
    s_hpf_z1 = kHpfB1 * in - kHpfA1 * y + s_hpf_z2;
    s_hpf_z2 = kHpfB2 * in - kHpfA2 * y;
    /* 软限幅防数值漂移越界（biquad 稳定但极端输入下保险）。 */
    if (y > 32767.0) y = 32767.0;
    else if (y < -32768.0) y = -32768.0;
    return (int16_t)lround(y);
}

typedef struct {
    uint32_t session_id;
    uint32_t seq;
    uint8_t  flags;
    uint16_t len;
    uint8_t  data[OPUS_MAX_PACKET_SIZE];
} audio_packet_t;

static atomic_bool s_running;
static bool s_initialized;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_tx_task;
static QueueHandle_t s_tx_queue;
// 首字延迟诊断：audio_pipeline_start 入口时刻，audio_task 首帧入队时打印相对值量化固件侧延迟。
static int64_t s_pipeline_start_us = 0;

/* Per-session resources: created on start, destroyed on stop */
static i2s_chan_handle_t s_rx_handle;
static i2s_chan_handle_t s_tx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static OpusEncoder *s_opus_encoder;
static const char *s_last_error_step = "none";

/* 测试回放状态（L3）：s_playback_active=true 时 audio_task 从 s_playback_fp 读 PCM 替代采集。
 * 默认 false，正常录音走 ES8311 采集分支，零行为变化。仅 test_playback 控制命令激活。 */
static bool s_playback_active = false;
/* 预读 PCM 到 PSRAM 缓冲：audio_task 栈在 PSRAM（见 audio_pipeline_start），不能直接 fread
 * SPIFFS——flash 操作 cache 禁用期间 PSRAM 栈不可访问，会触发 esp_task_stack_is_sane_cache_disabled
 * 断言崩溃。故在 set_playback_file（main 任务，内部 RAM 栈）一次性 fread 到 PSRAM buffer，
 * audio_task 仅 memcpy，不触 flash 操作。 */
static uint8_t *s_playback_buffer = NULL;
static size_t s_playback_size = 0;
static size_t s_playback_pos = 0;
static bool s_spiffs_mounted = false;

/* 懒挂载 storage SPIFFS 分区到 /spiffs。仅在首次启用回放时调用，正常录音不触发。 */
static esp_err_t ensure_spiffs_mounted(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "spiffs mounted (storage)");
    return ESP_OK;
}

esp_err_t audio_pipeline_set_playback_file(const char *filename)
{
    if (filename == NULL || filename[0] == '\0') {
        s_playback_active = false;
        if (s_playback_buffer != NULL) {
            heap_caps_free(s_playback_buffer);
            s_playback_buffer = NULL;
        }
        s_playback_size = 0;
        s_playback_pos = 0;
        ESP_LOGI(TAG, "playback disabled (restore ES8311 capture)");
        return ESP_OK;
    }
    esp_err_t err = ensure_spiffs_mounted();
    if (err != ESP_OK) {
        return err;
    }
    char path[80];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "playback open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        ESP_LOGE(TAG, "playback %s empty or size failed", path);
        return ESP_ERR_INVALID_SIZE;
    }
    /* 一次性读入 PSRAM buffer：本函数在 main 任务（内部 RAM 栈）调用，flash 读安全。
     * audio_task（PSRAM 栈）后续仅 memcpy 此 buffer，不触 flash 操作。 */
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(fp);
        ESP_LOGE(TAG, "playback alloc %ld bytes spiram failed", sz);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if ((long)got != sz) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "playback read %s short: %zu/%ld", path, got, sz);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s_playback_buffer != NULL) {
        heap_caps_free(s_playback_buffer);
    }
    s_playback_buffer = buf;
    s_playback_size = (size_t)sz;
    s_playback_pos = 0;
    s_playback_active = true;
    ESP_LOGI(TAG, "playback loaded: %s size=%zu", path, s_playback_size);
    return ESP_OK;
}

static bool tasks_exited(void)
{
    return s_audio_task == NULL && s_tx_task == NULL;
}

static esp_err_t wait_for_tasks_to_exit(TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (!tasks_exited()) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "tasks still exiting: audio=%p tx=%p",
                     s_audio_task, s_tx_task);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 120;
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                        TAG, "create i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = STICK_S3_PIN_ES8311_DOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "init i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                        TAG, "init i2s tx");
    /* 必须在此 enable：esp_codec_dev_open 随后会做一次 reconfig，流程为
     * disable→init_std→enable。若通道此刻未使能，reconfig 的 disable 会触发驱动
     * "the channel has not been enabled yet" ERROR。先 enable 让通道进入 RUNNING，
     * reconfig 的 disable 才能正常回到 READY 再重新 enable。
     * 停止侧不再重复 disable：esp_codec_dev_close 已成对 disable 两个通道，
     * deinit_i2s 只需 i2s_del_channel 即可，避免再次 disable 已 disable 的通道。 */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx");
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
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
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio");

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
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");
    /* PGA 增益：原 36 dB 是 ES8311 最大档，近场声压叠加后致 ADC 硬削波、ASR 变差。
     * 经多轮下调：24dB -> 18dB。18dB 留足 headroom 防近场削波，远场由 ALC 拉起补偿，
     * PGA 不再承担远场增益职责。 */
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 18.0) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set mic gain");
    /* 启用 ES8311 硬件 ADC ALC（自动电平控制），在 18 dB PGA 基础上动态压/拉增益，
     * 自适应不同说话距离：近场大声自动压防削波，远场小声自动拉起。
     *
     * 位域以 Linux 主线 sound/soc/codecs/es8311.h 为权威源（曾因按 es8311_reg.h 注释
     * "bit[7:4]=winsize, bit[3]=ALC enable" 理解而写反，致 ALC 长期未使能）：
     *   REG18(0x18): bit[7]=ALC_EN, bit[6]=AUTOMUTE_EN, bit[3:0]=ALC_WINSIZE
     *   REG19(0x19): bit[7:4]=ALC_MAXLEVEL(目标电平上限), bit[3:0]=ALC_MINLEVEL(拉起下限)
     * 参数：winsize=3（短响应），maxlevel=8（约 -11dBFS，饱满但仍在安全区），
     * minlevel=0（-30dBFS，远场拉到最低），不开 automute（避免误判停顿为静音）。
     * REG1B/REG1C 不写，保留 es8311_open 默认（0x1B=0x0A/0x1C=0x6A，含 ADC HPF）。
     * 参数与位域推导见 Doc/Plan/es8311-alc-bitfield-fix.md。 */
    ESP_RETURN_ON_FALSE(esp_codec_dev_write_reg(s_codec, 0x18, 0x83) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set alc enable+winsize");
    ESP_RETURN_ON_FALSE(esp_codec_dev_write_reg(s_codec, 0x19, 0x80) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set alc target level");
    ESP_RETURN_ON_FALSE(esp_codec_dev_write_reg(s_codec, 0x1A, 0x00) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set alc automute off");
    return ESP_OK;
}

static esp_err_t init_opus(void)
{
    int error = OPUS_OK;
    s_opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_opus_encoder != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "create opus encoder error=%d", error);
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return ESP_OK;
}

static void deinit_opus(void)
{
    if (s_opus_encoder) {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
    }
}

static void deinit_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
}

static void deinit_i2s(void)
{
    /* 通道的 enable/disable 由 esp_codec_dev_open/close 成对管理，close 后通道
     * 已处于 READY 态可直接删除。此处若再 i2s_channel_disable，会对已 disable
     * 的通道重复调用，驱动内部先打 "the channel has not been enabled yet"
     * ERROR 再返回 ESP_ERR_INVALID_STATE，应用层判断返回码无法抑制该日志，
     * 故直接删除句柄即可。 */
    if (s_rx_handle) {
        esp_err_t err = i2s_del_channel(s_rx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete i2s rx failed: %s", esp_err_to_name(err));
        }
        s_rx_handle = NULL;
    }
    if (s_tx_handle) {
        esp_err_t err = i2s_del_channel(s_tx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete i2s tx failed: %s", esp_err_to_name(err));
        }
        s_tx_handle = NULL;
    }
}

static void deinit_session_resources(void)
{
    deinit_opus();
    deinit_codec();
    deinit_i2s();
    ESP_LOGI(TAG, "session resources released");
}

static void audio_task(void *arg)
{
    (void)arg;
    /* 缓冲放回任务栈：本任务栈分配在 PSRAM（见 audio_pipeline_start），栈上局部不占
     * 内部 RAM；放栈上比 static .bss 更省内部 RAM，且无单实例之外的重入问题。 */
    int16_t stereo[AUDIO_FRAME_SAMPLES * 2];
    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_buf[OPUS_MAX_PACKET_SIZE];
    uint32_t enqueued = 0;
    uint32_t dropped = 0;

    while (atomic_load(&s_running)) {
        if (s_playback_active && s_playback_buffer != NULL) {
            /* 回放模式：从 PSRAM 预读缓冲取 mono 样本替代 ES8311 采集。
             * 仅 memcpy（不触 flash）：audio_task 栈在 PSRAM，cache 禁用期不可 fread。 */
            const size_t need = AUDIO_FRAME_SAMPLES * sizeof(int16_t);
            if (s_playback_pos + need <= s_playback_size) {
                memcpy(mono, s_playback_buffer + s_playback_pos, need);
                s_playback_pos += need;
            } else {
                const size_t remain = s_playback_size - s_playback_pos;
                if (remain > 0) memcpy(mono, s_playback_buffer + s_playback_pos, remain);
                if (remain < need) memset((uint8_t *)mono + remain, 0, need - remain);
                s_playback_pos = s_playback_size;
            }
            /* 回放无 I2S 阻塞，按帧时长节流模拟实时采集节奏；否则全速循环会把 PCM 压缩成加速音频，ASR 无法识别。 */
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
        } else {
            esp_err_t err = esp_codec_dev_read(s_codec, stereo, sizeof(stereo));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "codec read failed: %s", esp_err_to_name(err));
                continue;
            }
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
                mono[i] = stereo[i * 2];
            }
        }
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            mono[i] = hpf_process(mono[i]);
        }

        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_buf, sizeof(opus_buf));
        if (encoded < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)encoded);
            continue;
        }

        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = (s_seq == 0) ? VOICE_BLE_FLAG_START : 0x00,
            .len = (uint16_t)encoded,
        };
        memcpy(pkt.data, opus_buf, encoded);

        if (xQueueSend(s_tx_queue, &pkt, 0) == pdTRUE) {
            s_seq++;
            enqueued++;
            if (enqueued == 1) {
                // 首帧入队时刻：量化从 pipeline start 到首帧就绪的固件侧延迟
                //（含 codec read 20ms PCM + opus encode + 入队），桌面端日志对照可分离 BLE 传输延迟。
                ESP_LOGI(TAG, "latency: first frame enqueued %lldus after pipeline start",
                         esp_timer_get_time() - s_pipeline_start_us);
            }
        } else {
            /* Queue full: drop oldest packet to make room */
            audio_packet_t discard;
            xQueueReceive(s_tx_queue, &discard, 0);
            xQueueSend(s_tx_queue, &pkt, 0);
            s_seq++;
            enqueued++;
            dropped++;
            if (dropped == 1 || (dropped % 20) == 0) {
                ESP_LOGW(TAG, "tx queue overflow, dropped oldest (total=%" PRIu32 ")", dropped);
            }
        }
    }

    /* Drain：松开按键时 I2S DMA 缓冲区（4 描述符×120 帧 ≈ 60ms）里仍有残留尾音 PCM，
     * 若不读出编码发出，用户说完话立即松开会丢最后 1-2 字（instant 模式下尤为明显）。
     * 这里固定读 2 帧（80ms，覆盖 60ms 残留+余量）编码入队，让 tx_task 的 drain 一并发完。 */
    for (int drain = 0; drain < 2; ++drain) {
        if (s_playback_active && s_playback_buffer != NULL) {
            const size_t need = AUDIO_FRAME_SAMPLES * sizeof(int16_t);
            if (s_playback_pos + need <= s_playback_size) {
                memcpy(mono, s_playback_buffer + s_playback_pos, need);
                s_playback_pos += need;
            } else {
                const size_t remain = s_playback_size - s_playback_pos;
                if (remain > 0) memcpy(mono, s_playback_buffer + s_playback_pos, remain);
                if (remain < need) memset((uint8_t *)mono + remain, 0, need - remain);
                s_playback_pos = s_playback_size;
            }
        } else {
            esp_err_t err = esp_codec_dev_read(s_codec, stereo, sizeof(stereo));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "drain codec read failed: %s", esp_err_to_name(err));
                break;
            }
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
                mono[i] = stereo[i * 2];
            }
        }
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            mono[i] = hpf_process(mono[i]);
        }
        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_buf, sizeof(opus_buf));
        if (encoded < 0) {
            ESP_LOGE(TAG, "drain opus encode failed: %d", (int)encoded);
            break;
        }
        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = 0x00,
            .len = (uint16_t)encoded,
        };
        memcpy(pkt.data, opus_buf, encoded);
        if (xQueueSend(s_tx_queue, &pkt, 0) == pdTRUE) {
            s_seq++;
            enqueued++;
        }
    }

    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32 " overflow_drops=%" PRIu32
             " stack_hwm=%u",
             enqueued, dropped, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_audio_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void tx_task(void *arg)
{
    (void)arg;
    audio_packet_t pkt;
    uint32_t sent = 0;
    uint32_t tx_dropped = 0;

    while (true) {
        if (xQueueReceive(s_tx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Sentinel: END flag with no payload signals drain mode */
        if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
            goto drain;
        }

        int retries = 0;
        while (true) {
            esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                 pkt.flags, pkt.data, pkt.len);
            if (err == ESP_OK) {
                sent++;
                break;
            }
            retries++;
            if (retries >= TX_MAX_RETRIES) {
                tx_dropped++;
                if (tx_dropped == 1 || (tx_dropped % 20) == 0) {
                    ESP_LOGW(TAG, "tx failed after %d retries seq=%" PRIu32
                             " (total_dropped=%" PRIu32 ")",
                             TX_MAX_RETRIES, pkt.seq, tx_dropped);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
        }
    }

drain:
    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_DRAIN_TIMEOUT_MS);
        while (xQueueReceive(s_tx_queue, &pkt, 0) == pdTRUE) {
            if (pkt.flags == VOICE_BLE_FLAG_END && pkt.len == 0) {
                break;
            }
            if (xTaskGetTickCount() >= deadline) {
                UBaseType_t remaining = uxQueueMessagesWaiting(s_tx_queue);
                if (remaining > 0) {
                    ESP_LOGW(TAG, "drain timeout, discarding %u packets",
                             (unsigned)remaining);
                }
                xQueueReset(s_tx_queue);
                break;
            }
            int retries = 0;
            while (true) {
                esp_err_t err = voice_ble_send_audio(pkt.session_id, pkt.seq,
                                                     pkt.flags, pkt.data, pkt.len);
                if (err == ESP_OK) {
                    sent++;
                    break;
                }
                retries++;
                if (retries >= TX_MAX_RETRIES || xTaskGetTickCount() >= deadline) {
                    tx_dropped++;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
            }
        }

        /* Send the END marker over BLE */
        voice_ble_send_audio(s_session_id, s_seq, VOICE_BLE_FLAG_END, NULL, 0);

        ESP_LOGI(TAG, "tx task exit: sent=%" PRIu32 " dropped=%" PRIu32, sent, tx_dropped);

        /* Wait for audio_task to finish before destroying shared resources */
        while (s_audio_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        deinit_session_resources();
        s_tx_task = NULL;
        vTaskDelete(NULL);
    }
}

esp_err_t audio_pipeline_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create tx queue");

    s_initialized = true;
    ESP_LOGI(TAG, "audio pipeline ready (resources allocated on demand)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(uint32_t session_id)
{
    s_last_error_step = "none";
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    s_last_error_step = "wait";
    esp_err_t err = wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wait previous session exit: %s", esp_err_to_name(err));
        return err;
    }
    s_pipeline_start_us = esp_timer_get_time();

    s_last_error_step = "i2s";
    err = init_i2s();
    int64_t t_after_i2s = esp_timer_get_time();
    ESP_LOGI(TAG, "latency: init_i2s %lldus", t_after_i2s - s_pipeline_start_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init: %s", esp_err_to_name(err));
        return err;
    }
    s_last_error_step = "codec";
    err = init_codec();
    int64_t t_after_codec = esp_timer_get_time();
    ESP_LOGI(TAG, "latency: init_codec %lldus", t_after_codec - t_after_i2s);
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "codec init: %s", esp_err_to_name(err));
        return err;
    }
    s_last_error_step = "opus";
    err = init_opus();
    int64_t t_after_opus = esp_timer_get_time();
    ESP_LOGI(TAG, "latency: init_opus %lldus", t_after_opus - t_after_codec);
    if (err != ESP_OK) {
        deinit_codec();
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    voice_ble_request_fast_interval();

    xQueueReset(s_tx_queue);
    s_session_id = session_id;
    s_seq = 0;
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    s_hpf_z1 = 0.0;
    s_hpf_z2 = 0.0;
    /* 回放模式：每次录音从缓冲开头重放，保证可重复。 */
    if (s_playback_active && s_playback_buffer != NULL) {
        s_playback_pos = 0;
    }
    atomic_store(&s_running, true);

    s_last_error_step = "tx_task";
    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096,
                                            NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_tx_task = NULL;
        deinit_session_resources();
        return ESP_ERR_NO_MEM;
    }

    s_last_error_step = "audio_task";
    /* 任务栈分配在 PSRAM（MALLOC_CAP_SPIRAM）：Wi-Fi 常驻会占满内部 RAM，
     * CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768 又把内部 RAM 留给 DMA/Wi-Fi，
     * 普通 xTaskCreate 在共存场景下凑不出 24KB 连续内部 RAM 会 ESP_ERR_NO_MEM。
     * 放 PSRAM 后不占内部 RAM；本任务只做 codec 读取与 opus 编码，不在 cache 禁用期
     * （flash 写）运行（OTA 与录音互斥），PSRAM 栈安全。栈 32768 给足余量杜绝溢出。
     * 配套：自删用 vTaskDeleteWithCaps。 */
    ok = xTaskCreatePinnedToCoreWithCaps(audio_task, "audio_pipeline", 32768,
                                         NULL, 5, &s_audio_task, 1, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create audio task failed, free internal=%u largest_internal=%u free_spiram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        audio_packet_t sentinel = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = VOICE_BLE_FLAG_END,
            .len = 0,
        };
        xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
        (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        /* tx_task cleans up session resources on exit */
        return ESP_ERR_NO_MEM;
    }
    s_last_error_step = "none";
    ESP_LOGI(TAG, "start session %" PRIu32 " (pipeline init %lldus)",
             session_id, t_after_opus - s_pipeline_start_us);
    return ESP_OK;
}

const char *audio_pipeline_last_error_step(void)
{
    return s_last_error_step;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!atomic_load(&s_running)) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "stop session %" PRIu32, s_session_id);

    /* Send sentinel to trigger tx_task drain and exit */
    audio_packet_t sentinel = {
        .session_id = s_session_id,
        .seq = s_seq,
        .flags = VOICE_BLE_FLAG_END,
        .len = 0,
    };
    xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);

    /* 同步等 audio_task + tx_task 退出：audio_task 先 drain I2S DMA 残留尾音编码入队，
     * tx_task 再把队列排空（含 drain 帧）发往 BLE 并发 audio_end，最后 deinit codec/i2s。
     * 必须等两者退出后再返回，否则 stop_recording 后续发送的 button_up 会先于 drain 帧/
     * audio_end 到达桌面端，桌面端收到 button_up 即停止虚拟麦克风渲染，尾音被丢弃
     * （用户说完立即松开会丢最后 1-2 字）。超时兜底 TASK_EXIT_WAIT_MS，避免极端积压卡死。 */
    (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
    return ESP_OK;
}

uint32_t audio_pipeline_session_id(void)
{
    return s_session_id;
}
