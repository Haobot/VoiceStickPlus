#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
void ui_status_set_device_name(const char *device_name);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle_hint(const char *hint);
void ui_status_set_idle(void);
void ui_status_set_idle_dimmed(bool dimmed);
void ui_status_set_recording(uint32_t session_id);
void ui_status_set_battery(int level_percent, bool charging, bool usb_powered);
void ui_status_set_partial_text(const char *text);
// 刷新屏幕顶部常驻 IMU 行（实时显示 X 轴加速度等调试数值）。
void ui_status_set_imu_text(const char *text);
// 刷新屏幕顶部 IMU 下方的 Wi-Fi 信息行（由桌面端 show_wifi_info 开关控制）。
void ui_status_set_wifi_text(const char *text);
// 根据设备方向设置屏幕旋转：false 为正常方向（0°），true 为倒置（180°）。
void ui_status_set_orientation(bool upside_down);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);
