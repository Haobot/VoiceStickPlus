#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// PTT 接 RTC_GPIO（ext1 唤醒要求）。StickS3 主键 GPIO11（STICK_S3_PIN_BUTTON_FRONT）。
#define PTT_PIN  11   // StickS3 主键（STICK_S3_PIN_BUTTON_FRONT），RTC_GPIO 可 ext1 唤醒

// hold_to_talk 为主（评估第5点）：按住 >HOLD_THRESHOLD 才录音；
// 短按 <HOLD_THRESHOLD 走双击窗口。放弃方案的"短按单次录音"。
#define PTT_HOLD_THRESHOLD_MS  300
#define PTT_DOUBLE_WINDOW_MS   300

// 回调签名（由 main 注入，解耦 ptt 与 audio/ble）
// duration_ms 在按键释放时传（pressed=false 有效），按下确认时传 0。
typedef void (*ptt_start_fn)(uint32_t session_id);          // 长按确认：开始录音
typedef void (*ptt_stop_fn)(uint32_t duration_ms);          // 录音中松开：结束录音（带按下时长）
typedef void (*ptt_double_click_fn)(void);                  // 双击：上报 Enter
typedef void (*ptt_button_fn)(bool pressed, uint32_t session_id, uint32_t duration_ms);  // 按键事实上报

// 初始化 PTT：配置 GPIO + 中断 + 任务，注册回调。
esp_err_t ptt_init(ptt_start_fn start, ptt_stop_fn stop,
                   ptt_double_click_fn dbl, ptt_button_fn btn);

// 进入 power off（路径 B：ESP32 deep sleep + ext1 前键唤醒）。
// 调用前需确保无残留 session（main 在调用前自行 audio_pipeline_stop）。
void ptt_enter_deep_sleep(void);
