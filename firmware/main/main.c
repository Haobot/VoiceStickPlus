#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include "hal/gpio_types.h"
#include "iot_button.h"

#include "audio_pipeline.h"
#include "bmi270.h"
#include "stick_s3_board.h"
#include "ui_status.h"
#include "voice_ble.h"

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_MS (10 * 1000)
#define DISPLAY_DIM_TIMEOUT_MS (30 * 1000)
#define DISPLAY_OFF_TIMEOUT_MS (60 * 1000)      // T_off：S1 Resting → S2 ScreenOff
#define POWEROFF_TIMEOUT_MS (10 * 60 * 1000)    // T_pwr：S2 ScreenOff → S3 PowerOff（连接态）
#define DISC_POWEROFF_TIMEOUT_MS (10 * 60 * 1000)  // T_disc：BLE 断连 → S3 PowerOff
#define DISPLAY_ACTIVE_BRIGHTNESS 32
#define DISPLAY_DIM_BRIGHTNESS 8
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define DISPLAY_OFF_TIMEOUT_US (DISPLAY_OFF_TIMEOUT_MS * 1000ULL)
#define POWEROFF_TIMEOUT_US (POWEROFF_TIMEOUT_MS * 1000ULL)
#define DISC_POWEROFF_TIMEOUT_US (DISC_POWEROFF_TIMEOUT_MS * 1000ULL)
#define BATTERY_REFRESH_FALLBACK_US (BATTERY_REFRESH_FALLBACK_MS * 1000ULL)
// BMI270 拿起轮询周期。仅在 S1(Resting)/S2(ScreenOff) 态启用：用户放下设备后拿起即亮屏回 S0。
// 周期过长会丢快速拿起动作，过短增加功耗；500ms 兼顾两者。BMI270 不在线时轮询空转无开销。
#define PICKUP_POLL_INTERVAL_MS (500)
#define PICKUP_POLL_INTERVAL_US (PICKUP_POLL_INTERVAL_MS * 1000ULL)

static bool s_recording;
static bool s_ota_updating;
static bool s_display_dimmed;   // S1 Resting：背光降到 8
static bool s_screen_off;       // S2 ScreenOff：背光 0 + L3B 关，BLE 保连
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;
static bool s_battery_charging;
static bool s_usb_powered;
static int s_battery_level = 0;
static bool s_prompt_tone_enabled = true;
static esp_pm_lock_handle_t s_cpu_freq_lock;
static esp_timer_handle_t s_display_dim_timer;
static esp_timer_handle_t s_display_off_timer;   // S1→S2
static esp_timer_handle_t s_poweroff_timer;      // S2→S3（原 deep_sleep_timer 改造）
static esp_timer_handle_t s_disc_poweroff_timer; // BLE 断连→S3
static esp_timer_handle_t s_battery_refresh_timer;
static esp_timer_handle_t s_host_response_timer;
static esp_timer_handle_t s_pickup_poll_timer;
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static button_handle_t s_front_button;
static button_handle_t s_side_button;
static int64_t s_primary_down_us;
static int64_t s_secondary_down_us;
static uint32_t s_primary_session_id;

typedef enum {
    APP_INPUT_SOURCE_PHYSICAL,
    APP_INPUT_SOURCE_REMOTE,
} app_input_source_t;

static void apply_app_ui_state(const char *state, const char *text);

typedef enum {
    PRIMARY_OWNER_NONE,
    PRIMARY_OWNER_PHYSICAL,
    PRIMARY_OWNER_REMOTE,
} primary_owner_t;

static primary_owner_t s_primary_owner = PRIMARY_OWNER_NONE;

typedef enum {
    APP_UI_STATE_READY,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_PENDING_CONFIRMATION,
    APP_UI_STATE_ERROR,
} app_ui_state_t;

static app_ui_state_t s_app_ui_state = APP_UI_STATE_READY;

typedef enum {
    INTERACTION_MODE_HOLD_TO_TALK,
    INTERACTION_MODE_CLICK_TO_TALK,
} interaction_mode_t;

static interaction_mode_t s_interaction_mode = INTERACTION_MODE_HOLD_TO_TALK;

static const char *app_ui_state_name(app_ui_state_t state)
{
    switch (state) {
    case APP_UI_STATE_READY:
        return "ready";
    case APP_UI_STATE_RECORDING:
        return "recording";
    case APP_UI_STATE_THINKING:
        return "thinking";
    case APP_UI_STATE_PENDING_CONFIRMATION:
        return "pending_confirmation";
    case APP_UI_STATE_ERROR:
        return "error";
    }
    return "unknown";
}

typedef enum {
    APP_EVENT_FRONT_DOWN,
    APP_EVENT_FRONT_UP,
    APP_EVENT_SIDE_DOWN,
    APP_EVENT_SIDE_UP,
    APP_EVENT_UI_STATE,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_BATTERY_STATUS_REQUEST,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
    APP_EVENT_HOST_RESPONSE_TIMEOUT,
    APP_EVENT_PICKUP,
    APP_EVENT_ENTER_POWER_OFF,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    app_input_source_t source;
    uint32_t request_id;
    uint32_t written;
    uint32_t size;
    char state[32];
    char text[96];
} app_event_t;

