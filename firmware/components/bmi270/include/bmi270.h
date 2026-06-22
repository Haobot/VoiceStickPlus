#pragma once

#include <stdbool.h>
#include "esp_err.h"

// BMI270 拿起检测驱动。
//
// 硬件事实（据 M5Stack 官方文档与 Bosch BMI270-Sensor-API 源码）：
//   - IMU 为 BMI270，I2C 与 PMIC/ES8311 共用 G47/G48 总线（stick_s3_board 已建好）。
//   - BMI270 的 INT1 接 M5PM1 的 GPIO4(PYG4)，不接 ESP32-S3 任何 GPIO。
//     因此 ESP32 在线时无法被 IMU 中断被动唤醒，只能主动轮询 ACC 数据做软件差分。
//   - ESP32 关机后，IMU(INT1→PYG4) 可在 M5PM1 级唤醒整机（阶段 3 启用 any-motion feature）。
//
// 实现路线（路线 2，分阶段差异化）：
//   - 在线态（S0/S1/S2）：纯轮询 ACC，不加载 8KB config file，软件差分判拿起。
//   - 关机态（S3）：加载 config file + any-motion feature + INT1 输出，供 PYG4 唤醒 M5PM1。

// 初始化 BMI270：探测 CHIP_ID(0x24)、softreset、开 ACC、初始化轮询基线。
// 探测失败时置内部 s_present=false，后续接口安全降级（返回 false / ESP_OK 无操作）。
// 必须在 stick_s3_board_init() 之后调用（依赖 I2C 总线）。
esp_err_t bmi270_init(void);

// BMI270 是否在线（CHIP_ID 探测成功）。
bool bmi270_present(void);

// 轮询判定是否发生"拿起"动作。内部维护上次合加速度基线，本次合加速度幅值
// 相对基线变化超过阈值即判定为拿起，并更新基线避免连续重复触发。
// 返回 true 表示本次轮询检测到拿起。BMI270 不在线时恒返回 false。
bool bmi270_pickup_detected(void);

// [阶段 3 关机态用] 加载 8KB config file，配置 any-motion feature 与 INT1 输出，
// 使 IMU 在 M5PM1 关机后继续低功耗检测，翻转经 PYG4 唤醒 M5PM1。
// BMI270 不在线时返回 ESP_OK 无操作。
esp_err_t bmi270_enable_pickup_wake(void);
