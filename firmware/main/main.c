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
#include "mini_encoder_c.h"
#include "stick_s3_board.h"
#include "ui_status.h"
#include "voice_ble.h"
#include "nvs.h"
#include "esp_ota_ops.h"  // OTA rollback 签到：esp_ota_mark_app_valid_cancel_rollback

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_MS (10 * 1000)
#define DISPLAY_DIM_TIMEOUT_MS (10 * 1000)
#define DISPLAY_OFF_TIMEOUT_MS (20 * 1000)      // T_off：S1 Resting → S2 ScreenOff
#define POWEROFF_TIMEOUT_MS (5 * 60 * 1000)    // T_pwr：S2 ScreenOff → S3 PowerOff（连接态）
#define DISC_POWEROFF_TIMEOUT_MS (10 * 60 * 1000)  // T_disc：BLE 断连 → S3 PowerOff
#define DISPLAY_ACTIVE_BRIGHTNESS 20
#define DISPLAY_DIM_BRIGHTNESS 4
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define DISPLAY_OFF_TIMEOUT_US (DISPLAY_OFF_TIMEOUT_MS * 1000ULL)
#define POWEROFF_TIMEOUT_US (POWEROFF_TIMEOUT_MS * 1000ULL)
#define DISC_POWEROFF_TIMEOUT_US (DISC_POWEROFF_TIMEOUT_MS * 1000ULL)
#define BATTERY_REFRESH_FALLBACK_US (BATTERY_REFRESH_FALLBACK_MS * 1000ULL)
// BMI270 拿起轮询周期。仅在 S1(Resting)/S2(ScreenOff) 态启用：用户放下设备后拿起即亮屏回 S0。
// 周期过长会丢快速拿起动作，过短增加功耗；500ms 兼顾两者。BMI270 不在线时轮询空转无开销。
#define PICKUP_POLL_INTERVAL_MS (500)
#define PICKUP_POLL_INTERVAL_US (PICKUP_POLL_INTERVAL_MS * 1000ULL)

// 主按键双击检测参数。按住超过 HOLD_THRESHOLD 视为正常录音（长按）；短于此时间的按压
// 可能为双击的第一击。释放后在 WINDOW 时间内再次按下，确认双击并发送 Enter。
#define DOUBLE_CLICK_MAX_PRESS_MS 300
#define DOUBLE_CLICK_WINDOW_MS 500
// click_to_talk 首击后等双击窗口确认启动（双击回车语义 c）。窗口内第二次 click 发
// button_double_click（不启动录音，桌面端直接回车）；超时确认启动。代价：单击启动延迟此时长。
#define CLICK_TO_TALK_START_DELAY_MS 300

// hold_to_talk 在 BLE 连接就绪过渡期被拒时的录音启动重试参数。
// 设备重连后 Windows 需重新做服务发现 + 特征值订阅才能让 ble_ready 置位（约 1.5–2s），
// 在此窗口内按住按钮触发 hold threshold，start_recording 会因 ble_ready=0 被拒。
// 按住期间按短间隔重试，覆盖订阅过渡期；超时或松开则干净放弃。
#define RECORDING_RETRY_INTERVAL_MS 100   // 重试间隔
#define RECORDING_RETRY_WINDOW_MS   2000  // 重试总窗口（覆盖订阅过渡期 ~1.5s + 余量）

// IMU X 轴加速度上屏轮询周期。200ms 人眼可读、I²C 负载低；IMU 走 I²C 与录音 I²S 不同总线，
// 故常驻运行不随状态机开关。BMI270 不在线时仅刷一次 "IMU: n/a"。
#define IMU_POLL_INTERVAL_US (200 * 1000ULL)
// 敲击检测轮询周期。10ms=100Hz，覆盖 ~10-50ms 宽的敲击脉冲；录音/识别态暂停以降功耗。
#define TAP_POLL_INTERVAL_US (10 * 1000ULL)
// 体感鼠标轮询周期。20ms=50Hz，光标移动足够流畅且 BLE/I²C 负载低。仅体感态运行。
#define AIR_MOUSE_POLL_INTERVAL_US (20 * 1000ULL)
// 编码器轮询周期。10ms=100Hz，与敲击轮询一致；按钮边沿与旋转增量都经此轮询采集。
// 仅 MiniEncoderC 在线时运行；连续 I2C 失败后组件标记 absent，回调内停表。
#define ENCODER_POLL_INTERVAL_US (10 * 1000ULL)
// 按键事件后抑制敲击检测的窗口：覆盖"按下→录音启动"（hold_to_talk 300ms hold + 80ms 提示音
// + codec 初始化）及松开后手指余震。该窗口内 tap 轮询直接 return，避免按语音键的手指动作
// 被 IMU 误判为双击。仅作用于非录音态（录音态本就门控关闭 tap）。
#define TAP_SUPPRESS_AFTER_BUTTON_MS 600

// 基于 IMU X 轴的显示方向自动旋转。
// 当前握持方向 X 为正时画面不变；旋转 180° 后 X 为负，画面也旋转 180°。
#define ORIENTATION_THRESHOLD_G      0.5f
#define ORIENTATION_CONFIRM_COUNT    2

static bool s_recording;
static bool s_ota_updating;
static bool s_display_dimmed;   // S1 Resting：背光降到 8
static bool s_screen_off;       // S2 ScreenOff：背光 0 + L3B 关，BLE 保连
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;
static bool s_battery_charging;
static bool s_usb_powered;
static int s_battery_level = 0;
static bool s_show_imu_debug = false;
static bool s_tap_enabled = true;
// 按键事件后的敲击检测抑制截止时间戳（us）。在此之前 tap_poll_timer_cb 直接 return。
// 仅在 handle_primary_down/up（app_event 任务）与 tap_poll_timer_cb（timer 任务）读写，
// 均为任务上下文非 ISR，单 64 位读写在 ESP32-S3 上可接受，无需锁。
static int64_t s_tap_suppress_until_us = 0;
static esp_pm_lock_handle_t s_cpu_freq_lock;
static esp_timer_handle_t s_display_dim_timer;
static esp_timer_handle_t s_display_off_timer;   // S1→S2
static esp_timer_handle_t s_poweroff_timer;      // S2→S3（原 deep_sleep_timer 改造）
static esp_timer_handle_t s_disc_poweroff_timer; // BLE 断连→S3
static esp_timer_handle_t s_battery_refresh_timer;
static esp_timer_handle_t s_host_response_timer;
static esp_timer_handle_t s_pickup_poll_timer;
static esp_timer_handle_t s_imu_poll_timer;
static esp_timer_handle_t s_tap_poll_timer;
static esp_timer_handle_t s_air_mouse_poll_timer;
static esp_timer_handle_t s_encoder_poll_timer;
static bool s_encoder_button_pressed;
// MiniEncoderC 每格（detent）产生 2 个正交计数（真机验证）：跨轮询窗口累计计数，
// 每满 2 个同向计数上报 1 步；方向反转时丢弃反向余数。取值为 [-1,1] 的余数。
static int32_t s_encoder_count_rem;
static bool s_air_mouse_enabled = false;
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static button_handle_t s_front_button;
static button_handle_t s_side_button;
static int64_t s_primary_down_us;
static int64_t s_secondary_down_us;
static uint32_t s_primary_session_id;
static bool s_double_click_pending;          // 等待第二次按下（双击窗口内）
static bool s_double_click_second_press;     // 第二次按下进行中，忽略其释放
static bool s_hold_threshold_pending;        // 按住阈值计时中（300ms 后确认为长按）
static bool s_recording_retry_pending;       // ble_ready 未就绪，按住等待录音启动重试中
static int64_t s_recording_retry_deadline_us;  // 录音启动重试放弃时刻
static esp_timer_handle_t s_double_click_timer;
static uint32_t s_pending_button_up_duration_ms; // 暂存第一次短按的时长（窗口超时后补发 button_up）
static int64_t s_click_to_talk_first_click_us;  // click_to_talk 模式首次点击时刻（用于双击检测）
static bool s_click_to_talk_pending_start;      // click_to_talk 首击后等双击窗口确认启动
// 侧键双击检测：与主键独立。单击延迟到双击窗口超时后确认为 button_click；窗口内第二击发
// button_double_click。用于桌面端把侧键单击/双击映射到不同语义（进体感 vs 恢复上次输入）。
static bool s_side_click_pending;                // 侧键等待第二击（双击窗口内）
static uint32_t s_side_pending_duration_ms;      // 暂存第一次侧键短按时长（窗口超时后补发 button_click）
static esp_timer_handle_t s_side_double_click_timer;