static void update_battery_status(void);
static void send_current_battery_status(void);
static void queue_app_event(app_event_type_t type);
static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size);
static void queue_ui_state_event(const char *state, const char *text);
static void apply_interaction_mode(interaction_mode_t mode);
static void queue_primary_down_event(app_input_source_t source, uint32_t request_id);
static void queue_primary_up_event(app_input_source_t source, uint32_t request_id);
static void handle_primary_down(app_input_source_t source, uint32_t request_id);
static void handle_primary_up(app_input_source_t source, uint32_t request_id);

static bool is_external_powered(void)
{
    return s_battery_charging || s_usb_powered;
}

// S3 关机准入：允许 BLE 连接态关机（关机即断连），但录音/OTA/USB 供电时禁止。
// 阶段 2 走 deep sleep 路径 B，阶段 3 升级为 M5PM1 真关机（路径 A）。
static bool poweroff_allowed_now(void)
{
    return !s_recording && !s_ota_updating && !voice_ble_ota_is_active() &&
           !is_external_powered();
}

static void play_prompt_tone(uint32_t frequency_hz)
{
    if (!s_prompt_tone_enabled) {
        return;
    }
    esp_err_t err = audio_pipeline_play_tone(frequency_hz, 80, 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "prompt tone failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t init_power_management(void)
{
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording_cpu", &s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t acquire_recording_pm_locks(void)
{
    if (s_recording_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_recording_pm_locked = true;
    return ESP_OK;
}

static void release_recording_pm_locks(void)
{
    if (!s_recording_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_recording_pm_locked = false;
}

static esp_err_t acquire_ota_pm_locks(void)
{
    if (s_ota_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_ota_pm_locked = true;
    return ESP_OK;
}

static void release_ota_pm_locks(void)
{
    if (!s_ota_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_ota_pm_locked = false;
}

static void restart_display_dim_timer(void)
{
    if (!s_display_dim_timer) {
        return;
    }
    (void)esp_timer_stop(s_display_dim_timer);
    if (!s_recording && !s_ota_updating) {
        esp_err_t err = esp_timer_start_once(s_display_dim_timer, DISPLAY_DIM_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start dim timer failed: %s", esp_err_to_name(err));
        }
    }
}

// S1→S2 计时器：仅当已进入 Resting（dimmed）且无录音/OTA 时才推进。
static void restart_display_off_timer(void)
{
    if (!s_display_off_timer) {
        return;
    }
    (void)esp_timer_stop(s_display_off_timer);
    if (s_display_dimmed && !s_screen_off && !s_recording && !s_ota_updating) {
        esp_err_t err = esp_timer_start_once(s_display_off_timer, DISPLAY_OFF_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start display-off timer failed: %s", esp_err_to_name(err));
        }
    }
}

// S2→S3 关机计时器（原 deep_sleep_timer 改造，T_pwr=10min）。
// 允许 BLE 连接态关机；USB 供电/录音/OTA 时暂停。
static void restart_poweroff_timer(void)
{
    if (!s_poweroff_timer) {
        return;
    }
    (void)esp_timer_stop(s_poweroff_timer);
    if (poweroff_allowed_now()) {
        esp_err_t err = esp_timer_start_once(s_poweroff_timer, POWEROFF_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start poweroff timer failed: %s", esp_err_to_name(err));
        }
    } else if (is_external_powered()) {
        ESP_LOGD(TAG, "poweroff timer paused while external power is present");
    } else if (s_recording || s_ota_updating || voice_ble_ota_is_active()) {
        ESP_LOGD(TAG, "poweroff timer paused while recording or OTA is active");
    }
}

// 前向声明：拿起轮询定时器在 note_activity 之后定义，但 note_activity 需调用其启停接口。
static void set_pickup_polling_enabled(bool enabled);
// 前向声明：断连关机定时器在事件循环之后定义，但 BLE 连接/断连 case 需调用其启停接口。
static void start_disc_poweroff_timer(void);
static void cancel_disc_poweroff_timer(void);

// 活动复位钩子：按键/拿起/BLE事件/UI变化时调用，回到 S0 Active 态并重启全部空闲计时器。
static void note_activity(void)
{
    if (s_screen_off || s_display_dimmed) {
        // S2/S1 → S0：恢复正常背光。S2 因没关 L3B、panel 一直供电，PWM 恢复即亮屏。
        esp_err_t err = ui_status_set_brightness(DISPLAY_ACTIVE_BRIGHTNESS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore brightness failed: %s", esp_err_to_name(err));
        }
        s_screen_off = false;
        s_display_dimmed = false;
        ui_status_set_idle_dimmed(false);
    }
    // 回到 Active 态：停止拿起轮询，重启三级空闲计时器。
    set_pickup_polling_enabled(false);
    restart_display_dim_timer();
    restart_display_off_timer();
    restart_poweroff_timer();
}

static void stop_host_response_timer(void)
{
    if (s_host_response_timer) {
        (void)esp_timer_stop(s_host_response_timer);
    }
}

// S3 关机。阶段 2 走路径 B（deep sleep），阶段 3 升级为路径 A（M5PM1 真关机 + IMU 唤醒）。
// 允许 BLE 连接态关机（关机即断连）；USB 供电/录音/OTA 时跳过并重启计时器。
static void enter_power_off(void)
{
    if (s_recording || s_ota_updating || voice_ble_ota_is_active()) {
        restart_poweroff_timer();
        return;
    }

    if (is_external_powered()) {
        ESP_LOGI(TAG, "skip power off while charging or USB powered");
        restart_poweroff_timer();
        return;
    }

    bool charging = false;
    bool usb_powered = false;
    esp_err_t power_err = stick_s3_board_battery_charging(&charging);
    if (power_err == ESP_OK) {
        power_err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (power_err == ESP_OK && (charging || usb_powered)) {
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ESP_LOGI(TAG, "skip power off after fresh power check charging=%d usb=%d",
                 charging, usb_powered);
        restart_poweroff_timer();
        return;
    }

    const gpio_num_t wake_gpio = STICK_S3_PIN_BUTTON_FRONT;

    // 关机前收尾：关背光、停录音 PM 锁。
    release_recording_pm_locks();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_status_set_brightness(0));

    // 路径 A（M5PM1 真关机 + BMI270 拿起唤醒）暂禁用：关机位 0x0C=0xA1 与 PYG4 WAKE 配置
    // 在无法获取串口日志的情况下难以验证，且若关机未真正执行会污染后续 deep sleep 唤醒源。
    // 待串口日志恢复后单独调试启用。当前 S3 走路径 B（deep sleep），前键 ext1 唤醒。
#if 0
    if (bmi270_present()) {
        esp_err_t wake_err = bmi270_enable_pickup_wake();
        if (wake_err == ESP_OK) {
            ESP_LOGI(TAG, "power off via M5PM1 shutdown (path A), IMU pickup wake armed");
            stick_s3_board_power_off();
            ESP_LOGW(TAG, "M5PM1 shutdown did not power off, fallback to deep sleep");
        } else {
            ESP_LOGW(TAG, "pickup wake setup failed, fallback to deep sleep: %s",
                     esp_err_to_name(wake_err));
        }
    } else {
        ESP_LOGI(TAG, "BMI270 absent, power off via deep sleep (path B)");
    }
#endif

    // 路径 B：ESP32 deep sleep，前键 ext1 唤醒。
    ESP_LOGI(TAG, "entering power off (deep sleep path B), wake on front button GPIO%d",
             wake_gpio);
    ui_status_prepare_deep_sleep();
    stick_s3_board_prepare_deep_sleep();

    /* Clear any stale wakeup source bits left over from light sleep / esp_pm
       configuration (e.g. gpio_wakeup_enable on the PMIC IRQ line). Without
       this the chip can wake immediately from an unrelated trigger. */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Keep RTC peripherals powered so the internal pull-up on the wake pin
       remains effective in deep sleep; combined with the explicit RTC pull-up
       below this prevents GPIO%d from floating low and self-waking. */
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    (void)rtc_gpio_pulldown_dis(wake_gpio);
    (void)rtc_gpio_pullup_en(wake_gpio);

    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << wake_gpio,
                                                    ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        restart_poweroff_timer();
        return;
    }

    /* Wait for the wake pin to settle high (button release bounce, parasitic
       capacitance, etc.). If it stays low we would just wake up immediately
       after esp_deep_sleep_start(), so abort and retry later. */
    int wait_ms = 0;
    while (gpio_get_level(wake_gpio) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (gpio_get_level(wake_gpio) == 0) {
        ESP_LOGW(TAG, "front button still low after %d ms, abort power off", wait_ms);
        restart_poweroff_timer();
        return;
    }

    ESP_LOGI(TAG, "power off go (wait_ms=%d level=%d)", wait_ms,
             gpio_get_level(wake_gpio));
    esp_deep_sleep_start();
}

static bool app_ui_allows_recording_start(void)
{
    return s_app_ui_state != APP_UI_STATE_PENDING_CONFIRMATION;
}

static uint32_t start_recording(void)
{
    const bool ble_ready = voice_ble_is_ready();
    const bool ota_active = voice_ble_ota_is_active();
    const bool ui_allows_start = app_ui_allows_recording_start();
    if (s_recording || s_ota_updating || ota_active || !ble_ready || !ui_allows_start) {
        ESP_LOGW(TAG,
                 "start recording denied: recording=%d ota=%d ble_ota=%d ble_ready=%d ui_state=%d",
                 s_recording, s_ota_updating, ota_active, ble_ready, s_app_ui_state);
        return 0;
    }

    const uint32_t session_id = s_session_id++;
    play_prompt_tone(880);
    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm locks failed: %s", esp_err_to_name(err));
        ui_status_set_error("Power lock failed");
        return 0;
    }

    err = audio_pipeline_start(session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        release_recording_pm_locks();
        char message[64];
        snprintf(message, sizeof(message), "Audio %s: %s",
                 audio_pipeline_last_error_step(), esp_err_to_name(err));
        ui_status_set_error(message);
        return 0;
    }

    s_recording = true;
    s_app_ui_state = APP_UI_STATE_RECORDING;
    restart_display_dim_timer();
    restart_poweroff_timer();
    ui_status_set_recording(session_id);
    return session_id;
}

static uint32_t stop_recording(void)
{
    if (!s_recording) {
        return 0;
    }

    const uint32_t session_id = audio_pipeline_session_id();
    play_prompt_tone(440);
    s_recording = false;
    audio_pipeline_stop();
    release_recording_pm_locks();
    restart_display_dim_timer();
    restart_poweroff_timer();
    return session_id;
}

static void queue_app_event(app_event_type_t type)
{
    queue_app_event_with_ota(type, 0, 0);
}

static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .source = APP_INPUT_SOURCE_PHYSICAL,
            .request_id = 0,
            .written = written,
            .size = size,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_app_event_from_isr(app_event_type_t type, BaseType_t *high_task_woken)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .source = APP_INPUT_SOURCE_PHYSICAL,
            .request_id = 0,
            .written = 0,
            .size = 0,
        };
        (void)xQueueSendFromISR(s_app_event_queue, &event, high_task_woken);
    }
}

static void queue_primary_down_event(app_input_source_t source, uint32_t request_id)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = APP_EVENT_FRONT_DOWN,
            .source = source,
            .request_id = request_id,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_primary_up_event(app_input_source_t source, uint32_t request_id)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = APP_EVENT_FRONT_UP,
            .source = source,
            .request_id = request_id,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_ui_state_event(const char *state, const char *text)
{
    if (!s_app_event_queue) {
        ESP_LOGW(TAG, "drop ui_state state=%s text_len=%u: app queue unavailable",
                 state ? state : "nil",
                 (unsigned)(text ? strlen(text) : 0));
        return;
    }

    app_event_t event = {
        .type = APP_EVENT_UI_STATE,
    };
    if (state) {
        strlcpy(event.state, state, sizeof(event.state));
    }
    if (text) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    ESP_LOGI(TAG, "queue ui_state state=%s text_len=%u current=%s recording=%d",
             event.state[0] ? event.state : "nil",
             (unsigned)strlen(event.text),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop ui_state state=%s: app queue full",
                 event.state[0] ? event.state : "nil");
    }
}

static void front_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_primary_down_event(APP_INPUT_SOURCE_PHYSICAL, 0);
}

static void front_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_primary_up_event(APP_INPUT_SOURCE_PHYSICAL, 0);
}

