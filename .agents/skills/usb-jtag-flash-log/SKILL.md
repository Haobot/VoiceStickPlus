---
name: usb-jtag-flash-log
description: >-
  ESP32-S3 USB JTAG/serial debug unit 的固件烧录与运行时日志采集方法。
  当涉及"USB JTAG 烧录 / COM 口烧录 / 串口烧录 / 烧录后读不到日志 / 设备没启动 /
  烧录后串口 0 字节 / 抓启动日志 / 抓运行时日志 / DTR 复位 / 监控串口 /
  esptool 自动复位无效 / 进 Boot 模式"等，或用 idf_cli.py -u 烧录后需要抓日志验证时使用。
  涵盖 USB JTAG 控制台配置、烧录前后的按键操作、DTR 软复位抓 boot 日志、
  运行时日志采集的陷阱与绕行。是 sticks3-flash-ota 路径B（串口烧录）的深化补充。
---

# USB JTAG 固件烧录与日志采集

ESP32-S3（M5Stack StickS3）的 USB 是 **JTAG/serial debug unit**（VID 303A:1001），既是烧录通道也是日志通道。烧录用 idf_cli.py / esptool，日志用 pyserial。本 skill 聚焦 USB JTAG 的烧录操作与日志采集陷阱。

## 前置：USB JTAG 控制台配置（不加则读不到日志）

StickS3 **无独立 UART 引出**，日志走 USB JTAG serial 通道。固件 sdkconfig 必须显式配置，否则 USB 串口读不到任何启动日志（会误判设备没启动）。

```ini
# sdkconfig.defaults（对齐 firmware）
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_ESP_CONSOLE_UART_NUM=-1
```

⚠️ **不能用 `CONFIG_ESP_CONSOLE_UART_NONE=y`**：它与 `USB_SERIAL_JTAG` 互斥，会导致 `CONFIG_ESP_CONSOLE_NONE=y` 生效（控制台完全禁用）。正确做法是设 `UART_NUM=-1`（禁用 UART 控制台但保留 USB JTAG）。

验证配置生效（改了 defaults 必须删 sdkconfig 重新生成）：
```sh
grep "ESP_CONSOLE_USB_SERIAL_JTAG=" sdkconfig   # 应 =y
```

## 烧录流程

### 1. 进 Boot 模式（进 Boot 可自动，出 Boot 必须手动）

USB JTAG 烧录区分两个方向，可靠性不同：

| 方向 | esptool 自动 | 说明 |
|---|---|---|
| **进 Boot（烧录前，--before）** | ✅ 可自动 | USB JTAG 下 esptool `default_reset` 能软复位进下载模式，**不需手动长按**（设备正常运行时） |
| **出 Boot（烧录后，--after）** | ❌ 必须手动 | `hard_reset` 在本板无效（reset 线被按钮电路接管），**必须短按重启** |

**进 Boot 可跳过长按的前提**：设备处于 USB JTAG 可达的正常运行状态（在跑 app / 正常连接广播）。以下情况仍需手动长按进 Boot：
- 设备在 deep sleep（USB JTAG 不可达，需先唤醒或长按）
- 设备卡死 / USB 断连
- 首次烧录新板 / 分区表变更（长按更稳）

日常快速迭代（设备刚跑着）可直接 `-u` 跳过长按；设备状态不明或关键烧录仍长按进 Boot。

⚠️ esptool 报"成功"不代表出 Boot 自动复位生效，烧录后仍需短按重启。

### 2. 烧录命令

```sh
# 从仓库根目录（whisper_pen_firmware 有独立 idf_cli.yaml）
python scripts/idf_cli.py --config <工程>/idf_cli.yaml -u -p COM17

# 主固件（firmware/）用默认 config
python scripts/idf_cli.py -u -p COM17
```

`idf_cli.py` 自动注入 ESP-IDF 环境、自动选串口（可 `-p COMxx` 指定）。烧录 921600 baud。单步：`-c` 编译、`-u` 烧录、`-s` 监控、`-cus` 编译+烧录+监控。

### 3. 烧录后重启

⚠️ **esptool 自动复位在 StickS3 上无效**，烧录后设备留在下载模式。必须：

```
烧录成功后提示：「请短按前面板按钮重启」，等用户确认后再抓日志。
```

不短按的话，串口能开但无数据（`waiting for download`）。

### 分区表变更需先擦除

