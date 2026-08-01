#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define STICK_S3_PIN_BUTTON_FRONT 11
#define STICK_S3_PIN_BUTTON_SIDE  12
#define STICK_S3_PIN_PMIC_IRQ     13

#define STICK_S3_PIN_I2C_SCL 48
#define STICK_S3_PIN_I2C_SDA 47

// Grove 口第二路 I2C 引脚（备用，当前未接外设）。与内部 G47/G48 总线
// 分属不同 I2C 端口。Grove 口 5V 保持不启用（不动 PMIC BOOST_EN），外设供电由外部接线负责。
#define STICK_S3_PIN_GROVE_SDA 9
#define STICK_S3_PIN_GROVE_SCL 10

// 顶部 8pin Hat 排针第二路 I2C（MiniEncoderC Hat）。按 MiniEncoderC Hat 接线图：
// SDA=G8，SCL=G0。G0 是 strapping 引脚，Hat 板载上拉使其 boot 时为高，不影响正常启动。
#define STICK_S3_PIN_HAT_SDA 8
#define STICK_S3_PIN_HAT_SCL 0

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
// 返回内部总线（ES8311/BMI270/M5PM1）实际生效的 I2C 端口号。
// init_i2c 有 NUM_1→NUM_0 探测回退，第二路总线（如 MiniEncoderC）据此选用另一个端口。
// 仅在 stick_s3_board_init() 的 I2C 探测成功后有效；探测失败时返回缺省 I2C_NUM_1。
i2c_port_t stick_s3_board_i2c_port(void);
esp_err_t stick_s3_board_battery_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_vbus_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_battery_level(int *level_percent);
esp_err_t stick_s3_board_battery_charging(bool *charging);
esp_err_t stick_s3_board_usb_powered(bool *usb_powered);
esp_err_t stick_s3_board_clear_power_irqs(uint8_t *sys_status);
// M5PM1 RTC RAM（0xA0-0xBF，共 32 字节，睡眠/关机期间保持）读写接口。
// offset 为区域内偏移（0-31），offset+len 不得越过 32 字节；供 power_log 跨 S3 关机保存记账锚点。
esp_err_t stick_s3_board_pmic_rtc_ram_read(uint8_t offset, uint8_t *buf, size_t len);
esp_err_t stick_s3_board_pmic_rtc_ram_write(uint8_t offset, const uint8_t *data, size_t len);
void stick_s3_board_prepare_deep_sleep(void);
// 独立控制 L3B 层（LCD 背光/MIC/SPK）供电，用于 S2 熄屏保连态：关 L3B 降功耗但不进 deep sleep。
// enable=true 打开 L3B（恢复亮屏），false 关闭 L3B（熄屏）。不触碰其它电源层。
void stick_s3_board_set_l3b_power(bool enable);
// M5PM1 软件关机（写 SYS_CMD=0xA1）。调用前应已保持 L1(IMU) 供电并配置 PYG4 唤醒源。
// 关机后整机断电，仅 L0+L1 供电，靠按键或 IMU 翻转唤醒 M5PM1 重新上电冷启动。
void stick_s3_board_power_off(void);
bool stick_s3_front_button_pressed(void);
bool stick_s3_side_button_pressed(void);