static void side_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_SIDE_DOWN);
}

static void side_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_SIDE_UP);
}

static void ble_connection_cb(bool connected)
{
    queue_app_event(connected ? APP_EVENT_BLE_CONNECTED : APP_EVENT_BLE_DISCONNECTED);
}

static void ble_control_cb(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignore invalid control json");
        return;
    }

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *button = cJSON_GetObjectItemCaseSensitive(root, "button");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *request_id_json = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    uint32_t request_id = 0;
    if (cJSON_IsNumber(request_id_json)) {
        request_id = (uint32_t)request_id_json->valueint;
    }
    if (cJSON_IsString(event) && strcmp(event->valuestring, "ui_state") == 0 &&
        cJSON_IsString(state)) {
        queue_ui_state_event(state->valuestring, cJSON_IsString(text) ? text->valuestring : "");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "interaction_mode") == 0 &&
               cJSON_IsString(mode)) {
        if (strcmp(mode->valuestring, "click_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_CLICK_TO_TALK);
        } else if (strcmp(mode->valuestring, "hold_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_HOLD_TO_TALK);
        } else {
            ESP_LOGW(TAG, "unknown interaction_mode %s", mode->valuestring);
        }
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "prompt_tone") == 0 &&
               cJSON_IsBool(enabled)) {
        s_prompt_tone_enabled = cJSON_IsTrue(enabled);
        ESP_LOGI(TAG, "prompt tone %s", s_prompt_tone_enabled ? "enabled" : "disabled");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "battery_status_request") == 0) {
        queue_app_event(APP_EVENT_BATTERY_STATUS_REQUEST);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "remote_button_down") == 0 &&
               cJSON_IsString(button) && strcmp(button->valuestring, "primary") == 0) {
        ESP_LOGI(TAG, "remote primary down request_id=%" PRIu32, request_id);
        queue_primary_down_event(APP_INPUT_SOURCE_REMOTE, request_id);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "remote_button_up") == 0 &&
               cJSON_IsString(button) && strcmp(button->valuestring, "primary") == 0) {
        ESP_LOGI(TAG, "remote primary up request_id=%" PRIu32, request_id);
        queue_primary_up_event(APP_INPUT_SOURCE_REMOTE, request_id);
    }
    cJSON_Delete(root);
}