从旧分区表升级到新分区表时，普通 flash 不够，需先擦除（idf_cli.py 无 erase 选项，用原生 idf.py）：

```sh
idf.py -p COMxx erase-flash flash monitor
```

同样要先长按进 Boot。

## 日志采集

### 方法 A：DTR 软复位抓 boot 日志（最可靠）

烧录后短按重启的瞬间，boot 日志就刷过去了。用 pyserial 的 DTR 拉低-拉高软复位，可稳定抓到完整启动日志：

```python
import serial, time
s = serial.Serial('COM17', 115200, timeout=0.5)
s.dtr = False; time.sleep(0.1); s.dtr = True; time.sleep(0.3); s.dtr = False
# 现在设备复位，开始读 boot 日志
end = time.time() + 15
while time.time() < end:
    data = s.read(512)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
s.close()
```

⚠️ DTR 复位会触发二次复位（ROM bootloader 日志出现两次），第二次 boot 会覆盖第一次的后续日志。抓启动阶段日志足够，抓运行时事件不可靠。

### 方法 B：纯监听抓运行时事件

设备已在跑 app 时，不复位直接监听，等事件触发产生日志：

```python
s = serial.Serial('COM17', 115200, timeout=0.5)
# 等用户操作（按键/录音/断连）产生日志
```

⚠️ **运行时日志采集不稳定**，以下情况会读 0 字节（非设备挂死）：
- Core1 / PSRAM 栈任务的 ESP_LOG 输出（如 audio_task）经常读不到
- 设备无活动时不输出（正常，等事件）
- 烧录后 USB 枚举状态可能需重新打开串口

验证 USB 通道是否工作：加一个 Core0 主循环心跳日志（如每 30 秒 `ESP_LOGI(TAG, "alive")`），能读到心跳说明通道正常。

### 方法 C：关键数据绕过 USB 日志（最可靠）

当 USB 抓不到关键事件（如 disconnect reason），用**持久化 + 上报**绕行：

1. 事件发生时写 NVS（如断连 reason 存 `ble_diag/dc_reason`）
2. 下次连接后通过 BLE state_tx 发出（如 `{"event":"last_disconnect","reason":N}`）
3. 对端（Windows）日志记录，从 Windows 日志读

这比反复重试抓 USB 日志可靠得多。

## Windows 端日志（重要证据源）

设备侧抓不到时，Windows 端 BLE 日志是重要补充：

```
%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log   （注意是 LocalAppData 非 Roaming）
```

记录了 BLE 连接/断连时序、state notify 内容、ASR 响应、audio frame 等。grep 关键字定位事件。

⚠️ **区分采样日志 vs 全量日志**：`audio frame slow` 只在处理时间 >1000us 时打印（`voice_stick_coordinator.cc:1165`），seq 跳跃是快帧没打日志**非丢帧**。判断丢帧前先确认日志是否采样。

## 常见问题

| 现象 | 原因 | 解决 |
|---|---|---|
| 烧录后 USB 串口读不到任何日志 | 缺 USB JTAG 控制台配置 | 补 `ESP_CONSOLE_USB_SERIAL_JTAG=y`+`UART_NUM=-1` |
| 烧录报成功但设备无响应 | esptool 自动复位无效，设备留在下载模式 | 短按前面板按钮重启 |
| 串口 0 字节、`waiting for download` | 没短按重启 | 短按前面板按钮 |
| esptool 识别不到芯片 | 设备不可达（深睡/卡死/USB断连）或首次烧录 | 长按前面板按钮进 Boot；日常设备跑着时可跳过 |
| 改了 sdkconfig.defaults 不生效 | ESP-IDF 不覆盖已有 sdkconfig | 删 sdkconfig 重新生成 |
| `UnicodeDecodeError: 'gbk'` | defaults 含中文/非 ASCII | 改英文，配置行内不留注释 |
| 运行时日志读 0 字节但设备能工作 | Core1/PSRAM 任务日志不稳定 | 用 DTR 复位抓 / NVS 上报 / Windows 日志 |

## 相关

- `sticks3-flash-ota` skill：BLE OTA 路径（默认优先）与本 skill 串口路径互补
- `scripts/idf_cli.py`：烧录/编译/监控封装（自动注入 ESP-IDF 环境）
- 记忆：`stick-s3-button-boot-control` / `esp32-usb-jtag-runtime-log-trap` / `firmware-serial-log-capture`
