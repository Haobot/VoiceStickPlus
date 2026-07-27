#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// M5Stack MiniEncoderC（SKU U157）驱动：I2C @0x42，旋转编码器 + 按钮 + SK6812 LED。
//
// 硬件事实：
//   - 插在 StickS3 顶部 8pin Hat 排针，按接线图 SDA=G8、SCL=G0。
//   - 走第二路 I2C 总线（不占内部 G47/G48 总线），100 kHz。
//   - Grove 口 5V 保持不启用；编码器由 Hat 排针供电。
//   - I2C 外设，不能作为深睡唤醒源。
//
// 用法与 bmi270 一致：轮询式，组件内无线程无回调，timer 由 main.c 持有。
// 探测失败（含交换 SDA/SCL 线序重试一次后仍失败）时置 absent，后续接口安全降级。

// 建第二路 I2C 总线并探测 0x42。先按 G8=SDA/G0=SCL 探测，失败交换线序重试一次。
// 调用契约：仅在 stick_s3_board_init() 之后、轮询启动之前调用一次；非可重入。
esp_err_t mini_encoder_c_init(void);

// 编码器是否在线（init 探测成功且未因连续 I2C 失败降级）。
bool mini_encoder_c_present(void);

// 读按钮状态：寄存器 0x20，1 字节，低有效（0x00=按下，0x01=释放，真机验证）。
esp_err_t mini_encoder_c_read_button(bool *pressed);

// 读旋转增量：寄存器 0x10，int32 LE，读后清零语义（真机验证；
// 若不行则改读 0x00 计数器做软件差分，见 .c 中注释）。正值=顺时针（cw）。
esp_err_t mini_encoder_c_read_delta(int32_t *delta);

// 写 SK6812 LED 颜色：寄存器 0x30，写 3 字节 R,G,B。写失败静默忽略（不影响录音主链路）。
esp_err_t mini_encoder_c_set_led(uint8_t r, uint8_t g, uint8_t b);
