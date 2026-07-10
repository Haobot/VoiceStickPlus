# Whisper Pen 固件

复用 M5Stack StickS3 硬件的低语语音输入笔固件：ES8311 codec 采集 + NimBLE（128-bit 互通协议）+ Opus + ext1 唤醒 + LCD 显示。真机已验证全链路打通（详见末尾"真机验证结果"）。

> 本工程为独立精简实现，从 `firmware/`（StickS3 完整版）移植核心链路，去掉 IMU/OTA 状态机，专注采集+编码+BLE+PTT+power off+LCD。可直接 `idf.py build` 编译验证。

## 目录结构

```
whisper_pen_firmware/
├── CMakeLists.txt                      顶层 project()
├── sdkconfig.defaults                  PSRAM / NimBLE / 2M PHY / 分区表
├── partitions.csv                      单 app 分区（无 OTA）
├── idf_cli.yaml                        idf_cli.py 编译配置
├── main/
│   ├── CMakeLists.txt
│   └── main.c                          app_main 编排 + control_rx + 空闲 power off
└── components/
    ├── audio_pipeline/                  ES8311 采集 + Opus 编码 + HPF + drain + 双核任务
    │   ├── CMakeLists.txt              REQUIRES voice_ble stick_s3_board esp_codec_dev
    │   ├── idf_component.yml           esp-opus + esp_codec_dev
    │   ├── include/audio_pipeline.h
    │   └── audio_pipeline.c
    ├── voice_ble/                       NimBLE 128-bit 互通协议（复制自 firmware/）
    │   ├── CMakeLists.txt
    │   ├── include/voice_ble.h
    │   └── voice_ble.c                  1120 行，5 特征 + OTA
    ├── stick_s3_board/                  板级 I2C + PMIC + 引脚（复制自 firmware/）
    │   ├── CMakeLists.txt
    │   ├── include/stick_s3_board.h
    │   └── stick_s3_board.c
    ├── ui_status/                        ST7789 LCD + LVGL + 背光 + 猫图标（复制自 firmware/）
    │   ├── CMakeLists.txt               EMBED_FILES 5 个 cat 图标 bin
    │   ├── idf_component.yml            lvgl/lvgl 9.2.0
    │   ├── assets/                      5 个 cat_*_argb8888.bin
    │   ├── include/ui_status.h
    │   ├── ui_status.c
    │   └── ui_status_icons.{h,c}
    └── ptt/                             hold_to_talk + 双击 + ext1 deep sleep
        ├── CMakeLists.txt
        ├── include/ptt.h
        └── ptt.c
```

## 改造来源（移植自 firmware/ 已踩坑实现）

| 组件 | 来源 | 说明 |
|---|---|---|
| stick_s3_board | 复制自 firmware/ | I2C(SCL48/SDA47) + PMIC + 引脚常量，零自定义依赖 |
| ui_status | 复制自 firmware/ | ST7789 LCD + LVGL + 背光 + 5 猫图标，零自定义依赖 |
| voice_ble | 复制自 firmware/ | 128-bit 互通协议（5 特征），零自定义依赖；OTA 随组件带入但 main 不注册回调故不启用 |
| audio_pipeline 采集链路 | 移植自 firmware/ | ES8311 init_i2s/init_codec/采集/deinit，PGA=18dB+ALC，enable/disable 归属等踩坑点照搬 |
| audio_pipeline HPF/Opus/drain/双核 | 保留自脚手架 | 90Hz HPF + complexity 1 + drain 2帧 + PSRAM 栈 |
| ptt deep sleep | 移植自 firmware/ 路径 B | ext1 唤醒 + rtc_gpio_pullup + settle high + prepare_deep_sleep |

## 修正点对照（评估结论 -> 落地）

| # | 评估结论 | 落地位置 |
|---|---|---|
| 1 | complexity 5 偏激进，ESP32-S3 VOIP 用 1 | `audio_pipeline.c` `OPUS_COMPLEXITY` |
| 2 | ~~24bit->16bit 用硬件取高16~~ **已作废** | ES8311 直接 16-bit 输出，无需 slot_bit_width=32 技巧（见下） |
| 3 | 上电常驻 I2S 与 <15uA 互斥，改按需启停 | `audio_pipeline_start/stop` 间 init/deinit；`ptt_enter_deep_sleep` |
| 4 | 低延迟头号参数是 connection interval，非 PHY | `voice_ble.c` `CONN_ITVL_FAST=6`(7.5ms) + 主动 MTU exchange + 2M PHY |
| 5 | 短按录音与双击冲突，hold_to_talk 为主 | `ptt.c` `PTT_HOLD_THRESHOLD_MS=300` + 双击窗口 |
| 6 | LSB_DEPTH 不是低语 SNR 关键 | ~~`init_opus` 设 LSB_DEPTH(16)~~ 真机后已去掉，对齐 firmware（firmware 不设此参数） |
| 7 | 缺软件 HPF 去爆破音 | `audio_pipeline.c` 二阶 Butterworth 90Hz |
| 8 | 缺 drain 尾音机制 | `audio_pipeline.c` `audio_task` drain 2 帧 + `stop` 同步等退出 |