typedef enum {
    DISPLAY_ORIENTATION_NORMAL = 0,
    DISPLAY_ORIENTATION_UPSIDE_DOWN = 1,
} display_orientation_t;

static display_orientation_t s_display_orientation = DISPLAY_ORIENTATION_NORMAL;
static int s_orientation_confirm_count = 0;

typedef enum {
    APP_INPUT_SOURCE_PHYSICAL,
    APP_INPUT_SOURCE_REMOTE,
    // MiniEncoderC 编码器按钮：交互语义与 PHYSICAL 完全相同（双击、hold 阈值、
    // click_to_talk、体感映射），仅日志里用 source 值区分来源；owner 仲裁独立
    // （PRIMARY_OWNER_ENCODER），避免与物理键互相截断录音。
    APP_INPUT_SOURCE_ENCODER,
} app_input_source_t;

static void apply_app_ui_state(const char *state, const char *text);

typedef enum {
    PRIMARY_OWNER_NONE,
    PRIMARY_OWNER_PHYSICAL,
    PRIMARY_OWNER_REMOTE,
    // 编码器按钮：手势语义同 PHYSICAL（见 is_local_primary_source），
    // 但 owner 仲裁独立，避免两个本地源互相截断对方的录音。
    PRIMARY_OWNER_ENCODER,
} primary_owner_t;

static primary_owner_t s_primary_owner = PRIMARY_OWNER_NONE;

// 最近一次主键按下（任意来源）的输入源：button_up/click/double_click 发送时据此
// 补 source 标签（编码器事件带 "encoder"，其它来源省略）。
// timer（double_click_timer_cb）/app_event 双任务访问，与 s_primary_owner 同模式可接受。
static app_input_source_t s_primary_press_source = APP_INPUT_SOURCE_PHYSICAL;

// 编码器录音灯颜色（0xRRGGBB，0=off）：桌面端经 control_rx 下发颜色名，NVS 持久化。
static uint32_t s_encoder_led_rgb = 0xFF0000u;  // 默认红

// 编码器录音门控：false 时编码器按下只发按键事件不启动录音（桌面端单击=自定义按键）。
static bool s_encoder_recording_gate = true;

typedef enum {
    APP_UI_STATE_READY,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_PENDING_CONFIRMATION,
    APP_UI_STATE_ERROR,
    APP_UI_STATE_AIR_MOUSE,
} app_ui_state_t;

static app_ui_state_t s_app_ui_state = APP_UI_STATE_READY;

