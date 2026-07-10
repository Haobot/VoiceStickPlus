// main.c -- 应用编排：板级 + LCD + BLE + 音频管线 + PTT + 空闲 power off
//
// 初始化顺序：
//   1. stick_s3_board_init  PMIC + I2C + 引脚（codec/LCD 供电前提，必须最先）
//   2. ui_status_init        ST7789 LCD + LVGL + 背光（显示 VS-XXXX 配对）
//   3. voice_ble_init        NimBLE + GATT(128-bit 互通协议) + 注册连接/control 回调
//   4. audio_pipeline_init   仅创建发送队列；session 资源按需创建/释放
//   5. ptt_init              注册回调：长按录音 / 松开停止 / 双击
//   6. idle timer            5 分钟无活动 -> power off（ext1 前键唤醒）
//
// 录音链路（PTT 长按触发）：
//   ptt EVT_HOLD -> button_down + audio_pipeline_start -> Core1 ES8311+Opus -> Core0 BLE notify
//   UI 跟随状态：recording 图标 -> 松开 idle

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "ptt.h"
#include "stick_s3_board.h"
#include "ui_status.h"
#include "voice_ble.h"

static const char *TAG = "main";

#define IDLE_DEEP_SLEEP_MS (5 * 60 * 1000)   // 5 分钟无活动进 power off
#define DISPLAY_ACTIVE_BRIGHTNESS 20         // 与 firmware 对齐（memory: 亮度 32->20）

static esp_timer_handle_t s_idle_timer;

static void kick_idle_timer(void) {
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, IDLE_DEEP_SLEEP_MS * 1000);
}

// ─── PTT 回调（由 ptt 组件在按键事件时调用）──────────────────
// 长按确认：启动音频 + 请求快 interval + 录音图标。button_down 由 on_button_event 发。
static void on_recording_start(uint32_t session_id) {
    audio_pipeline_start(session_id);
    voice_ble_request_fast_interval();
    ui_status_set_recording(session_id);
    kick_idle_timer();
}
// 录音中松开：停止音频（含同步 drain 保尾音）+ 请求慢 interval + 回 idle。
static void on_recording_stop(uint32_t duration_ms) {
    (void)duration_ms;   // button_up 的 duration 由 on_button_event 发
    audio_pipeline_stop();
    voice_ble_request_slow_interval();
    ui_status_set_idle();
    kick_idle_timer();
}
// 双击：上报 button_double_click，桌面端注入 Enter。
static void on_double_click(void) {
    voice_ble_send_button_double_click("primary");
    kick_idle_timer();
}
// 按键事实上报：按下发 button_down，松开发 button_up（带按下时长）。
static void on_button_event(bool pressed, uint32_t session_id, uint32_t duration_ms) {
    if (pressed) {
        voice_ble_send_button_down("primary", session_id);
    } else {
        voice_ble_send_button_up("primary", duration_ms, session_id);
    }
    kick_idle_timer();
}

// ─── BLE 连接回调 ───────────────────────────────────────────
static void on_connection(bool connected) {
    if (connected) {
        // 上报 device_info：固件版本/能力，桌面端配对识别。
        voice_ble_send_device_info();
        ui_status_set_idle();      // 已连接待机
        kick_idle_timer();
    }
}

// ─── control_rx 回调：解析桌面端下发的 JSON ─────────────────
// 脚手架固定 hold_to_talk；ui_state 驱动 LCD 状态；air_mouse/tap/imu 忽略。
static void on_control_rx(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "control rx parse fail: %s", json);
        return;
    }
    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (cJSON_IsString(event)) {
        if (strcmp(event->valuestring, "ui_state") == 0) {
            // 桌面端权威 UI 状态：渲染到 LCD
            cJSON *state = cJSON_GetObjectItem(root, "state");
            cJSON *text = cJSON_GetObjectItem(root, "text");
            const char *s = cJSON_IsString(state) ? state->valuestring : "";
            const char *t = cJSON_IsString(text) ? text->valuestring : "";
            if (strcmp(s, "ready") == 0) {
                ui_status_set_idle();
            } else if (strcmp(s, "recording") == 0) {
                ui_status_set_recording(0);
            } else if (strcmp(s, "thinking") == 0) {
                ui_status_set_partial_text(t);
            } else if (strcmp(s, "pending_confirmation") == 0) {
                ui_status_set_partial_text(t[0] ? t : "Confirm or cancel");
            } else if (strcmp(s, "error") == 0) {
                ui_status_set_error(t[0] ? t : "Error");
            }
        } else if (strcmp(event->valuestring, "interaction_mode") == 0) {
            cJSON *mode = cJSON_GetObjectItem(root, "mode");
            if (cJSON_IsString(mode)) {
                // 脚手架固定 hold_to_talk；记录收到模式但不生效（老固件忽略未知模式语义）。
                ESP_LOGI(TAG, "interaction_mode=%s (hold_to_talk fixed)", mode->valuestring);
            }
        }
        // air_mouse/tap/imu 等忽略（脚手架无 IMU）
    }
    cJSON_Delete(root);
}

// ─── 空闲 power off ─────────────────────────────────────────
static void idle_timer_cb(void *arg) {
    (void)arg;
    // USB 供电不关机（保持可用）；录音中由 kick 重置 timer 不会到此。
    bool usb = false;
    if (stick_s3_board_usb_powered(&usb) == ESP_OK && usb) {
        ESP_LOGI(TAG, "idle but USB powered, keep awake");
        kick_idle_timer();
        return;
    }
    ESP_LOGI(TAG, "idle timeout, entering power off");
    audio_pipeline_stop();              // 确保无残留 session（幂等）
    (void)ui_status_set_brightness(0);  // 熄屏
    ui_status_prepare_deep_sleep();
    ptt_enter_deep_sleep();              // 路径 B：deep sleep + ext1 前键唤醒（不返回）
}

void app_main(void) {
    ESP_LOGI(TAG, "Whisper Pen boot (StickS3, ES8311)");

    // 最先：PMIC 配 L3B 给 codec/LCD 供电 + I2C bus（codec init 前提）
    ESP_ERROR_CHECK(stick_s3_board_init());
    // LCD + LVGL + 背光：初始化后即可显示配对界面
    ESP_ERROR_CHECK(ui_status_init());
    (void)ui_status_set_brightness(DISPLAY_ACTIVE_BRIGHTNESS);

    esp_err_t ble_err = voice_ble_init();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(ble_err));
        ui_status_set_error("BLE init failed");
    } else {
        voice_ble_set_connection_callback(on_connection);
        voice_ble_set_control_callback(on_control_rx);
        ui_status_set_device_name(voice_ble_device_name());
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Audio init failed");
    }

    ESP_ERROR_CHECK(ptt_init(on_recording_start, on_recording_stop,
                             on_double_click, on_button_event));

    // BLE 初始化成功即显示配对界面（VS-XXXX），等待桌面端连接
    if (ble_err == ESP_OK) {
        ui_status_set_pairing(voice_ble_device_name());
    }

    esp_timer_create_args_t idle_args = { .callback = idle_timer_cb, .name = "idle" };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_idle_timer));
    kick_idle_timer();

    ESP_LOGI(TAG, "Whisper Pen booted");
    // 主循环保活：idle timer 到期触发 power off。
    // deep sleep 唤醒 = reset，重新走 app_main，无需在此恢复状态。
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