static uint32_t elapsed_button_ms(int64_t down_us)
{
    if (down_us <= 0) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - down_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}

static void handle_primary_down(app_input_source_t source, uint32_t request_id)
{
    (void)request_id;
    ESP_LOGI(TAG, "button front down source=%d", source);
    note_activity();

    if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK && s_recording) {
        const primary_owner_t owner_from_source = (source == APP_INPUT_SOURCE_PHYSICAL)
            ? PRIMARY_OWNER_PHYSICAL : PRIMARY_OWNER_REMOTE;
        if (s_primary_owner != PRIMARY_OWNER_NONE && s_primary_owner != owner_from_source) {
            ESP_LOGI(TAG, "ignore primary down from source=%d, owner is %d", source, s_primary_owner);
            return;
        }
    }

    if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording) {
        const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
        s_primary_session_id = stop_recording();
        esp_err_t primary_up_err = voice_ble_send_button_click("primary", primary_duration_ms,
                                                               s_primary_session_id);
        if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
            apply_app_ui_state("ready", "");
        }
        s_primary_down_us = 0;
        s_primary_session_id = 0;
        s_primary_owner = PRIMARY_OWNER_NONE;
    } else {
        s_primary_down_us = esp_timer_get_time();
        if (s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION) {
            ESP_LOGI(TAG, "button front down as pending confirmation control");
            s_primary_session_id = 0;
            (void)voice_ble_send_button_click("primary", 0, 0);
            return;
        }
        s_primary_session_id = start_recording();
        if (s_primary_session_id == 0) {
            s_primary_down_us = 0;
            return;
        }
        s_primary_owner = (source == APP_INPUT_SOURCE_PHYSICAL)
            ? PRIMARY_OWNER_PHYSICAL : PRIMARY_OWNER_REMOTE;
        esp_err_t primary_down_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
            ? voice_ble_send_button_click("primary", 0, s_primary_session_id)
            : voice_ble_send_button_down("primary", s_primary_session_id);
        if (s_primary_session_id != 0 && primary_down_err != ESP_OK) {
            (void)stop_recording();
            s_primary_session_id = 0;
            s_primary_owner = PRIMARY_OWNER_NONE;
            apply_app_ui_state("ready", "");
        }
    }
}