typedef enum {
    INTERACTION_MODE_HOLD_TO_TALK,
    INTERACTION_MODE_HOLD_TO_TALK_INSTANT,  // wechat 模式：按下即录音，跳过 300ms hold 阈值
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
    case APP_UI_STATE_AIR_MOUSE:
        return "air_mouse";
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
    APP_EVENT_TAP,
    APP_EVENT_ENCODER_ROTATE,
    APP_EVENT_ENTER_POWER_OFF,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    app_input_source_t source;
    uint32_t request_id;
    uint32_t written;
    uint32_t size;
    // 编码器旋转事件 payload：direction 0=cw / 1=ccw（原始物理方向）；
    // steps 为该轮询窗口内同向累计格数（>=1）。仅 APP_EVENT_ENCODER_ROTATE 使用。
    uint8_t encoder_direction;
    uint8_t encoder_steps;
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
static void queue_encoder_rotate_event(int32_t delta);
static void handle_primary_down(app_input_source_t source, uint32_t request_id);
static void handle_primary_up(app_input_source_t source, uint32_t request_id);
static void load_pickup_threshold_from_nvs(void);
static void save_pickup_threshold_to_nvs(int32_t threshold);
static void load_tap_settings_from_nvs(void);
static void save_tap_settings_to_nvs(bool enabled, int32_t sensitivity);
static void load_encoder_settings_from_nvs(void);
static void save_encoder_settings_to_nvs(void);
static void set_tap_polling_enabled(bool enabled);
static void set_air_mouse_enabled(bool enabled);

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

// 主键当前是否处于按住态：正面物理键（GPIO 低电平）或编码器按钮任一按下即视为按住。
// app 任务上下文（如关机前按住检查）用此活读判定；编码器 absent 时退化为纯 GPIO 判定。
// esp_timer 上下文（双击/hold 阈值定时器）改用 primary_button_held_from_timer()。
static bool primary_button_held(void)
{
    if (gpio_get_level(STICK_S3_PIN_BUTTON_FRONT) == 0) {
        return true;
    }
    if (mini_encoder_c_present()) {
        bool pressed = false;
        if (mini_encoder_c_read_button(&pressed) == ESP_OK && pressed) {
            return true;
        }
    }
    return false;
}

// esp_timer 上下文专用：编码器按钮态复用轮询缓存（同一任务，无竞态），
// 避免再做一次带超时的 I2C 活读——瞬时 I2C 失败会误判"已松开"。
static bool primary_button_held_from_timer(void)
{
    return gpio_get_level(STICK_S3_PIN_BUTTON_FRONT) == 0 || s_encoder_button_pressed;
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

    /* 关机前优雅断开 BLE：直接 deep sleep 会让链路无声消失，对端只能靠
       supervision timeout 发现（WinRT 对静默消失的空闲连接甚至可能不投递
       断连事件，主机侧留存"已连接"僵尸会话，设备唤醒重播后无法回连，
       卡 pairing 屏）。先 terminate 让主机立刻收到断连事件再睡。超时也
       继续睡，不阻塞关机。 */
    if (voice_ble_is_connected()) {
        (void)voice_ble_disconnect(1000);
    }

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
    while (primary_button_held() && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (primary_button_held()) {
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
    // 提前请求 fast conn interval：conn update 是异步过程，需等 central 同意，
    // 耗时可达数百毫秒。若等到 audio_pipeline_start 内部才请求，录音前半段
    // 仍跑在 slow interval，链路吞吐不足导致 mbuf 堆积丢帧、ASR 流开头缺口、
    // 识别到输入卡顿。这里在提示音与 audio 初始化期间并行启动 conn update，
    // 等真正产帧时 interval 多半已切到 7.5ms。
    voice_ble_request_fast_interval();
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
    // 录音期间编码器 LED 亮灯（颜色 NVS 可配，0=off 不亮）；LED 写失败静默忽略。
    if (mini_encoder_c_present() && s_encoder_led_rgb != 0) {
        (void)mini_encoder_c_set_led((s_encoder_led_rgb >> 16) & 0xFF,
                                     (s_encoder_led_rgb >> 8) & 0xFF,
                                     s_encoder_led_rgb & 0xFF);
    }
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
    s_recording = false;
    audio_pipeline_stop();
    // audio_pipeline_stop 同步等 drain 完成才返回，此处即录音会话真正结束点，灭灯。
    if (mini_encoder_c_present()) {
        (void)mini_encoder_c_set_led(0, 0, 0);
    }
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

// 旋转步数入队：入参已按 2 计数=1 格折算为格数（见 encoder_poll_timer_cb）。
// 方向在窗口内反转的极端情况按净值方向处理（真机罕见，可接受）。
// steps 截断到 uint8_t 上限 255。
static void queue_encoder_rotate_event(int32_t delta)
{
    if (s_app_event_queue && delta != 0) {
        app_event_t event = {
            .type = APP_EVENT_ENCODER_ROTATE,
            .source = APP_INPUT_SOURCE_ENCODER,
            .encoder_direction = (delta > 0) ? 0 : 1,
            .encoder_steps = (delta > 0)
                ? (delta > 255 ? 255 : (uint8_t)delta)
                : (delta < -255 ? 255 : (uint8_t)(-delta)),
        };
        // 低频异常路径观测：队列满时告警（正常路径保持静默，见 case 处说明）。
        if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "encoder rotate event dropped, queue full");
        }
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

// 颜色名 → 0xRRGGBB 预设表。未知名返回 false（调用方忽略并保持当前值）。
static bool encoder_led_rgb_from_name(const char *name, uint32_t *rgb_out)
{
    struct { const char *name; uint32_t rgb; } presets[] = {
        {"red", 0xFF0000u}, {"green", 0x00FF00u}, {"blue", 0x0000FFu},
        {"yellow", 0xFFFF00u}, {"purple", 0xFF00FFu}, {"cyan", 0x00FFFFu},
        {"white", 0xFFFFFFu}, {"off", 0x000000u},
    };
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        if (strcmp(name, presets[i].name) == 0) {
            *rgb_out = presets[i].rgb;
            return true;
        }
    }
    return false;
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
    const cJSON *threshold_item = cJSON_GetObjectItemCaseSensitive(root, "threshold");
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
        } else if (strcmp(mode->valuestring, "hold_to_talk_instant") == 0) {
            apply_interaction_mode(INTERACTION_MODE_HOLD_TO_TALK_INSTANT);
        } else {
            ESP_LOGW(TAG, "unknown interaction_mode %s", mode->valuestring);
        }
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "show_imu_debug") == 0 &&
               cJSON_IsBool(enabled)) {
        s_show_imu_debug = cJSON_IsTrue(enabled);
        ESP_LOGI(TAG, "show_imu_debug %s", s_show_imu_debug ? "enabled" : "disabled");
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
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "ota_commit") == 0) {
        // 桌面端手动确认新固件健康：直接签到 mark_app_valid_cancel_rollback。
        // 正常情况下 boot 时已无条件自动签到，此命令作为手动兜底。
        ESP_LOGI(TAG, "ota_commit");
        esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
        if (mark_err != ESP_OK && mark_err != ESP_ERR_NOT_SUPPORTED &&
            mark_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "ota_commit mark_valid failed: %s", esp_err_to_name(mark_err));
        }
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "imu_wake_sensitivity") == 0 &&
               cJSON_IsNumber(threshold_item)) {
        double threshold_raw = threshold_item->valuedouble;
        if (threshold_raw < BMI270_PICKUP_THRESHOLD_MIN_LSB) {
            threshold_raw = BMI270_PICKUP_THRESHOLD_MIN_LSB;
        } else if (threshold_raw > BMI270_PICKUP_THRESHOLD_MAX_LSB) {
            threshold_raw = BMI270_PICKUP_THRESHOLD_MAX_LSB;
        }
        int32_t threshold = (int32_t)threshold_raw;
        bmi270_set_pickup_threshold((float)threshold);
        save_pickup_threshold_to_nvs(threshold);
        ESP_LOGI(TAG, "imu_wake_sensitivity threshold=%" PRId32, threshold);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "tap_enabled") == 0 &&
               cJSON_IsBool(enabled)) {
        s_tap_enabled = cJSON_IsTrue(enabled);
        bmi270_set_tap_enabled(s_tap_enabled);
        set_tap_polling_enabled(s_tap_enabled);
        save_tap_settings_to_nvs(s_tap_enabled, (int32_t)-1);
        ESP_LOGI(TAG, "tap_enabled %s", s_tap_enabled ? "true" : "false");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "encoder_led_color") == 0) {
        const cJSON *color_item = cJSON_GetObjectItemCaseSensitive(root, "color");
        uint32_t rgb = 0;
        if (cJSON_IsString(color_item) &&
            encoder_led_rgb_from_name(color_item->valuestring, &rgb)) {
            s_encoder_led_rgb = rgb;
            save_encoder_settings_to_nvs();
            ESP_LOGI(TAG, "encoder_led_color %s -> 0x%06" PRIX32,
                     color_item->valuestring, s_encoder_led_rgb);
        } else {
            ESP_LOGW(TAG, "unknown encoder_led_color ignored: %s",
                     cJSON_IsString(color_item) ? color_item->valuestring : "<missing>");
        }
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "encoder_recording_gate") == 0 &&
               cJSON_IsBool(enabled)) {
        s_encoder_recording_gate = cJSON_IsTrue(enabled);
        save_encoder_settings_to_nvs();
        ESP_LOGI(TAG, "encoder_recording_gate %s",
                 s_encoder_recording_gate ? "enabled" : "disabled");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "tap_sensitivity") == 0) {
        // 灵敏度 1..10（用户面向）：1=最不灵敏，10=最灵敏，默认 5。
        // 兼容 legacy 字符串 low/medium/high -> 2/5/9。
        const cJSON *level_item = cJSON_GetObjectItemCaseSensitive(root, "level");
        int32_t sensitivity = 5;
        if (cJSON_IsNumber(level_item)) {
            sensitivity = (int32_t)level_item->valueint;
        } else if (cJSON_IsString(level_item)) {
            if (strcmp(level_item->valuestring, "low") == 0) {
                sensitivity = 2;
            } else if (strcmp(level_item->valuestring, "medium") == 0) {
                sensitivity = 5;
            } else if (strcmp(level_item->valuestring, "high") == 0) {
                sensitivity = 9;
            } else {
                ESP_LOGW(TAG, "unknown tap_sensitivity %s", level_item->valuestring);
            }
        } else {
            ESP_LOGW(TAG, "tap_sensitivity missing level field");
        }
        bmi270_set_tap_sensitivity((int)sensitivity);
        save_tap_settings_to_nvs(s_tap_enabled, sensitivity);
        ESP_LOGI(TAG, "tap_sensitivity=%" PRId32, sensitivity);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "air_mouse_enabled") == 0 &&
               cJSON_IsBool(enabled)) {
        // 体感鼠标开关：由桌面端状态机权威控制。开启时校准零偏并启动 20ms 轮询上报 motion。
        set_air_mouse_enabled(cJSON_IsTrue(enabled));
        ESP_LOGI(TAG, "air_mouse_enabled %s", cJSON_IsTrue(enabled) ? "true" : "false");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "test_playback") == 0) {
        // 测试回放（L3 端到端测试）：设置预存 PCM 文件名，audio_task 从该文件读 PCM 替代采集。
        // file 为空或缺失则关闭回放恢复 ES8311 采集。仅端到端测试用，正常使用不触发。
        const cJSON *file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
        if (cJSON_IsString(file_item) && file_item->valuestring[0] != '\0') {
            esp_err_t pb_err = audio_pipeline_set_playback_file(file_item->valuestring);
            ESP_LOGI(TAG, "test_playback file=%s -> %s", file_item->valuestring,
                     esp_err_to_name(pb_err));
        } else {
            audio_pipeline_set_playback_file(NULL);
            ESP_LOGI(TAG, "test_playback cleared (restore capture)");
        }
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

// 编码器按钮与正面物理键在交互语义上完全等价：双击检测、hold 阈值、click_to_talk、
// 体感鼠标映射对两者一视同仁（见 app_input_source_t 注释）；owner 仲裁按来源独立
// （见 primary_owner_from_source），避免两个本地源互相截断录音。
static bool is_local_primary_source(app_input_source_t source)
{
    return source == APP_INPUT_SOURCE_PHYSICAL || source == APP_INPUT_SOURCE_ENCODER;
}

// 输入源 → owner 映射：三个来源各自独立仲裁，本地两源（物理/编码器）手势语义相同
// 但 owner 不同，互相按住时不截断对方录音（与本地 vs 远程同一互斥行为）。
static primary_owner_t primary_owner_from_source(app_input_source_t source)
{
    return source == APP_INPUT_SOURCE_PHYSICAL ? PRIMARY_OWNER_PHYSICAL :
           source == APP_INPUT_SOURCE_ENCODER  ? PRIMARY_OWNER_ENCODER :
                                                 PRIMARY_OWNER_REMOTE;
}

// 主键事件的 source 标签：编码器返回 "encoder"，其它来源返回 NULL（省略字段）。
static const char *primary_button_source_tag(void)
{
    return s_primary_press_source == APP_INPUT_SOURCE_ENCODER ? "encoder" : NULL;
}

// 侧键双击窗口超时：确认为单次点击，补发 button_click secondary（原单击语义）。
static void side_double_click_timer_cb(void *arg)
{
    (void)arg;
    if (!s_side_click_pending) {
        return;
    }
    s_side_click_pending = false;
    ESP_LOGI(TAG, "button side single-click (double-click window timeout)");
    (void)voice_ble_send_button_click("secondary", s_side_pending_duration_ms, 0, NULL);
}

// 侧键释放：单击延迟到双击窗口超时后确认；窗口内第二击直接发 button_double_click。
// 桌面端据此把侧键单击/双击映射到不同语义（单击进体感、双击恢复上次输入）。
static void handle_side_up(void)
{
    const uint32_t duration_ms = elapsed_button_ms(s_secondary_down_us);
    s_secondary_down_us = 0;

    // 双击窗口内第二次释放：确认双击。
    if (s_side_click_pending) {
        s_side_click_pending = false;
        (void)esp_timer_stop(s_side_double_click_timer);
        ESP_LOGI(TAG, "button side double-click");
        (void)voice_ble_send_button_double_click("secondary", NULL);
        return;
    }

    // 第一次释放：进入双击窗口，暂缓单击语义。
    s_side_click_pending = true;
    s_side_pending_duration_ms = duration_ms;
    (void)esp_timer_start_once(s_side_double_click_timer,
                               DOUBLE_CLICK_WINDOW_MS * 1000ULL);
}

static esp_err_t init_side_double_click_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = side_double_click_timer_cb,
        .name = "side_double_click",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_side_double_click_timer);
}

