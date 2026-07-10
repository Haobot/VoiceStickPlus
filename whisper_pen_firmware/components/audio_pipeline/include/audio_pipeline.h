#pragma once

#include <stdint.h>
#include "esp_err.h"

// === 采样参数 ===
#define AP_SAMPLE_RATE    16000
#define AP_CHANNELS       1
#define AP_FRAME_MS       40
#define AP_FRAME_SAMPLES  ((AP_SAMPLE_RATE * AP_FRAME_MS) / 1000)   // 640 samples/帧

// === ICS-41351 引脚（按实际板改；必须可作 I2S 的 GPIO）===
// ES8311 引脚取自 stick_s3_board.h（StickS3 板级定义），本头文件不再单独定义

// 仅创建发送队列；session 资源（I2S/Opus）按需创建、停止时释放。
esp_err_t audio_pipeline_init(void);

// 启动一次录音 session：按需 init I2S+Opus，创建双核任务，请求快 interval。
esp_err_t audio_pipeline_start(uint32_t session_id);

// 停止 session：同步等 drain 完成再返回，防松开丢尾音。返回后 I2S/Opus 已释放。
esp_err_t audio_pipeline_stop(void);