static void handle_primary_up(app_input_source_t source, uint32_t request_id)
{
    (void)request_id;
    ESP_LOGI(TAG, "button front up source=%d", source);
    note_activity();
    if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK) {
        return;
    }

    const primary_owner_t owner_from_source = (source == APP_INPUT_SOURCE_PHYSICAL)
        ? PRIMARY_OWNER_PHYSICAL : PRIMARY_OWNER_REMOTE;
    if (s_primary_owner != PRIMARY_OWNER_NONE && s_primary_owner != owner_from_source) {
        ESP_LOGI(TAG, "ignore primary up from source=%d, owner is %d", source, s_primary_owner);
        return;
    }

    if (!s_recording && s_primary_session_id == 0 && s_primary_down_us == 0) {
        return;
    }
    const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
    if (s_recording) {
        s_primary_session_id = stop_recording();
    }
    esp_err_t primary_up_err = voice_ble_send_button_up("primary", primary_duration_ms,
                                                        s_primary_session_id);
    if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
        apply_app_ui_state("ready", "");
    }
    s_primary_down_us = 0;
    s_primary_session_id = 0;
    s_primary_owner = PRIMARY_OWNER_NONE;
}

static void apply_app_ui_state(const char *state, const char *text)
{
    ESP_LOGI(TAG, "apply ui_state state=%s text_len=%u current=%s recording=%d",
             state ? state : "nil",
             (unsigned)(text ? strlen(text) : 0),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    stop_host_response_timer();
    if (strcmp(state, "ready") == 0) {
        if (s_recording) {
            ESP_LOGI(TAG, "ignore ready ui_state while recording");
            return;
        }
        s_app_ui_state = APP_UI_STATE_READY;
        ui_status_set_idle();
        note_activity();
        voice_ble_request_slow_interval();
    } else if (strcmp(state, "recording") == 0) {
        s_app_ui_state = APP_UI_STATE_RECORDING;
        if (!s_recording) {
            ui_status_set_recording(0);
        }
    } else if (strcmp(state, "thinking") == 0) {
        s_app_ui_state = APP_UI_STATE_THINKING;
        ui_status_set_partial_text("");
    } else if (strcmp(state, "pending_confirmation") == 0) {
        s_app_ui_state = APP_UI_STATE_PENDING_CONFIRMATION;
        ui_status_set_partial_text("Confirm or cancel");
    } else if (strcmp(state, "error") == 0) {
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error(text && text[0] ? text : "Unknown error");
    } else {
        ESP_LOGW(TAG, "unknown ui_state %s", state);
    }
    ESP_LOGI(TAG, "applied ui_state current=%s recording=%d",
             app_ui_state_name(s_app_ui_state),
             s_recording);
}

static void apply_interaction_mode(interaction_mode_t mode)
{
    s_interaction_mode = mode;
    ui_status_set_idle_hint(mode == INTERACTION_MODE_CLICK_TO_TALK ? "Click to Talk" : "Hold to Talk");
    if (s_app_ui_state == APP_UI_STATE_READY && !s_recording) {
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "interaction mode %s",
             mode == INTERACTION_MODE_CLICK_TO_TALK ? "click_to_talk" : "hold_to_talk");
}

static void ble_ota_cb(voice_ble_ota_event_t event, uint32_t written, uint32_t size)
{
    switch (event) {
    case VOICE_BLE_OTA_EVENT_BEGIN:
        queue_app_event_with_ota(APP_EVENT_OTA_BEGIN, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_PROGRESS:
        queue_app_event_with_ota(APP_EVENT_OTA_PROGRESS, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_DONE:
        queue_app_event_with_ota(APP_EVENT_OTA_DONE, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_ERROR:
    case VOICE_BLE_OTA_EVENT_ABORT:
        queue_app_event(APP_EVENT_OTA_END);
        break;
    }
}

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case APP_EVENT_FRONT_DOWN:
            handle_primary_down(event.source, event.request_id);
            break;
        case APP_EVENT_FRONT_UP:
            handle_primary_up(event.source, event.request_id);
            break;
        case APP_EVENT_SIDE_DOWN:
            ESP_LOGI(TAG, "button side down");
            note_activity();
            s_secondary_down_us = esp_timer_get_time();
            break;
        case APP_EVENT_SIDE_UP:
            ESP_LOGI(TAG, "button side up");
            note_activity();
            voice_ble_send_button_click("secondary", elapsed_button_ms(s_secondary_down_us), 0);
            s_secondary_down_us = 0;
            break;
        case APP_EVENT_UI_STATE:
            apply_app_ui_state(event.state, event.text);
            break;
        case APP_EVENT_BLE_CONNECTED:
            s_app_ui_state = APP_UI_STATE_READY;
            ui_status_set_idle();
            cancel_disc_poweroff_timer();
            note_activity();
            send_current_battery_status();
            break;
        case APP_EVENT_BLE_DISCONNECTED:
            s_recording = false;
            s_ota_updating = false;
            s_app_ui_state = APP_UI_STATE_READY;
            s_primary_owner = PRIMARY_OWNER_NONE;
            s_primary_down_us = 0;
            s_primary_session_id = 0;
            stop_host_response_timer();
            audio_pipeline_stop();
            release_recording_pm_locks();
            release_ota_pm_locks();
            ui_status_set_pairing(voice_ble_device_name());
            // 断连后改用 T_disc 倒计时关机（替代连接态 T_pwr）；重连则取消。
            (void)esp_timer_stop(s_poweroff_timer);
            restart_display_dim_timer();
            start_disc_poweroff_timer();
            break;
        case APP_EVENT_POWER_IRQ:
            gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
            /* fall through */
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            break;
        case APP_EVENT_BATTERY_STATUS_REQUEST:
            update_battery_status();
            send_current_battery_status();
            break;
        case APP_EVENT_ENTER_POWER_OFF:
            enter_power_off();
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            if (s_recording) {
                const uint32_t session_id = stop_recording();
                voice_ble_send_button_up("primary", elapsed_button_ms(s_primary_down_us),
                                         session_id);
            }
            esp_err_t ota_pm_err = acquire_ota_pm_locks();
            if (ota_pm_err != ESP_OK) {
                ESP_LOGW(TAG, "acquire OTA pm lock failed: %s", esp_err_to_name(ota_pm_err));
            }
            note_activity();
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_PROGRESS:
            s_ota_updating = true;
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_DONE:
            ui_status_set_ota_rebooting();
            break;
        case APP_EVENT_OTA_END:
            s_ota_updating = false;
            release_ota_pm_locks();
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            ui_status_set_idle();
            note_activity();
            break;
        case APP_EVENT_HOST_RESPONSE_TIMEOUT:
            if (!s_recording && (s_app_ui_state == APP_UI_STATE_RECORDING ||
                                 s_app_ui_state == APP_UI_STATE_THINKING)) {
                ESP_LOGW(TAG, "host response timeout, returning to ready");
                apply_app_ui_state("ready", "");
            }
            break;
        case APP_EVENT_PICKUP:
            // 拿起唤醒：仅在 Resting 态有意义，亮屏回 Active。
            // note_activity 会停止轮询并重置计时器。
            if (s_display_dimmed) {
                ESP_LOGI(TAG, "pickup detected, waking display");
                note_activity();
            }
            break;
        }
    }
}

static esp_err_t init_gpio_button(gpio_num_t gpio_num, button_handle_t *button)
{
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = 0,
        .enable_power_save = true
    };

    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(12, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_gpio_button(STICK_S3_PIN_BUTTON_FRONT, &s_front_button);
    if (err != ESP_OK) {
        return err;
    }
    err = init_gpio_button(STICK_S3_PIN_BUTTON_SIDE, &s_side_button);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_DOWN, NULL,
                                 front_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_UP, NULL,
                                 front_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_side_button, BUTTON_PRESS_DOWN, NULL,
                                 side_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_side_button, BUTTON_PRESS_UP, NULL,
                                 side_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(app_event_task, "app_event_task", 4096,
                                NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_dim_timer_cb(void *arg)
{
    (void)arg;

    if (!s_display_dimmed && !s_recording && !s_ota_updating) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_DIM_BRIGHTNESS);
        if (err == ESP_OK) {
            s_display_dimmed = true;
            ui_status_set_idle_dimmed(true);
            // 进入 S1 Resting：启动拿起轮询 + S1→S2 与 S2→S3 计时器。
            set_pickup_polling_enabled(true);
            restart_display_off_timer();
            restart_poweroff_timer();
            ESP_LOGI(TAG, "display dimmed after inactivity (S1)");
        } else {
            ESP_LOGW(TAG, "dim display failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t init_display_dim_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_dim_timer_cb,
        .name = "display_dim",
    };
    return esp_timer_create(&timer_args, &s_display_dim_timer);
}

static void poweroff_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_ENTER_POWER_OFF);
}

static void display_off_timer_cb(void *arg)
{
    (void)arg;
    // 仅在 S1(Resting) 态推进到 S2。S2 只把背光 PWM 设为 0，不关 L3B 层电源。
    // 关 L3B 会让 LCD panel 也掉电，唤醒后 panel 状态丢失需要重新 init 才能显示
    // （PWM 即使恢复 duty=32 也只是背光通电、panel 仍黑屏）。
    // 背光 PWM duty=0 时 LED 不通电，省电等价于关 L3B 的背光部分；MIC/SPK 在 S2 不工作
    // 时本就不耗电，因此 L3B 实际省电收益有限。BLE 与 panel 持续供电换取唤醒即亮屏。
    if (s_display_dimmed && !s_screen_off && !s_recording && !s_ota_updating) {
        (void)ui_status_set_brightness(0);
        s_screen_off = true;
        ESP_LOGI(TAG, "display off after inactivity (S2), BLE & panel kept");
    }
}

static void host_response_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HOST_RESPONSE_TIMEOUT);
}