static void handle_primary_down(app_input_source_t source, uint32_t request_id)
{
    (void)request_id;
    ESP_LOGI(TAG, "button front down source=%d", source);
    const app_input_source_t prev_press_source = s_primary_press_source;
    s_primary_press_source = source;
    note_activity();
    // 按键按下抑制敲击检测，避免手指动作被 IMU 误判为双击（见 TAP_SUPPRESS_AFTER_BUTTON_MS）。
    s_tap_suppress_until_us = esp_timer_get_time() + (TAP_SUPPRESS_AFTER_BUTTON_MS * 1000LL);

    // 体感鼠标态：主键不启动本地录音（否则设备录音、屏幕卡 Recording，而桌面端在体感态
    // 会无视 button_down 不起 ASR，两端状态分裂）。仅记录按下时刻，松开时上报 button_click
    // 供桌面端映射为鼠标左键。
    if (s_air_mouse_enabled && is_local_primary_source(source)) {
        s_primary_down_us = esp_timer_get_time();
        s_primary_owner = primary_owner_from_source(source);
        return;
    }

    // 远程按键（热键）不走双击检测，直接走原有逻辑。
    if (is_local_primary_source(source)) {
        // 双击窗口内第二次按下：确认双击，发送 button_double_click。
        if (s_double_click_pending) {
            ESP_LOGI(TAG, "button front down as double-click second press");
            s_double_click_pending = false;
            (void)esp_timer_stop(s_double_click_timer);
            (void)voice_ble_send_button_double_click("primary", primary_button_source_tag());
            // 标记第二次按下，忽略其后续释放事件。
            s_double_click_second_press = true;
            s_primary_down_us = esp_timer_get_time();
            return;
        }

        // click_to_talk 模式已录音时：检测是否为双击的第二击。
        if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording &&
            s_click_to_talk_first_click_us > 0) {
            int64_t elapsed_us = esp_timer_get_time() - s_click_to_talk_first_click_us;
            if (elapsed_us < DOUBLE_CLICK_WINDOW_MS * 1000LL) {
                ESP_LOGI(TAG, "button front down as double-click in click_to_talk mode");
                (void)stop_recording();
                (void)voice_ble_send_button_double_click("primary", primary_button_source_tag());
                s_click_to_talk_first_click_us = 0;
                s_primary_down_us = 0;
                s_primary_session_id = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                apply_app_ui_state("ready", "");
                return;
            }
            s_click_to_talk_first_click_us = 0;
        }
    }

    // 编码器录音门控关闭：按下只走按键事件链路（双击检测已在上方完成；
    // 松开时由 handle_primary_up 的门控分支进双击窗口补发 button_click），
    // 不启动任何音频会话。物理主键不受影响。
    // !s_recording 条件：门控运行中从开切关时若录音已在进行，放行正常停录路径。
    if (source == APP_INPUT_SOURCE_ENCODER && !s_encoder_recording_gate && !s_recording) {
        s_primary_down_us = esp_timer_get_time();
        return;
    }

    if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK && s_recording) {
        const primary_owner_t owner_from_source = primary_owner_from_source(source);
        if (s_primary_owner != PRIMARY_OWNER_NONE && s_primary_owner != owner_from_source) {
            // 被拒按下不覆盖进行中按压的来源标签。
            s_primary_press_source = prev_press_source;
            ESP_LOGI(TAG, "ignore primary down from source=%d, owner is %d", source, s_primary_owner);
            return;
        }
    }

    if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording) {
        const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
        s_primary_session_id = stop_recording();
        esp_err_t primary_up_err = voice_ble_send_button_click("primary", primary_duration_ms,
                                                               s_primary_session_id,
                                                               primary_button_source_tag());
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
            (void)voice_ble_send_button_click("primary", 0, 0, primary_button_source_tag());
            return;
        }

        // hold_to_talk_instant（wechat 模式）：按下即启动录音 + 发送 button_down，跳过 300ms
        // hold 阈值，最小化按下到桌面端弹框的延迟。button_up 短按仍进双击窗口（双击发 Enter
        // 保留），与 hold_to_talk 一致（见 handle_primary_up 的短按双击判定）。
        if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK_INSTANT &&
            is_local_primary_source(source)) {
            // 提前请求 fast conn interval：conn update 异步耗时可达数百毫秒，按下即请求，
            // 让 button_down notify 在 fast interval(7.5ms) 下发出，避免 slow interval 传输延迟。
            voice_ble_request_fast_interval();
            s_primary_owner = primary_owner_from_source(source);
            s_primary_session_id = start_recording();
            if (s_primary_session_id == 0) {
                s_primary_down_us = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                return;
            }
            esp_err_t primary_down_err = voice_ble_send_button_down("primary", s_primary_session_id, primary_button_source_tag());
            if (primary_down_err != ESP_OK) {
                (void)stop_recording();
                s_primary_session_id = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                apply_app_ui_state("ready", "");
            }
            return;
        }

        // hold_to_talk：启动 300ms 按住阈值定时器，超时后确认为长按 → 启动录音 + 发送
        // button_down。若在阈值内释放 → 进入双击检测窗口（不产生任何桌面端浮窗）。
        // 提前请求 fast conn interval：conn update 异步耗时可达数百毫秒，在阈值等待期间
        // 并行启动，到 button_down 发出时多半已切到 7.5ms，避免 slow interval 传输延迟。
        if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK &&
            is_local_primary_source(source)) {
            voice_ble_request_fast_interval();
            s_hold_threshold_pending = true;
            s_primary_owner = primary_owner_from_source(source);
            s_primary_down_us = esp_timer_get_time();
            (void)esp_timer_start_once(s_double_click_timer,
                                       DOUBLE_CLICK_MAX_PRESS_MS * 1000ULL);
            return;
        }

        // click_to_talk 物理首击延迟双击窗口确认启动（双击回车语义 c）：
        // 窗口内第二次 click 发 button_double_click（不启动录音，桌面端 wechat_active=false
        // 走 else 直接 SendEnter 干净回车）；超时确认启动 start_recording + button_click(start)。
        // 代价：单击启动延迟 CLICK_TO_TALK_START_DELAY_MS。远程热键不走双击，立即启动。
        if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK &&
            is_local_primary_source(source)) {
            if (s_click_to_talk_pending_start) {
                s_click_to_talk_pending_start = false;
                (void)esp_timer_stop(s_double_click_timer);
                ESP_LOGI(TAG, "click_to_talk double-click (send button_double_click, no recording)");
                (void)voice_ble_send_button_double_click("primary", primary_button_source_tag());
                s_primary_down_us = 0;
                s_primary_session_id = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                s_click_to_talk_first_click_us = 0;
                return;
            }
            s_click_to_talk_pending_start = true;
            s_click_to_talk_first_click_us = esp_timer_get_time();
            s_primary_owner = primary_owner_from_source(source);
            s_primary_down_us = esp_timer_get_time();
            (void)esp_timer_start_once(s_double_click_timer,
                                       CLICK_TO_TALK_START_DELAY_MS * 1000ULL);
            ESP_LOGI(TAG, "click_to_talk first click, pending start (window %dms)",
                     CLICK_TO_TALK_START_DELAY_MS);
            return;
        }

        s_primary_session_id = start_recording();
        if (s_primary_session_id == 0) {
            s_primary_down_us = 0;
            return;
        }
        // click_to_talk 模式记录首次点击时刻，用于后续双击检测。
        if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK &&
            is_local_primary_source(source)) {
            s_click_to_talk_first_click_us = esp_timer_get_time();
        }
        s_primary_owner = primary_owner_from_source(source);
        esp_err_t primary_down_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
            ? voice_ble_send_button_click("primary", 0, s_primary_session_id, primary_button_source_tag())
            : voice_ble_send_button_down("primary", s_primary_session_id, primary_button_source_tag());
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
    // 按键松开同样抑制，覆盖松开瞬间手指余震。
    s_tap_suppress_until_us = esp_timer_get_time() + (TAP_SUPPRESS_AFTER_BUTTON_MS * 1000LL);

    // 体感鼠标态：主键松开上报 button_click，桌面端映射为鼠标左键单击。不涉及录音。
    if (s_air_mouse_enabled && is_local_primary_source(source)) {
        const uint32_t duration_ms = elapsed_button_ms(s_primary_down_us);
        (void)voice_ble_send_button_click("primary", duration_ms, 0, primary_button_source_tag());
        s_primary_down_us = 0;
        s_primary_owner = PRIMARY_OWNER_NONE;
        return;
    }

    // 双击第二击的释放：忽略，不触发任何事件。
    if (s_double_click_second_press) {
        ESP_LOGI(TAG, "button front up ignored (second press of double-click)");
        s_double_click_second_press = false;
        s_primary_down_us = 0;
        return;
    }

    // 门控关闭时的编码器释放：统一按短按处理进双击窗口（窗口超时补发 button_click，
    // 窗口内再按发 button_double_click），不产生 button_up，不涉及录音。
    if (source == APP_INPUT_SOURCE_ENCODER && !s_encoder_recording_gate && !s_recording) {
        if (s_primary_down_us == 0) {
            return;  // 无配对按下（如门控运行中切换），忽略
        }
        const uint32_t duration_ms = elapsed_button_ms(s_primary_down_us);
        ESP_LOGI(TAG, "encoder button up (recording gate off), double-click window (%" PRIu32 " ms)",
                 duration_ms);
        s_double_click_pending = true;
        s_pending_button_up_duration_ms = duration_ms;
        (void)esp_timer_start_once(s_double_click_timer,
                                   DOUBLE_CLICK_WINDOW_MS * 1000ULL);
        return;
    }

    if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK) {
        return;
    }

    // hold_to_talk：按住阈值计时中释放 → 短按，进入双击检测窗口。
    // 此时录音尚未启动，无 button_down 发送过，桌面端无感知。
    if (s_hold_threshold_pending) {
        s_hold_threshold_pending = false;
        (void)esp_timer_stop(s_double_click_timer);
        const uint32_t duration_ms = elapsed_button_ms(s_primary_down_us);
        ESP_LOGI(TAG, "button front up during hold threshold, entering double-click window (%" PRIu32 " ms)", duration_ms);
        s_double_click_pending = true;
        s_pending_button_up_duration_ms = duration_ms;
        (void)esp_timer_start_once(s_double_click_timer,
                                   DOUBLE_CLICK_WINDOW_MS * 1000ULL);
        return;
    }

    // 录音启动重试期间松开：从未发过 button_down、录音也未启动，干净退出，
    // 不走正常 button_up 路径（否则会发无对应 button_down 的 button_up）。
    if (s_recording_retry_pending) {
        s_recording_retry_pending = false;
        (void)esp_timer_stop(s_double_click_timer);
        ESP_LOGI(TAG, "button front up during recording start retry, aborting");
        s_primary_down_us = 0;
        s_primary_session_id = 0;
        s_primary_owner = PRIMARY_OWNER_NONE;
        return;
    }

    const primary_owner_t owner_from_source = primary_owner_from_source(source);
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

    // 双击检测：短按（< 300ms）可能为双击的第一击，暂缓发送 button_up。
    // 若 500ms 内无第二次按下则补发（桌面端因 < 0.5s 自动丢弃录音）。
    if (is_local_primary_source(source) &&
        primary_duration_ms > 0 &&
        primary_duration_ms < DOUBLE_CLICK_MAX_PRESS_MS) {
        ESP_LOGI(TAG, "button front up short press, entering double-click window");
        s_double_click_pending = true;
        s_pending_button_up_duration_ms = primary_duration_ms;
        (void)esp_timer_start_once(s_double_click_timer,
                                   DOUBLE_CLICK_WINDOW_MS * 1000ULL);
        return;
    }

    esp_err_t primary_up_err = voice_ble_send_button_up("primary", primary_duration_ms,
                                                        s_primary_session_id,
                                                        primary_button_source_tag());
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
    } else if (strcmp(state, "air_mouse") == 0) {
        s_app_ui_state = APP_UI_STATE_AIR_MOUSE;
        ui_status_set_air_mouse();
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
    const char *hint = "Hold to Talk";
    const char *name = "hold_to_talk";
    if (mode == INTERACTION_MODE_CLICK_TO_TALK) {
        hint = "Click to Talk";
        name = "click_to_talk";
    } else if (mode == INTERACTION_MODE_HOLD_TO_TALK_INSTANT) {
        hint = "Hold to Talk";
        name = "hold_to_talk_instant";
    }
    ui_status_set_idle_hint(hint);
    if (s_app_ui_state == APP_UI_STATE_READY && !s_recording) {
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "interaction mode %s", name);
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
            handle_side_up();
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
            s_double_click_pending = false;
            s_double_click_second_press = false;
            s_hold_threshold_pending = false;
            s_recording_retry_pending = false;
            s_click_to_talk_first_click_us = 0;
            s_click_to_talk_pending_start = false;
            (void)esp_timer_stop(s_double_click_timer);
            stop_host_response_timer();
            audio_pipeline_stop();
            // 断连直接停 pipeline（不走 stop_recording），同样灭编码器 LED。
            if (mini_encoder_c_present()) {
                (void)mini_encoder_c_set_led(0, 0, 0);
            }
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
                                         session_id, primary_button_source_tag());
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
        case APP_EVENT_TAP:
            // 敲击手势：仅在 BLE 已连接且未录音/识别时上报，避免干扰语音周期。
            if (voice_ble_is_connected() && !s_recording && !s_ota_updating &&
                (s_app_ui_state == APP_UI_STATE_READY ||
                 s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION)) {
                ESP_LOGI(TAG, "tap detected, sending to host");
                voice_ble_send_tap("double");
                note_activity();
            }
            break;
        case APP_EVENT_ENCODER_ROTATE:
            // 编码器旋转：发送门控仿 APP_EVENT_TAP，仅空闲态上报，避免干扰语音周期。
            // 体感开关与 ui_state 下发是两条独立消息，存在 ui_state 仍为 READY 但体感
            // 已开的窗口，故显式检查 s_air_mouse_enabled。方向映射在桌面端完成，
            // 固件只报原始物理事实。连转时可达 100 帧/秒，成功路径用 LOGD 保持默认静默。
            if (voice_ble_is_connected() && !s_recording && !s_ota_updating &&
                !s_air_mouse_enabled &&
                (s_app_ui_state == APP_UI_STATE_READY ||
                 s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION)) {
                const char *direction = (event.encoder_direction == 0) ? "cw" : "ccw";
                ESP_LOGD(TAG, "encoder rotate %s steps=%u, sending to host",
                         direction, (unsigned)event.encoder_steps);
                voice_ble_send_encoder_rotate(direction, event.encoder_steps);
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

// 双击/按住阈值共用定时器回调。
// 阶段一（s_hold_threshold_pending）：按住 300ms 确认 → 启动录音，发送 button_down。
// 阶段二（s_double_click_pending）：双击窗口 500ms 超时 → 单次短击，发送 button_click。
static void double_click_timer_cb(void *arg)
{
    (void)arg;

    // click_to_talk 首击延迟确认：双击窗口超时无第二次 click -> 确认启动录音。
    if (s_click_to_talk_pending_start) {
        s_click_to_talk_pending_start = false;
        ESP_LOGI(TAG, "click_to_talk pending start timeout, confirming recording");
        s_primary_session_id = start_recording();
        if (s_primary_session_id != 0) {
            esp_err_t err = voice_ble_send_button_click("primary", 0, s_primary_session_id, primary_button_source_tag());
            if (err != ESP_OK) {
                (void)stop_recording();
                s_primary_session_id = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                apply_app_ui_state("ready", "");
            }
        } else {
            s_primary_down_us = 0;
            s_primary_owner = PRIMARY_OWNER_NONE;
        }
        return;
    }

    if (s_recording_retry_pending) {
        // 录音启动重试：hold threshold 到点时 ble_ready 未就绪被拒，按住期间续重试。
        s_recording_retry_pending = false;
        // 用户已松开 → 干净放弃（未发过 button_down，无需补 button_up）。
        if (!primary_button_held_from_timer()) {
            ESP_LOGI(TAG, "recording start retry aborted: button released");
            s_primary_down_us = 0;
            s_primary_owner = PRIMARY_OWNER_NONE;
            return;
        }
        if (esp_timer_get_time() >= s_recording_retry_deadline_us) {
            ESP_LOGW(TAG, "recording start retry timed out (ble not ready in window)");
            s_primary_down_us = 0;
            s_primary_owner = PRIMARY_OWNER_NONE;
            return;
        }
        if (!voice_ble_is_ready()) {
            // 仍未就绪，继续重试。
            s_recording_retry_pending = true;
            (void)esp_timer_start_once(s_double_click_timer,
                                       RECORDING_RETRY_INTERVAL_MS * 1000ULL);
            return;
        }
        // ble_ready 已就绪，启动录音（复用现有成功路径）。
        ESP_LOGI(TAG, "recording start retry: ble ready, starting");
        s_primary_session_id = start_recording();
        if (s_primary_session_id != 0) {
            esp_err_t err = voice_ble_send_button_down("primary", s_primary_session_id, primary_button_source_tag());
            if (err != ESP_OK) {
                (void)stop_recording();
                s_primary_session_id = 0;
                s_primary_owner = PRIMARY_OWNER_NONE;
                apply_app_ui_state("ready", "");
            }
        } else {
            // ble_ready=1 仍失败 → 不可恢复原因，放弃。
            s_primary_down_us = 0;
            s_primary_owner = PRIMARY_OWNER_NONE;
        }
        return;
    }

    if (s_hold_threshold_pending) {
        // 按住阈值达成：按钮仍按下则确认为长按，启动录音。
        s_hold_threshold_pending = false;
        if (primary_button_held_from_timer()) {
            ESP_LOGI(TAG, "hold threshold reached, starting recording");
            s_primary_session_id = start_recording();
            if (s_primary_session_id != 0) {
                esp_err_t err = voice_ble_send_button_down("primary", s_primary_session_id, primary_button_source_tag());
                if (err != ESP_OK) {
                    (void)stop_recording();
                    s_primary_session_id = 0;
                    s_primary_owner = PRIMARY_OWNER_NONE;
                    apply_app_ui_state("ready", "");
                }
            } else if (!voice_ble_is_ready()) {
                // ble_ready=0 可恢复：按住等待重试，覆盖 Windows 订阅完成的过渡期。
                ESP_LOGI(TAG, "ble not ready, deferring recording start (retrying)");
                s_recording_retry_pending = true;
                s_recording_retry_deadline_us =
                    esp_timer_get_time() + RECORDING_RETRY_WINDOW_MS * 1000LL;
                (void)esp_timer_start_once(s_double_click_timer,
                                           RECORDING_RETRY_INTERVAL_MS * 1000ULL);
                // 保留 s_primary_owner / s_primary_down_us 不变。
            } else {
                // 不可恢复原因，放弃。
                s_primary_down_us = 0;
            }
        }
        return;
    }

    if (s_double_click_pending) {
        // 双击窗口超时：单次短击。
        s_double_click_pending = false;
        ESP_LOGI(TAG, "double-click window expired, sending button_click");
        voice_ble_send_button_click("primary", s_pending_button_up_duration_ms, 0,
                                    primary_button_source_tag());
        s_primary_down_us = 0;
        s_primary_session_id = 0;
        s_primary_owner = PRIMARY_OWNER_NONE;
    }
}

static esp_err_t init_double_click_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = double_click_timer_cb,
        .name = "double_click",
    };
    return esp_timer_create(&timer_args, &s_double_click_timer);
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

