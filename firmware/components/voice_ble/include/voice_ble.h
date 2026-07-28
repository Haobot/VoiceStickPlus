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
// 发送体感鼠标运动帧（state_tx 通道，type=0x11 二进制帧，6 字节）。
esp_err_t voice_ble_send_motion(int16_t dx, int16_t dy);
esp_err_t voice_ble_send_battery_status(int level_percent, bool charging, bool usb_powered);
