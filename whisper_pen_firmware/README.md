# Whisper Pen 固件脚手架

基于评估结论落地的完整 ESP-IDF 工程：ICS-41351 数字 MEMS + 裸 I2S（无 codec 层）+ NimBLE + Opus，手持式低语语音输入笔。

> 本工程为评估方案的参考实现，独立于现有 `firmware/`（StickS3 + ES8311）。结构上可直接 `idf.py build` 验证编译；音频/延迟参数需 ICS-41351 实物真机调参。

## 目录结构

```
whisper_pen_firmware/
├── CMakeLists.txt                      顶层 project()
├── sdkconfig.defaults                  PSRAM / NimBLE / 2M PHY / 分区表
├── partitions.csv                      单 app 分区（无 OTA）
├── main/
│   ├── CMakeLists.txt
│   └── main.c                          app_main 编排 + 空闲 deep sleep
└── components/
    ├── audio_pipeline/                  I2S 采集 + Opus 编码 + HPF + drain + 双核任务
    │   ├── CMakeLists.txt
    │   ├── idf_component.yml            依赖 78/esp-opus
    │   ├── include/audio_pipeline.h
    │   └── audio_pipeline.c
    ├── voice_ble/                       NimBLE 全套 + GATT + Opus 帧发送
    │   ├── CMakeLists.txt
    │   ├── include/voice_ble.h
    │   └── voice_ble.c
    └── ptt/                             hold_to_talk + 双击 + RTC deep sleep
        ├── CMakeLists.txt
        ├── include/ptt.h
        └── ptt.c
```

## 修正点对照（评估结论 -> 落地）

| # | 评估结论 | 落地位置 |
|---|---|---|
| 1 | complexity 5 偏激进，ESP32-S3 VOIP 用 1 | `audio_pipeline.c` `OPUS_COMPLEXITY` |
| 2 | 24bit->16bit 用硬件，删软件右移（源码已证实） | `audio_pipeline.c` `init_i2s`：`slot_bit_width=32` + `data_bit_width=16` |
| 3 | 上电常驻 I2S 与 <15uA 互斥，改按需启停 | `audio_pipeline_start/stop` 间 init/deinit；`ptt_enter_deep_sleep` |
| 4 | 低延迟头号参数是 connection interval，非 PHY | `voice_ble.c` `CONN_ITVL_FAST=6`(7.5ms) + 主动 MTU exchange + 2M PHY |
| 5 | 短按录音与双击冲突，hold_to_talk 为主 | `ptt.c` `PTT_HOLD_THRESHOLD_MS=300` + 双击窗口 |
| 6 | LSB_DEPTH 不是低语 SNR 关键 | `audio_pipeline.c` `init_opus` 注释，如实设置不寄望治低电平 |
| 7 | 缺软件 HPF 去爆破音 | `audio_pipeline.c` 二阶 Butterworth 90Hz |
| 8 | 缺 drain 尾音机制 | `audio_pipeline.c` `audio_task` drain 2 帧 + `stop` 同步等退出 |

### 关键技术证据：24bit->16bit 无需软件右移

评估第2点的核心论断已用 ESP-IDF v5.5.1 源码证实：

- `i2s_std.c:118` — DMA buffer 大小按 `data_bit_width` 计算：
  `buf_size = i2s_get_buf_size(handle, slot_cfg->data_bit_width, ...)`
- `i2s_common.c:423` — ESP32-S3 路径：`bytes_per_sample = (data_bit_width + 7) / 8`，即 16-bit 时每 sample 2 字节
- `i2s_common.c:1378` — `i2s_channel_read` 直接 `memcpy` DMA buffer 到 user buffer，**无位宽转换**
- `i2s_std.h:230-231` — `data_bit_width`=valid bits/sample，`slot_bit_width`=total bits/slot

结论：`slot_bit_width=32`(总宽) + `data_bit_width=16`(有效宽)，硬件从 32-bit slot 取高 16 位存入 DMA buffer，read 出来直接是 `int16_t[]`，可直接喂 `opus_encode`。方案的"软件右移 8 位"对左对齐 24-bit 数据是错的（丢高 8 位=截断衰减），已删除。

## 编译

```sh
cd whisper_pen_firmware
idf.py set-target esp32s3
idf.py build
```

需 ESP-IDF v5.5.x，组件管理器自动拉取 `78/esp-opus`。

## 待真机验证项（脚手架无法替代）

1. **ICS-41351 引脚**：`AP_PIN_BCLK/WS/DIN` 按实际板改；L/R 引脚决定左/右声道（`slot_mask`）。
2. **MONO 声道对齐**：默认采左声道，若 MEMS 输出在右声道改 `I2S_STD_SLOT_RIGHT`。
3. **warm-up 丢帧数**：`WARMUP_DROP_FRAMES=1`(40ms) 覆盖 MEMS 稳定，真机听首字是否干净再调。
4. **HPF 系数**：90Hz 针对低语爆破音，若环境不同重算（scripts 下可复现）。
5. **drain 帧数**：2 帧(80ms) 覆盖 60ms DMA 残留，真机验证松开是否丢尾音。
6. **ext0 唤醒 GPIO**：默认 GPIO4(=RTC_GPIO4)，按板选 RTC_GPIO；ESP32-S3 ext0 支持的 RTC GPIO 列表见数据手册。
7. **MTU/interval 实测**：与目标 central 配对后看 `conn updated interval=` 日志是否到 6(7.5ms)、`mtu=` 是否 247。
8. **PSRAM 栈**：audio_task 24KB 栈放 PSRAM，真机看 `stack_hwm` 是否够用。