static esp_err_t init_poweroff_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = poweroff_timer_cb,
        .name = "poweroff",
    };
    return esp_timer_create(&timer_args, &s_poweroff_timer);
}

// BLE 断连关机：断连后 T_disc 内若未重连则关机。USB 供电时不启动。
static void disc_poweroff_timer_cb(void *arg)
{
    (void)arg;
    // 二次确认：重连后取消，或 USB 供电则放弃。
    if (voice_ble_is_connected() || is_external_powered()) {
        return;
    }
    queue_app_event(APP_EVENT_ENTER_POWER_OFF);
}

static esp_err_t init_disc_poweroff_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = disc_poweroff_timer_cb,
        .name = "disc_poweroff",
    };
    return esp_timer_create(&timer_args, &s_disc_poweroff_timer);
}

// BLE 断连时启动 T_disc 倒计时（USB 供电时不启动，避免充电中关机）。
static void start_disc_poweroff_timer(void)
{
    if (!s_disc_poweroff_timer) {
        return;
    }
    (void)esp_timer_stop(s_disc_poweroff_timer);
    if (!is_external_powered()) {
        esp_err_t err = esp_timer_start_once(s_disc_poweroff_timer, DISC_POWEROFF_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start disc poweroff timer failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "BLE disconnected, will power off in %d min if not reconnected",
                     DISC_POWEROFF_TIMEOUT_MS / 60000);
        }
    } else {
        ESP_LOGD(TAG, "disc poweroff timer skipped while external power present");
    }
}

