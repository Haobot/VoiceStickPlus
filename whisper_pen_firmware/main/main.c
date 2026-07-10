// main.c -- 应用编排：BLE + 音频管线 + PTT 状态机 + 空闲 deep sleep
//
// 初始化顺序（评估第3点：按需启停，待机零功耗）：
//   1. voice_ble_init    NimBLE 启动 + GATT + 广播（异步在 on_sync 开始）
//   2. audio_pipeline_init  仅创建发送队列；session 资源按需创建/释放
//   3. ptt_init           注册回调：长按录音 / 松开停止 / 双击 Enter
//   4. idle timer         5 分钟无活动 -> deep sleep（PTT RTC 唤醒）
//
// 录音链路（PTT 长按触发）：
//   ptt EVT_HOLD -> audio_pipeline_start -> Core1 I2S+Opus -> queue -> Core0 BLE notify

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "ptt.h"
#include "voice_ble.h"

static const char *TAG = "main";

#define IDLE_DEEP_SLEEP_MS (5 * 60 * 1000)   // 5 分钟无活动进 deep sleep

static esp_timer_handle_t s_idle_timer;

// ─── PTT 回调（由 ptt 组件在按键事件时调用）──────────────────
static void on_recording_start(uint32_t session_id) {
    audio_pipeline_start(session_id);
    voice_ble_request_fast_interval();
}
static void on_recording_stop(void) {
    audio_pipeline_stop();              // 含同步 drain，保尾音
    voice_ble_request_slow_interval();
}
static void on_double_click(void) {
    // 双击 -> 上报 Enter（state 通道脚手架仅日志，后续扩展）
    voice_ble_send_button_event("primary", false, 0);
}
static void on_button_event(bool pressed, uint32_t session_id) {
    voice_ble_send_button_event("primary", pressed, session_id);
}

// ─── 空闲 deep sleep ────────────────────────────────────────
static void idle_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "idle timeout, entering deep sleep");
    audio_pipeline_stop();     // 确保无残留 session（幂等）
    ptt_enter_deep_sleep();    // 配 RTC 唤醒 + deep sleep（不返回）
}

static void kick_idle_timer(void) {
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, IDLE_DEEP_SLEEP_MS * 1000);
}

void app_main(void) {
    ESP_LOGI(TAG, "Whisper Pen boot");

    ESP_ERROR_CHECK(voice_ble_init());
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(ptt_init(on_recording_start, on_recording_stop,
                             on_double_click, on_button_event));

    esp_timer_create_args_t idle_args = { .callback = idle_timer_cb, .name = "idle" };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_idle_timer));
    kick_idle_timer();

    // 主循环保活：idle timer 到期触发 deep sleep。
    // deep sleep 唤醒 = reset，重新走 app_main，无需在此恢复状态。
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
