#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define STICK_S3_PIN_BUTTON_FRONT 11
#define STICK_S3_PIN_BUTTON_SIDE  12
#define STICK_S3_PIN_PMIC_IRQ     13

#define STICK_S3_PIN_I2C_SCL 48
#define STICK_S3_PIN_I2C_SDA 47

#define STICK_S3_PIN_ES8311_MCLK 18
// Pin names follow the codec's perspective:
//   ES8311_DIN  = codec serial data input  (DSDIN, MCU -> codec, speaker path) = GPIO14
//   ES8311_DOUT = codec serial data output (ASDOUT, codec -> MCU, mic path)   = GPIO16
#define STICK_S3_PIN_ES8311_BCLK 17
#define STICK_S3_PIN_ES8311_LRCK 15
#define STICK_S3_PIN_ES8311_DIN  14
#define STICK_S3_PIN_ES8311_DOUT 16

#define STICK_S3_PIN_LCD_MOSI 39
#define STICK_S3_PIN_LCD_SCK  40
#define STICK_S3_PIN_LCD_DC   45
#define STICK_S3_PIN_LCD_CS   41
#define STICK_S3_PIN_LCD_RST  21
#define STICK_S3_PIN_LCD_BL   38

esp_err_t stick_s3_board_init(void);
i2c_master_bus_handle_t stick_s3_board_i2c_bus(void);
esp_err_t stick_s3_board_battery_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_vbus_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_battery_level(int *level_percent);
esp_err_t stick_s3_board_battery_charging(bool *charging);
esp_err_t stick_s3_board_usb_powered(bool *usb_powered);
esp_err_t stick_s3_board_clear_power_irqs(uint8_t *sys_status);
void stick_s3_board_prepare_deep_sleep(void);
// 独立控制 L3B 层（LCD 背光/MIC/SPK）供电，用于 S2 熄屏保连态：关 L3B 降功耗但不进 deep sleep。
// enable=true 打开 L3B（恢复亮屏），false 关闭 L3B（熄屏）。不触碰其它电源层。
void stick_s3_board_set_l3b_power(bool enable);
// M5PM1 软件关机（写 SYS_CMD=0xA1）。调用前应已保持 L1(IMU) 供电并配置 PYG4 唤醒源。
// 关机后整机断电，仅 L0+L1 供电，靠按键或 IMU 翻转唤醒 M5PM1 重新上电冷启动。
void stick_s3_board_power_off(void);
bool stick_s3_front_button_pressed(void);
bool stick_s3_side_button_pressed(void);