static void cancel_disc_poweroff_timer(void)
{
    if (s_disc_poweroff_timer && esp_timer_is_active(s_disc_poweroff_timer)) {
        (void)esp_timer_stop(s_disc_poweroff_timer);
        ESP_LOGI(TAG, "BLE reconnected, disc poweroff timer cancelled");
    }
}

static esp_err_t init_display_off_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_off_timer_cb,
        .name = "display_off",
    };
    return esp_timer_create(&timer_args, &s_display_off_timer);
}

static esp_err_t init_host_response_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = host_response_timer_cb,
        .name = "host_response",
    };
    return esp_timer_create(&timer_args, &s_host_response_timer);
}

static void battery_refresh_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_BATTERY_REFRESH);
}

static esp_err_t init_battery_refresh_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = battery_refresh_timer_cb,
        .name = "battery_refresh",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_battery_refresh_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_battery_refresh_timer, BATTERY_REFRESH_FALLBACK_US);
}

// 拿起检测轮询：仅在 S1(Resting) 态启用。
// 进入 Resting 时启动，回到 Active 或进入录音/OTA 时停止，避免无谓的 I2C 读与功耗。
// IMU 不在线时定时器仍启动但 bmi270_pickup_detected 返回 false，轮询空转无开销。
static void set_pickup_polling_enabled(bool enabled)
{
    if (!s_pickup_poll_timer) {
        return;
    }
    if (enabled) {
        if (!esp_timer_is_active(s_pickup_poll_timer)) {
            esp_err_t err = esp_timer_start_periodic(s_pickup_poll_timer, PICKUP_POLL_INTERVAL_US);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "start pickup poll failed: %s", esp_err_to_name(err));
            }
        }
    } else {
        (void)esp_timer_stop(s_pickup_poll_timer);
    }
}

