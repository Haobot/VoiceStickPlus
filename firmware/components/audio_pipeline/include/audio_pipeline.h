#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
const char *audio_pipeline_last_error_step(void);
uint32_t audio_pipeline_session_id(void);

/* 测试回放（L3 端到端测试用）：设置预存 PCM 文件名（位于 storage SPIFFS 分区 /spiffs/ 下）。
 * 设置后，下一次 audio_pipeline_start 起，audio_task 从该文件读 16kHz/16bit/mono PCM 替代
 * ES8311 采集，走完整 HPF+Opus 编码+BLE 发送链路，用于可重复的端到端测试输入。
 * filename 为 NULL 或空串则关闭回放，恢复 ES8311 采集（默认）。
 * 非回放模式下本函数不被调用，audio_pipeline 行为零变化，保证正常录音回归安全。 */
esp_err_t audio_pipeline_set_playback_file(const char *filename);