// 敲击检测轮询：10ms 周期，仅在启用、BMI270 在线、未录音/OTA、BLE 已连接时工作。
static void tap_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!s_tap_enabled || !voice_ble_is_connected() || s_recording || s_ota_updating) {
        return;
    }
    // 按键事件抑制窗口内不检测，避免按语音键的手指动作误触发双击。
    if (esp_timer_get_time() < s_tap_suppress_until_us) {
        return;
    }
    if (bmi270_tap_poll()) {
        queue_app_event(APP_EVENT_TAP);
    }
}

static esp_err_t init_tap_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = tap_poll_timer_cb,
        .name = "tap_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_tap_poll_timer);
}

static void set_tap_polling_enabled(bool enabled)
{
    if (!s_tap_poll_timer) {
        return;
    }
    if (enabled) {
        if (!esp_timer_is_active(s_tap_poll_timer)) {
            esp_err_t err = esp_timer_start_periodic(s_tap_poll_timer, TAP_POLL_INTERVAL_US);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "start tap poll failed: %s", esp_err_to_name(err));
            }
        }
    } else {
        (void)esp_timer_stop(s_tap_poll_timer);
    }
}

// 编码器轮询：按钮边沿 → 主键 down/up 事件（APP_INPUT_SOURCE_ENCODER，语义等价物理键）；
// 旋转增量 → APP_EVENT_ENCODER_ROTATE（非零读数即入队，发送门控在 app_event_task）。
// 在 timer 任务上下文做 I2C 读，与 air_mouse_poll_timer_cb 同一先例（负载轻）。
// I2C 失败 streak 期间每次轮询最坏占 60ms（6 倍轮询周期，按钮+增量两次 I2C 交易），
// 与双击 500ms/hold 300ms 窗口相比可忽略；累计 10 次失败即停表降级。
// 组件连续 I2C 失败标记 absent 后停表，避免空转与日志刷屏。
static void encoder_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!mini_encoder_c_present()) {
        // 按住期间掉线（拔线/松线）：补发 up 事件释放悬挂按下态，否则录音无法结束。
        if (s_encoder_button_pressed) {
            s_encoder_button_pressed = false;
            queue_primary_up_event(APP_INPUT_SOURCE_ENCODER, 0);
        }
        (void)esp_timer_stop(s_encoder_poll_timer);
        return;
    }

    bool pressed = false;
    if (mini_encoder_c_read_button(&pressed) == ESP_OK &&
        pressed != s_encoder_button_pressed) {
        s_encoder_button_pressed = pressed;
        if (pressed) {
            queue_primary_down_event(APP_INPUT_SOURCE_ENCODER, 0);
        } else {
            queue_primary_up_event(APP_INPUT_SOURCE_ENCODER, 0);
        }
    }

    int32_t delta = 0;
    if (mini_encoder_c_read_delta(&delta) == ESP_OK && delta != 0) {
        // 2 计数 = 1 格：跨窗口累计，满 2 个同向计数上报 1 步；方向反转丢弃余数。
        if ((s_encoder_count_rem > 0) != (delta > 0)) {
            s_encoder_count_rem = 0;
        }
        s_encoder_count_rem += delta;
        const int32_t steps = s_encoder_count_rem / 2;  // 向零取整，保留符号
        if (steps != 0) {
            s_encoder_count_rem -= steps * 2;
            queue_encoder_rotate_event(steps);
        }
    }
}

