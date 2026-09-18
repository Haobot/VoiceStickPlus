#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 音频采集源：ES8311 为默认（物理 mic）；EXTERNAL 为外部 PCM 馈送（网关模式
 * 小米 ATVV 音频经 ADPCM 解码后喂入，见 Doc/Plan/xiaomi-remote-stick-gateway.md §5.4.1）。 */
typedef enum {
    AUDIO_SOURCE_ES8311 = 0,
    AUDIO_SOURCE_EXTERNAL,
} audio_source_t;

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint32_t session_id);
/* 外部 PCM 源会话：跳过 I2S/codec 初始化与 click_guard（遥控器 mic 无外壳按键
 * 传导，淡入会砍首音节），HPF/AGC 保留。 */
esp_err_t audio_pipeline_start_ext(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
const char *audio_pipeline_last_error_step(void);
uint32_t audio_pipeline_session_id(void);

/* 外部源 PCM 馈送（NimBLE host 任务上下文调用，单写单读安全）：
 * - 会话未启动时数据在环形缓冲暂存（hold 阈值期间不丢语音），容量 2s，
 *   溢出丢新保序；
 * - 懒创建缓冲（首次调用），audio_pipeline_external_reset 清空残留。 */
esp_err_t audio_pipeline_feed_pcm(const int16_t *pcm, size_t samples);
/* 清空外部源缓冲残留（语音键按下沿/会话收尾时调用，防跨会话污染）。 */
void audio_pipeline_external_reset(void);
/* 外部源缓冲预创建（网关模式入口调用，幂等）。把 64KB PSRAM 分配从「首次按
 * 语音键」挪到模式入口：① 分配不再落在按下→首帧的关键路径上；② 分配失败在
 * 模式入口一次性暴露，而不是每次按键刷屏；③ 模式入口时内存最干净。
 * 失败返回 ESP_ERR_NO_MEM——调用方记录日志即可，start_ext 仍会自行重试创建。 */
esp_err_t audio_pipeline_external_prepare(void);

/* 测试回放（L3 端到端测试用）：设置预存 PCM 文件名（位于 storage SPIFFS 分区 /spiffs/ 下）。
 * 设置后，下一次 audio_pipeline_start 起，audio_task 从该文件读 16kHz/16bit/mono PCM 替代
 * ES8311 采集，走完整 HPF+Opus 编码+BLE 发送链路，用于可重复的端到端测试输入。
 * filename 为 NULL 或空串则关闭回放，恢复 ES8311 采集（默认）。
 * 非回放模式下本函数不被调用，audio_pipeline 行为零变化，保证正常录音回归安全。 */
esp_err_t audio_pipeline_set_playback_file(const char *filename);
