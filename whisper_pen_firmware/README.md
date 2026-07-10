# Whisper Pen 固件

复用 M5Stack StickS3 硬件的低语语音输入笔固件：ES8311 codec 采集 + NimBLE（128-bit 互通协议）+ Opus + ext1 唤醒。

> 本工程为独立精简实现，从 `firmware/`（StickS3 完整版）移植核心链路，去掉 LCD/IMU/OTA 状态机，专注采集+编码+BLE+PTT+power off。可直接 `idf.py build` 编译验证；音频/延迟参数需真机调参。

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
    └── ptt/                             hold_to_talk + 双击 + ext1 deep sleep
        ├── CMakeLists.txt
        ├── include/ptt.h
        └── ptt.c
```

## 改造来源（移植自 firmware/ 已踩坑实现）

| 组件 | 来源 | 说明 |
|---|---|---|
| stick_s3_board | 复制自 firmware/ | I2C(SCL48/SDA47) + PMIC + 引脚常量，零自定义依赖 |
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
| 6 | LSB_DEPTH 不是低语 SNR 关键 | `audio_pipeline.c` `init_opus` 如实设置 |
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

编译产物：`build/whisper_pen.bin`（约 815KB）。

## 待真机验证项

1. **codec 供电**：`stick_s3_board_init` 配 PMIC L3B 给 ES8311 供电；真机看 codec read 是否非零 PCM。
2. **PCM peak / Opus 帧大小**：日志每秒打印 `diag: pcm_peak= opus_avg=`，看采声是否正常、是否削波（peak 接近 32767）、Opus 帧大小合理（~20-80 字节）。
3. **stereo->mono**：`mono[i]=stereo[i*2]` 取左声道；若采到全 0 检查 ES8311 通道配置。
4. **I2S enable/disable 归属**：codec_open 前 enable、close 后只 del 不 disable；真机看无 "not enabled" ERROR。
5. **HPF 系数**：90Hz 针对低语爆破音，环境不同可重算。
6. **drain 帧数**：2 帧(80ms) 覆盖 60ms DMA 残留，真机验证松开是否丢尾音。
7. **ext1 唤醒 GPIO11**：power off 后按主键唤醒（reset 重走 app_main）。
8. **MTU/interval 实测**：与桌面端配对后看 `conn updated interval=` 是否到 6(7.5ms)、`mtu=` 是否 247。
9. **PSRAM 栈**：audio_task 24KB 栈放 PSRAM，看 `stack_hwm`。
10. **互通桌面端**：用现有 Voice Stick 桌面端配对，走完整 ASR 验证"说话->文字"全链路。