static esp_err_t init_encoder_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = encoder_poll_timer_cb,
        .name = "encoder_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_encoder_poll_timer);
}

// 体感鼠标轮询：读陀螺仪→整型位移→直接发 motion 帧。参照 imu_poll_timer_cb 在 timer
// 任务里做 I²C + BLE notify（负载轻，非 Wi-Fi 重活，不违反 timer cb 栈约束）。
static void air_mouse_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!s_air_mouse_enabled || !voice_ble_is_connected() || s_recording || s_ota_updating) {
        return;
    }
    int16_t dx = 0;
    int16_t dy = 0;
    if (bmi270_air_mouse_poll(&dx, &dy)) {
        (void)voice_ble_send_motion(dx, dy);
    }
}

static esp_err_t init_air_mouse_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = air_mouse_poll_timer_cb,
        .name = "air_mouse_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_air_mouse_poll_timer);
}

static void set_air_mouse_enabled(bool enabled)
{
    s_air_mouse_enabled = enabled;
    if (!s_air_mouse_poll_timer) {
        return;
    }
    if (enabled) {
        bmi270_air_mouse_start();
        // 请求 7.5ms fast conn interval：50Hz motion 帧在 slow interval(100~400ms)下会被
        // 严重限流导致光标卡顿。conn update 异步，进入时立即请求，等真正发帧时多半已切换。
        (void)voice_ble_request_fast_interval();
        if (!esp_timer_is_active(s_air_mouse_poll_timer)) {
            esp_err_t err = esp_timer_start_periodic(s_air_mouse_poll_timer, AIR_MOUSE_POLL_INTERVAL_US);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "start air mouse poll failed: %s", esp_err_to_name(err));
            }
        }
    } else {
        (void)esp_timer_stop(s_air_mouse_poll_timer);
        bmi270_air_mouse_stop();
        // 退出体感恢复省电的 slow interval（若此时未在录音）。
        if (!s_recording) {
            (void)voice_ble_request_slow_interval();
        }
    }
}

