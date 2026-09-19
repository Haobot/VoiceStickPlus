#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "VS"

#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02

#define VOICE_BLE_OTA_TYPE_BEGIN 0x20
#define VOICE_BLE_OTA_TYPE_DATA  0x21
#define VOICE_BLE_OTA_TYPE_END   0x22
#define VOICE_BLE_OTA_TYPE_ABORT 0x23
#define VOICE_BLE_OTA_TYPE_STATE 0x30

typedef void (*voice_ble_connection_cb_t)(bool connected);
typedef void (*voice_ble_control_cb_t)(const char *json);

// 额外 GATT 服务注入钩子：在 voice_ble_init 的 NimBLE 注册窗口
// （nimble_port_freertos_init 之前）调用回调取服务数组统一 count/add。
// 用于 gateway HOGP 等外设侧附加服务；普通模式下注入与否不影响既有行为。
struct ble_gatt_svc_def;
typedef const struct ble_gatt_svc_def *(*voice_ble_extra_svcs_fn_t)(size_t *count);
void voice_ble_set_extra_svcs(voice_ble_extra_svcs_fn_t fn);

typedef enum {
    VOICE_BLE_OTA_EVENT_BEGIN,
    VOICE_BLE_OTA_EVENT_PROGRESS,
    VOICE_BLE_OTA_EVENT_DONE,
    VOICE_BLE_OTA_EVENT_ERROR,
    VOICE_BLE_OTA_EVENT_ABORT,
} voice_ble_ota_event_t;

typedef void (*voice_ble_ota_cb_t)(voice_ble_ota_event_t event,
                                   uint32_t written,
                                   uint32_t size);

// 对端身份回调（peripheral 侧）：连接建立/断开时上报对端 identity address 与地址类型，
// 供网关目标表（gateway_targets）识别「当前目标是哪台桌面端」。id_addr 为 NimBLE val 字节序。
typedef void (*voice_ble_peer_cb_t)(bool connected, const uint8_t id_addr[6], uint8_t addr_type);
void voice_ble_set_peer_callback(voice_ble_peer_cb_t callback);

esp_err_t voice_ble_init(void);
void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback);
void voice_ble_set_control_callback(voice_ble_control_cb_t callback);
void voice_ble_set_ota_callback(voice_ble_ota_cb_t callback);
const char *voice_ble_device_id(void);
const char *voice_ble_device_name(void);
bool voice_ble_is_connected(void);
bool voice_ble_is_ready(void);
// 优雅断开当前 BLE 连接（LL_TERMINATE），并同步等待 DISCONNECT 事件返回，
// 让对端立刻感知断连。用于 deep sleep 关机前调用，避免对端留存僵尸连接。
// 未连接时立即返回 ESP_OK；超时仍未断开返回 ESP_ERR_TIMEOUT。
esp_err_t voice_ble_disconnect(uint32_t timeout_ms);
bool voice_ble_ota_is_active(void);
esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len);
esp_err_t voice_ble_request_fast_interval(void);
esp_err_t voice_ble_request_slow_interval(void);
esp_err_t voice_ble_send_device_info(void);
// 上报 MiniEncoderC 编码器在线状态（独立小帧：device_info 已接近 BLE 通知 MTU 上限，
// 不宜再扩字段）。在 state_tx 订阅后随 device_info 一起发送；运行期降级时也发送。
esp_err_t voice_ble_send_encoder_status(void);
// 设置 MiniEncoderC 编码器在线标志。在 mini_encoder_c_init() 之后由 main 设置一次；
// 运行期降级为 absent 时更新为 false 并触发一次 encoder_status 上报。
void voice_ble_set_encoder_present(bool present);
// 上报网关模式（gateway/normal，独立小帧）。桌面端据此抑制「直连遥控器 ATVV」：
// 网关模式下遥控器 bond 在 Stick 上，桌面端直连必然订阅超时。
// 发送时机：state_tx 订阅成功后随 device_info 一起补发；模式翻转后由 main 再发一次。
esp_err_t voice_ble_send_gateway_status(void);
// 设置网关模式标志（由 main 在 gateway_apply_mode() 中调用）。
void voice_ble_set_gateway_mode(bool gateway);
// source 为事件来源标签（如 "encoder"），NULL 时省略该字段（物理键/远程键行为不变）。
esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id,
                                     const char *source);
esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id, const char *source);
esp_err_t voice_ble_send_button_click(const char *button, uint32_t duration_ms,
                                      uint32_t session_id, const char *source);
esp_err_t voice_ble_send_button_double_click(const char *button, const char *source);
// 发送敲击事件（如 double-tap）。kind 建议为 "double"，空时默认 double。
esp_err_t voice_ble_send_tap(const char *kind);
// 发送编码器旋转事件。direction 为 "cw"/"ccw"（原始物理方向，固件不做语义映射），
// steps 为该轮询窗口内同向累计格数（>=1）。
esp_err_t voice_ble_send_encoder_rotate(const char *direction, uint8_t steps);
// 发送网关按键事件（小米软件路由键，P1 隧道融合）：key 为 gateway_keymap_key_name
// 给出的协议键名（如 "back"/"volume_up"）。仅网关模式且该键设软件路由时发出。
esp_err_t voice_ble_send_gateway_key(const char *key, bool pressed);
// 上报网关按键路由表（gateway_keymap_get 命令的回执）。routes_json 为调用方
// 拼好的 JSON 数组，元素形如 {"key":"back","route":"software"}。
esp_err_t voice_ble_send_gateway_keymap(const char *routes_json);
// 发送体感鼠标运动帧（state_tx 通道，type=0x11 二进制帧，6 字节）。
esp_err_t voice_ble_send_motion(int16_t dx, int16_t dy);
esp_err_t voice_ble_send_battery_status(int level_percent, bool charging, bool usb_powered);
// 上报供电态（USB）自动关机开关状态（独立小帧 {"event":"power_mgmt","usb_auto_off":...}）。
// BLE 连接建立与开关变更时发送，供桌面端电池监测窗口的勾选框同步。
esp_err_t voice_ble_send_power_mgmt_status(bool usb_auto_off);