### 关于评估第2点（24bit->16bit）

原评估针对 ICS-41351 数字 MEMS（24-bit 左对齐于 32-bit slot），用 `slot_bit_width=32` + `data_bit_width=16` 硬件取高 16 位。改造为 StickS3（ES8311 codec）后**此论断不适用**：ES8311 直接配 16-bit 输出（`I2S_DATA_BIT_WIDTH_16BIT` + `I2S_SLOT_MODE_STEREO`），无 32-bit slot 容器技巧。原 ICS-41351 源码证据保留于下供参考。

> ICS-41351 版证据（已不适用，仅供参考）：
> - `i2s_common.c:423` - `bytes_per_sample = (data_bit_width + 7) / 8`
> - `i2s_common.c:1378` - `i2s_channel_read` 直接 memcpy 无位宽转换
> - 配 `slot_bit_width=32` + `data_bit_width=16`，硬件从 32-bit slot 取高 16 位

## 编译

```sh
cd whisper_pen_firmware
idf.py set-target esp32s3
idf.py build
```

需 ESP-IDF v5.5.x，组件管理器自动拉取 `78/esp-opus` 与 `espressif/esp_codec_dev`。或用封装脚本（自动注入 ESP-IDF 环境）：

```sh
python scripts/idf_cli.py --config whisper_pen_firmware/idf_cli.yaml -c
```

编译产物：`build/whisper_pen.bin`（约 1.39MB，含 LVGL + 图标资源）。

## 真机验证结果（2026-07-11，COM17 烧录 + StickS3 实物）

全链路打通并达到预期：屏幕亮、连 Windows 桌面端、按下录音识别到文字、火山 ASR 长句正常。启动日志实证：

```
I (748) main: Whisper Pen boot (StickS3, ES8311)
I (748) stick_s3_board: I2C probe ok + M5PM1 l3b_power=on   ✅ PMIC/L3B 供电
I (968) ui_status: display ready                              ✅ LCD+LVGL 初始化
I (1028) voice_ble: BLE initialized as VS-D010 + advertising  ✅ NimBLE 128-bit 互通
I (3018) voice_ble: connected handle=1 interval=6            ✅ 桌面端连接 + 7.5ms interval
I (3028) voice_ble: phy updated tx_phy=1 rx_phy=1            ✅ PHY 更新
I (4008) main: interaction_mode=hold_to_talk                 ✅ control_rx 下发生效
```

录音日志实证（243 帧/9.7 秒长录音）：`diag: pcm_peak=1201 opus_avg=160bytes`（采声正常、CBR 32kbps 帧大小正确）、`stack_hwm=3988`（32KB 栈用 28KB，稳定不溢出）。

### 真机过程修复的根因

改造从零编写时踩的坑，按 disciplined-execution 逐个用日志/源码证据查证修复：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 烧录后 USB 串口读不到任何日志 | 缺 USB JTAG 控制台配置（StickS3 无 UART 引出） | 补 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`+`UART_NUM=-1`（对齐 firmware） |
| 2 | MTU/msys 用默认值（256/12 非 247/200） | sdkconfig.defaults 行内 `#` 注释致 kconfgen 解析失败 | 删全部行内注释 |
| 3 | 录音约 10 秒后 `stack overflow in task audio_pipeline` | 24KB 栈在加 LVGL 后溢出 | 改 32768 对齐 firmware + 补 stack_hwm 日志 |
| 4 | 松开按钮丢最后 1-2 字（尾音） | tx_task 遇 sentinel 直接 break，不排空 drain 帧就发 audio_end | 改 `goto drain` 排空队列再发 audio_end（移植 firmware 逻辑） |
| 5 | 偶发断连（松开约 11 秒后）reason=8（supervision timeout） | 只开 `MODEM_SLEEP` 没开 `PM_ENABLE`，modem sleep 时主晶振管理异常致 BT 时钟漂移 | 补 PM_ENABLE + tickless + 240MHz + MAIN_XTAL_PU 全套（对齐 firmware） |

### 已确认非固件问题

- **腾讯 ASR 长句只识别一句**：切火山 ASR 测同一固件长句正常，证明是腾讯 ASR 的 VAD（`needvad=1` 停顿切句后不续识别）行为，非固件音频质量问题。音频帧实测完整到达 ASR（Windows 日志 `audio frame slow` 是 >1000us 采样日志，seq 跳跃非丢帧）。

### 诊断日志

录音期间每秒打印一条（INFO 级）：
- `audio: diag: pcm_peak=<N> opus_avg=<N>bytes (<N> frames)` — 看采声/削波/编码
- 松开时：`audio task exit: enqueued=<N> overflow_drops=<N> stack_hwm=<N>` + `tx task exit: sent=<N> dropped=<N>`

### 待后续验证

- **ext1 唤醒 GPIO11**：power off 后按主键唤醒（代码已移植 firmware 路径 B，未实测深睡唤醒）
- **HPF 系数**：90Hz 针对低语爆破音，环境不同可重算