static void update_display_orientation(float x_g)
{
    display_orientation_t desired = s_display_orientation;

    if (s_display_orientation == DISPLAY_ORIENTATION_NORMAL) {
        if (x_g < -ORIENTATION_THRESHOLD_G) {
            desired = DISPLAY_ORIENTATION_UPSIDE_DOWN;
        }
    } else {
        if (x_g > ORIENTATION_THRESHOLD_G) {
            desired = DISPLAY_ORIENTATION_NORMAL;
        }
    }

    if (desired == s_display_orientation) {
        s_orientation_confirm_count = 0;
        return;
    }

    s_orientation_confirm_count++;
    if (s_orientation_confirm_count >= ORIENTATION_CONFIRM_COUNT) {
        s_display_orientation = desired;
        s_orientation_confirm_count = 0;
        ui_status_set_orientation(s_display_orientation == DISPLAY_ORIENTATION_UPSIDE_DOWN);
    }
}

// IMU 轮询：读三轴加速度、更新朝向自动旋转、按需上屏调试值。
// IMU 不在线时仅在首次刷一次 "IMU: n/a" 并停表，避免空转刷屏。
static void imu_poll_timer_cb(void *arg)
{
    (void)arg;

    if (!bmi270_present()) {
        ui_status_set_imu_text("IMU: n/a");
        (void)esp_timer_stop(s_imu_poll_timer);
        return;
    }

    float x_g = 0.0f;
    float y_g = 0.0f;
    float z_g = 0.0f;
    if (bmi270_read_acc_g(&x_g, &y_g, &z_g) != ESP_OK) {
        return;
    }

    update_display_orientation(x_g);

    // 串口日志跟随 show_imu_debug 开关：未开启调试时不刷屏，避免每 200ms 淹没其他日志。
    if (!s_show_imu_debug) {
        ui_status_set_imu_text("");
        return;
    }

    ESP_LOGI(TAG, "IMU acc X=%+.2f Y=%+.2f Z=%+.2f g", x_g, y_g, z_g);

    float x_dps = 0.0f, y_dps = 0.0f, z_dps = 0.0f;
    (void)bmi270_read_gyr_dps(&x_dps, &y_dps, &z_dps);

    char buf[80];
    snprintf(buf, sizeof(buf),
             "A:%+.2f,%+.2f,%+.2f\nG:%+.1f,%+.1f,%+.1f\nT:%s",
             x_g, y_g, z_g, x_dps, y_dps, z_dps,
             s_tap_enabled ? "on" : "off");
    ui_status_set_imu_text(buf);
}

static esp_err_t init_imu_poll_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = imu_poll_timer_cb,
        .name = "imu_poll",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_imu_poll_timer);
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

static void load_pickup_threshold_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READONLY, &handle);
    int32_t threshold = (int32_t)BMI270_PICKUP_THRESHOLD_DEFAULT_LSB;
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, "pickup_thr", &threshold);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load pickup threshold failed: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
    }

    if (threshold < (int32_t)BMI270_PICKUP_THRESHOLD_MIN_LSB) {
        threshold = (int32_t)BMI270_PICKUP_THRESHOLD_MIN_LSB;
    } else if (threshold > (int32_t)BMI270_PICKUP_THRESHOLD_MAX_LSB) {
        threshold = (int32_t)BMI270_PICKUP_THRESHOLD_MAX_LSB;
    }

    bmi270_set_pickup_threshold((float)threshold);
    ESP_LOGI(TAG, "pickup threshold loaded from nvs: %" PRId32, threshold);
}

static void save_pickup_threshold_to_nvs(int32_t threshold)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(handle, "pickup_thr", threshold);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save pickup threshold failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit pickup threshold failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "pickup threshold saved to nvs: %" PRId32, threshold);
    }
    nvs_close(handle);
}

