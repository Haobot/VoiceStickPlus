// ptt.c -- PTT 按键状态机：hold_to_talk 为主 + 双击 + RTC deep sleep
//
// 修正点落地（对照评估结论）：
//   - hold_to_talk 为主（评估第5点）：按住 >300ms 才录音；短按走双击窗口。
//     放弃方案的"短按单次录音"：它与双击同源必冲突。
//   - 按需启停（评估第3点）：deep sleep 断电 I2S/外设，<15uA 与"上电常驻I2S"互斥。
//     唤醒=reset，重新走 app_main 重新初始化，不在 deep sleep 常驻 I2S。
//
// 状态机（事件驱动）：
//   DOWN   -> 启动 hold_timer(300ms)
//   HOLD   -> timer 回调检查仍按下 -> 长按确认，开始录音
//   UP     -> 若录音中：stop（含 drain）；若短按：双击窗口判定

#include "ptt.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "stick_s3_board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ptt";

typedef enum { EVT_DOWN, EVT_HOLD, EVT_UP } ptt_evt_t;

static QueueHandle_t s_q;
static atomic_bool s_running = true;
static esp_timer_handle_t s_hold_timer;

static ptt_start_fn        cb_start;
static ptt_stop_fn        cb_stop;
static ptt_double_click_fn cb_dbl;
static ptt_button_fn      cb_btn;
static uint32_t s_session_id = 1;
static int64_t s_down_us = 0;        // 按下时刻，松开算 duration 上报 button_up

// ISR：任一边沿中断，按电平判 DOWN/UP（低电平=按下）
static void IRAM_ATTR ptt_isr(void *arg) {
    (void)arg;
    ptt_evt_t e = gpio_get_level(PTT_PIN) ? EVT_UP : EVT_DOWN;
    xQueueSendFromISR(s_q, &e, NULL);
}

// hold_timer 回调（timer task 上下文）：按下 300ms 后检查仍按下则确认长按
static void hold_timer_cb(void *arg) {
    (void)arg;
    if (gpio_get_level(PTT_PIN) == 0) {   // 仍按下=长按
        ptt_evt_t e = EVT_HOLD;
        xQueueSend(s_q, &e, 0);
    }
}

static void ptt_task(void *arg) {
    (void)arg;
    ptt_evt_t e;
    int64_t last_short_up = 0;
    bool recording = false;

    while (atomic_load(&s_running)) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        int64_t now = esp_timer_get_time();

        switch (e) {
        case EVT_DOWN:
            s_down_us = now;
            // 启动 hold 判定计时：300ms 后若仍按下则确认为长按录音
            esp_timer_start_once(s_hold_timer, PTT_HOLD_THRESHOLD_MS * 1000);
            break;

        case EVT_HOLD:
            if (!recording) {
                recording = true;
                if (cb_start) cb_start(s_session_id);
                if (cb_btn)   cb_btn(true, s_session_id, 0);
            }
            break;

        case EVT_UP:
            esp_timer_stop(s_hold_timer);
            if (recording) {
                // 长按录音中松开：结束（audio_pipeline_stop 含同步 drain，保尾音）
                uint32_t duration_ms = s_down_us ? (uint32_t)((now - s_down_us) / 1000) : 0;
                if (cb_stop) cb_stop(duration_ms);
                if (cb_btn)   cb_btn(false, s_session_id, duration_ms);
                s_session_id++;
                recording = false;
                s_down_us = 0;
            } else {
                // 短按（<300ms 松开）：双击窗口判定（评估第5点）
                if (now - last_short_up < PTT_DOUBLE_WINDOW_MS * 1000) {
                    if (cb_dbl) cb_dbl();        // 双击 -> 上报 Enter
                    last_short_up = 0;
                } else {
                    last_short_up = now;          // 等待第二击
                }
            }
            break;
        }
    }
    vTaskDelete(NULL);
}

esp_err_t ptt_init(ptt_start_fn start, ptt_stop_fn stop,
                   ptt_double_click_fn dbl, ptt_button_fn btn) {
    cb_start = start;
    cb_stop = stop;
    cb_dbl = dbl;
    cb_btn = btn;

    s_q = xQueueCreate(8, sizeof(ptt_evt_t));
    ESP_RETURN_ON_FALSE(s_q != NULL, ESP_ERR_NO_MEM, TAG, "create queue");

    // hold 判定计时器（一次性）
    esp_timer_create_args_t timer_args = {
        .callback = hold_timer_cb,
        .name = "ptt_hold",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_hold_timer), TAG, "create hold timer");

    // 按键 GPIO：输入 + 内部上拉 + 任一边沿中断（低电平=按下）
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PTT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config");
    // 注：若多组件都调 install_isr_service 会冲突；脚手架单组件调用。
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "install isr service");
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PTT_PIN, ptt_isr, NULL), TAG, "isr handler add");

    BaseType_t ok = xTaskCreate(ptt_task, "ptt", 4096, NULL, 7, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create ptt task");

    ESP_LOGI(TAG, "PTT ready on GPIO%d (hold_to_talk, threshold=%dms)",
             PTT_PIN, PTT_HOLD_THRESHOLD_MS);
    return ESP_OK;
}

void ptt_enter_deep_sleep(void) {
    ESP_LOGI(TAG, "entering power off (deep sleep path B, ext1 wake on PTT GPIO%d)", PTT_PIN);
    atomic_store(&s_running, false);
    gpio_isr_handler_remove(PTT_PIN);
    gpio_uninstall_isr_service();
    rtc_gpio_deinit(PTT_PIN);

    // 路径 B（移植自 firmware/main.c:453-495）：脚手架不带 IMU，不走 M5PM1 真关机路径 A。
    stick_s3_board_prepare_deep_sleep();

    // 清残留唤醒源位（light sleep/esp_pm 配置可能遗留，否则会立即误唤醒）。
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // RTC 外设保持供电，内部 pull-up 在深睡期间有效；配合下方显式 rtc pull-up
    // 防 PTT_PIN 浮空低自唤醒。
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    (void)rtc_gpio_pulldown_dis(PTT_PIN);
    (void)rtc_gpio_pullup_en(PTT_PIN);

    // ext1 唤醒（非 ext0）：任意配置引脚低电平唤醒。PTT 按下=低电平唤醒。
    // 唤醒后 = reset，重新走 app_main 重新初始化全链路（按需启停，不常驻 I2S）。
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << PTT_PIN,
                                                     ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        return;
    }

    // 等唤醒 pin settle high：松开抖动/寄生电容。若仍低会立即唤醒，abort 重试。
    int wait_ms = 0;
    while (gpio_get_level(PTT_PIN) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (gpio_get_level(PTT_PIN) == 0) {
        ESP_LOGW(TAG, "wake pin still low, abort deep sleep");
        return;
    }

    ESP_LOGI(TAG, "power off go (wait_ms=%d level=%d)", wait_ms, gpio_get_level(PTT_PIN));
    esp_deep_sleep_start();
    // 不会返回
}
