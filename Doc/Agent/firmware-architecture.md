# 固件架构（组件、引脚、电源）

本文承载固件侧架构细节，2026-08 由根目录 `AGENTS.md`/`CLAUDE.md` 的「固件职责」章节迁入；根指南只保留职责边界一句话与指向本文的指针。

固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。关键组件（`firmware/components/` 下共 7 个）：

- `firmware/main/main.c`：主循环，编排按键、BLE、录音会话、UI 状态、电源管理和 OTA 事件。
- `components/audio_pipeline/`：从 ES8311 读取 16 kHz 单声道 PCM，经 HPF 与软件 AGC（v2.1.2 起：target -6 dBFS、max +20 dB、噪声门、0.8FS 瞬时限幅，硬件 ALC 已关闭）后编码为 Opus 交给 BLE 层；开头 60ms 静音+淡入、drain 尾帧淡出以消除按键音。
- `components/voice_ble/`：GATT 服务、通知、控制写入、BLE OTA。
- `components/ui_status/`：ST7789/LVGL 渲染、亮度、休眠、OTA 进度。
- `components/bmi270/`：BMI270 IMU 驱动。
- `components/mini_encoder_c/`：MiniEncoderC 编码器驱动（I2C @0x42，顶部 Hat 排针 SDA=G8/SCL=G0 第二路总线，按钮/旋转增量/SK6812 LED，轮询式；探测失败优雅降级）。
- `components/stick_s3_board/`：板级初始化，引脚定义在 `include/stick_s3_board.h`。
- `components/power_log/`：分模式功耗记账——纯观察组件，不改动电源状态机行为；记录模式切换事件与 60s VBAT 周期采样（RAM 环形缓冲 + SPIFFS `/storage/power_log.bin` 环形文件，M5PM1 RTC RAM 存跨 S3 关机锚点），经 `control_rx`/`state_tx` 的 `power_log` 命令导出，协议见 `Doc/Ref/protocol.md`，设计见 `Doc/Plan/power-mode-energy-profiling.md`。

## 板级硬件映射

| 硬件 | 引脚/接口 | 说明 |
|---|---|---|
| 主键（正面） | GPIO11 | 协议 `primary`，push-to-talk 与深度睡眠唤醒 |
| 侧键 | GPIO12 | 协议 `secondary`，取消/体感鼠标/恢复上一次输入确认 |
| PMIC IRQ | GPIO13 | 电源管理芯片中断 |
| LCD 背光 | GPIO38 | PWM 调光 |
| IMU | BMI270 | I2C，体感鼠标与敲击检测 |
| MiniEncoderC 编码器 | I2C @0x42，顶部 Hat 排针 SDA=G8 / SCL=G0（第二路 I2C 总线） | 按钮等价主键，旋转映射方向键，录音时亮红灯；不能作为深睡唤醒源 |
| 音频 codec | ES8311 | I2S，16 kHz / 16 bit / mono |
| 显示屏 | ST7789 | 135 × 240 竖屏，SPI |