static void load_tap_settings_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READONLY, &handle);
    int32_t enabled = 1;  // 默认开启
    int32_t sensitivity = 5;  // 默认档 5（对应原 medium 体验）
    bool migrated = false;
    if (err == ESP_OK) {
        esp_err_t e = nvs_get_i32(handle, "tap_en", &enabled);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load tap enabled failed: %s", esp_err_to_name(e));
        }
        // 优先读新 key tap_lvl2（用户面向 1..10）
        e = nvs_get_i32(handle, "tap_lvl2", &sensitivity);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            // 旧 key tap_lvl 存的是 0/1/2 索引（low/medium/high），一次性迁移到 1..10 尺度
            int32_t legacy = 0;
            e = nvs_get_i32(handle, "tap_lvl", &legacy);
            if (e == ESP_OK) {
                switch (legacy) {
                    case 0:  sensitivity = 2; break;  // low
                    case 1:  sensitivity = 5; break;  // medium
                    case 2:  sensitivity = 9; break;  // high
                    default: sensitivity = 5; break;
                }
                migrated = true;
                ESP_LOGI(TAG, "tap sensitivity migrated from legacy %" PRId32 " to %" PRId32, legacy, sensitivity);
            }
        } else if (e != ESP_OK) {
            ESP_LOGW(TAG, "load tap sensitivity failed: %s", esp_err_to_name(e));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
    }

    if (sensitivity < 1 || sensitivity > 10) {
        sensitivity = 5;
    }

    s_tap_enabled = (enabled != 0);
    bmi270_set_tap_enabled(s_tap_enabled);
    bmi270_set_tap_sensitivity((int)sensitivity);
    ESP_LOGI(TAG, "tap settings loaded from nvs: enabled=%d sensitivity=%" PRId32, s_tap_enabled, sensitivity);

    // 迁移后持久化到新 key 并删除旧 key，避免重复迁移。
    if (migrated) {
        save_tap_settings_to_nvs(s_tap_enabled, sensitivity);
        if (nvs_open("voicestick", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, "tap_lvl");
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
}

// 编码器设置：enc_led（i32，0xRRGGBB，默认红）、enc_rec_gate（i32，1=录音语义，默认开）。
static void load_encoder_settings_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READONLY, &handle);
    int32_t led = (int32_t)0xFF0000;
    int32_t gate = 1;  // 默认开（录音语义）
    if (err == ESP_OK) {
        esp_err_t e = nvs_get_i32(handle, "enc_led", &led);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load encoder led failed: %s", esp_err_to_name(e));
        }
        e = nvs_get_i32(handle, "enc_rec_gate", &gate);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load encoder recording gate failed: %s", esp_err_to_name(e));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
    }
    s_encoder_led_rgb = ((uint32_t)led) & 0xFFFFFFu;
    s_encoder_recording_gate = (gate != 0);
    ESP_LOGI(TAG, "encoder settings loaded from nvs: led=0x%06" PRIX32 " gate=%d",
             s_encoder_led_rgb, s_encoder_recording_gate ? 1 : 0);
}

static void save_encoder_settings_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i32(handle, "enc_led", (int32_t)s_encoder_led_rgb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save encoder led failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    err = nvs_set_i32(handle, "enc_rec_gate", s_encoder_recording_gate ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save encoder recording gate failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit encoder settings failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "encoder settings saved to nvs: led=0x%06" PRIX32 " gate=%d",
                 s_encoder_led_rgb, s_encoder_recording_gate ? 1 : 0);
    }
    nvs_close(handle);
}

static void save_tap_settings_to_nvs(bool enabled, int32_t sensitivity)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
        return;
    }

    if (enabled) {
        err = nvs_set_i32(handle, "tap_en", 1);
    } else {
        err = nvs_set_i32(handle, "tap_en", 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save tap enabled failed: %s", esp_err_to_name(err));
    }
    if (sensitivity >= 1 && sensitivity <= 10) {
        err = nvs_set_i32(handle, "tap_lvl2", sensitivity);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save tap sensitivity failed: %s", esp_err_to_name(err));
        }
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit tap settings failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "tap settings saved to nvs: enabled=%d sensitivity=%" PRId32, enabled ? 1 : 0, sensitivity);
    }
    nvs_close(handle);
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset_reason=%d wakeup_cause=%d ext1_status=0x%llx",
             esp_reset_reason(), esp_sleep_get_wakeup_cause(),
             (unsigned long long)esp_sleep_get_ext1_wakeup_status());

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(stick_s3_board_init());
    // BLE 初始化（含 NimBLE host 任务启动）提前到屏幕初始化之前：on_sync 回调里
    // 开始广播是异步的，提前调用让「NimBLE 同步→开始广播」与耗时的 ui_status_init
    // （ST7789/LVGL）并行，缩短深睡唤醒后到设备可被发现的启动时间。
    // voice_ble_init 内部自初始化 NVS，不依赖 ui_status。
    // 回调注册也随之前移：广播/连接可能在 ui_status_init 期间就发生，
    // 必须在此之前挂好 connection/control/OTA 回调。
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_control_callback(ble_control_cb);
    voice_ble_set_ota_callback(ble_ota_cb);
    esp_err_t err = voice_ble_init();
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_display_off_timer());
    ESP_ERROR_CHECK(init_poweroff_timer());
    ESP_ERROR_CHECK(init_disc_poweroff_timer());
    ESP_ERROR_CHECK(init_host_response_timer());
    ESP_ERROR_CHECK(init_double_click_timer());
    ESP_ERROR_CHECK(init_side_double_click_timer());
    ESP_ERROR_CHECK(init_pickup_poll_timer());
    ESP_ERROR_CHECK(init_tap_poll_timer());
    ESP_ERROR_CHECK(init_air_mouse_poll_timer());
    // BMI270 初始化在 I2C 总线就绪后；探测失败时优雅降级，不影响主流程。
    (void)bmi270_init();
    // 根据初始握持方向设置屏幕方向，避免启动后方向与实际相反。
    float initial_x_g = 0.0f;
    if (bmi270_present() && bmi270_read_acc_g(&initial_x_g, NULL, NULL) == ESP_OK) {
        s_display_orientation = (initial_x_g < 0.0f)
            ? DISPLAY_ORIENTATION_UPSIDE_DOWN
            : DISPLAY_ORIENTATION_NORMAL;
        ui_status_set_orientation(s_display_orientation == DISPLAY_ORIENTATION_UPSIDE_DOWN);
    }
    // IMU X 轴加速度常驻上屏：定时器在 ui_status 与 IMU 就绪后启动，常驻运行。
    ESP_ERROR_CHECK(init_imu_poll_timer());
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_imu_poll_timer, IMU_POLL_INTERVAL_US));
    note_activity();
    // MiniEncoderC 编码器：探测失败优雅降级（absent），不影响主流程。
    (void)mini_encoder_c_init();
    ESP_ERROR_CHECK(init_encoder_poll_timer());
    ESP_ERROR_CHECK(init_buttons());
    // 仅在线时启动 10ms 轮询；必须在 init_buttons 之后（事件队列已创建）。
    if (mini_encoder_c_present()) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_encoder_poll_timer,
                                                 ENCODER_POLL_INTERVAL_US));
    }

    // voice_ble_init 已提前到 ui_status_init 之前执行（见上方注释），此处仅处理其结果。
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        ui_status_set_error("BLE init failed");
    } else {
        ui_status_set_device_name(voice_ble_device_name());
    }

    // 从 NVS 恢复拿起检测阈值与敲击手势设置；voice_ble_init 已初始化 NVS flash。
    load_pickup_threshold_from_nvs();
    load_tap_settings_from_nvs();
    load_encoder_settings_from_nvs();
    set_tap_polling_enabled(s_tap_enabled);

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        ui_status_set_error("Audio init failed");
    }

    if (err == ESP_OK) {
        ui_status_set_pairing(voice_ble_device_name());
    }
    ESP_LOGI(TAG, "Voice Stick booted");

    // OTA rollback 签到（CONFIG_APP_ROLLBACK_ENABLE=y）：新固件首次启动处于
    // PENDING_VERIFY，必须签到否则 bootloader 超时回滚。BLE OTA 与 COM 口烧录的
    // OTA 都依赖此签到，故 boot 时无条件直接 mark_app_valid_cancel_rollback。
    esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_err == ESP_OK) {
        ESP_LOGI(TAG, "mark_app_valid_cancel_rollback ok");
    } else if (mark_err == ESP_ERR_NOT_SUPPORTED || mark_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "mark_valid no-op: %s", esp_err_to_name(mark_err));
    } else {
        ESP_LOGW(TAG, "mark_valid failed: %s", esp_err_to_name(mark_err));
    }

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