static void pickup_poll_timer_cb(void *arg)
{
    (void)arg;
    // 仅在 Resting（dimmed 且非录音/OTA）态判定拿起，其余态直接忽略避免误触。
    if (!s_display_dimmed || s_recording || s_ota_updating) {
        return;
    }
    if (bmi270_pickup_detected()) {
        queue_app_event(APP_EVENT_PICKUP);
    }
}

static esp_err_t init_pickup_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = pickup_poll_timer_cb,
        .name = "pickup_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_pickup_poll_timer);
}

static void IRAM_ATTR pmic_irq_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(STICK_S3_PIN_PMIC_IRQ);

    BaseType_t high_task_woken = pdFALSE;
    queue_app_event_from_isr(APP_EVENT_POWER_IRQ, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_pmic_irq(void)
{
    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << STICK_S3_PIN_PMIC_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&irq_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_wakeup_enable(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(STICK_S3_PIN_PMIC_IRQ, pmic_irq_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_intr_type(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static void update_battery_status(void)
{
    uint8_t sys_status = 0;
    esp_err_t irq_err = stick_s3_board_clear_power_irqs(&sys_status);
    if (irq_err == ESP_OK && sys_status) {
        ESP_LOGI(TAG, "PMIC sys irq=0x%02x", sys_status);
    }

    int level = 0;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t err = stick_s3_board_battery_level(&level);
    if (err == ESP_OK) {
        err = stick_s3_board_battery_charging(&charging);
    }
    if (err == ESP_OK) {
        err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (err == ESP_OK) {
        const bool external_power_changed = (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        const bool level_changed = (level != s_battery_level);
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        s_battery_level = level;
        ui_status_set_battery(level, charging, usb_powered);
        if (voice_ble_is_connected() && (external_power_changed || level_changed)) {
            voice_ble_send_battery_status(level, charging, usb_powered);
        }
        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d",
                     charging, usb_powered);
            restart_poweroff_timer();
        }
    } else {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }
}

static void send_current_battery_status(void)
{
    if (!voice_ble_is_connected()) {
        return;
    }
    voice_ble_send_battery_status(s_battery_level, s_battery_charging, s_usb_powered);
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset_reason=%d wakeup_cause=%d ext1_status=0x%llx",
             esp_reset_reason(), esp_sleep_get_wakeup_cause(),
             (unsigned long long)esp_sleep_get_ext1_wakeup_status());

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(stick_s3_board_init());
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_display_off_timer());
    ESP_ERROR_CHECK(init_poweroff_timer());
    ESP_ERROR_CHECK(init_disc_poweroff_timer());
    ESP_ERROR_CHECK(init_host_response_timer());
    ESP_ERROR_CHECK(init_pickup_poll_timer());
    // BMI270 初始化在 I2C 总线就绪后；探测失败时优雅降级，不影响主流程。
    (void)bmi270_init();
    note_activity();
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_control_callback(ble_control_cb);
    voice_ble_set_ota_callback(ble_ota_cb);
    ESP_ERROR_CHECK(init_buttons());

    esp_err_t err = voice_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        ui_status_set_error("BLE init failed");
    } else {
        ui_status_set_device_name(voice_ble_device_name());
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Audio init failed");
    }

    if (err == ESP_OK) {
        ui_status_set_pairing(voice_ble_device_name());
    }
    ESP_LOGI(TAG, "Voice Stick booted");

    update_battery_status();
    ESP_ERROR_CHECK(init_battery_refresh_timer());
    ESP_ERROR_CHECK(init_pmic_irq());

    ESP_LOGI(TAG, "configuring PMIC");
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        // 把 min_freq 设为与 max_freq 相同（即禁用 CPU 降频）：
        // ESP32-S3 原生 USB-Serial-JTAG 需要稳定的 PLL 时钟产生 USB 48MHz，
        // CPU 降到 40MHz (XTAL) 时 PLL 可能被关，导致 USB 设备不可用（COM 端口出现 PermissionError 13）。
        // 同时关闭自动 light sleep：USB-Serial-JTAG 控制台下 light sleep 会让 USB 挂起、
        // CONFIG_PM_SLP_DISABLE_GPIO=y 让按键 gpio_wakeup 失效。
        // S0/S1/S2 省电靠：背光分级 + L3B 开关 + BLE modem sleep；S3 deep sleep 才是深度省电。
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_config);
}
