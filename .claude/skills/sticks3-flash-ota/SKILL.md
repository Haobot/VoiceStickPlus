---
name: sticks3-flash-ota
description: >-
  给 M5Stack StickS3 / ESP32-S3 固件烧录与升级的标准流程。当用户说"烧固件 / 刷固件 / 刷机 /
  下载固件 / 烧录固件 / 给 stick 刷进去 / 串口烧录 / OTA 升级 / 推送固件 / BLE OTA /
  flash / 把改好的固件刷到设备 / 更新设备固件"等，或在改完 firmware/ 后需要把固件装到设备上验证时使用。
  涵盖两条路径：默认优先的 BLE 本地文件 OTA（命令行 --ota 或托盘菜单，免拔线免按键），
  以及回退的串口烧录（idf_cli.py，需人工按键进 Boot）。v1.8.0 已移除 VoiceStickCtl/HTTP OTA。
---

# StickS3 固件烧录与 OTA

把改好的固件装到 M5Stack StickS3（ESP32-S3）设备上的标准操作手册。两条路径，**默认优先 BLE 本地文件 OTA**。

## 决策：先选路径

```
固件已构建通过？
├─ 设备已 BLE 配对连接 + 桌面端 VoiceStick.exe 可用
│     -> ✅ 路径 A：BLE 本地文件 OTA（默认，免拔线免按键，最快）
│        ├─ 命令行：VoiceStick.exe --ota <bin>（自动化首选）
│        └─ GUI：托盘菜单"从本地文件更新固件..."
└─ 未配对 / 设备深睡连不上 / 改了分区表 / OTA 失败不可恢复 / 首次烧录新板
      -> 路径 B：串口烧录（需人工按键进 Boot）
```

只有在 A 不可用或失败不可恢复时才回退 B。详见根记忆 `firmware-http-ota-default`。

⚠️ **v1.8.0 已彻底移除 VoiceStickCtl 与 LAN HTTP OTA pull**（随 Wi-Fi 删除）。不要再调用 `VoiceStickCtl ota-pull`，该命令已不存在。固件升级只走 BLE OTA（远程 manifest / 本地文件）或 COM 口烧录。

---

## 路径 A：BLE 本地文件 OTA（默认优先）

把本地编译的 `firmware/build/voice_stick.bin` 经 BLE GATT OTA 推送给已连接设备，设备写 OTA 分区后切分区重启。**不用拔线、不用按键。** 底层 `BleCentralWin::UpdateFirmware` 接受任意字节流，OTA 协议发原始 app bin（`OtaBeginPayload(size)` -> 分块写 `ota_rx`），无额外封装头，不校验版本号。

### 前置条件

1. 固件已构建：`firmware/build/voice_stick.bin`（≤3MB）。
2. **VoiceStick.exe 已运行**且**设备已 BLE 连接**（设备屏幕非 Pairing 闪烁态，app 日志 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 出现 `stage=ready` / `connected VS-XXXX`）。

确认连接的两种方式：
- 读 app 日志末尾找 `connected VS-XXXX` + `stage=ready`。
- 或采集设备串口（COM 口，115200）找 `advertising as VS-XXXX`（在广播）或 `battery status`（已连）。

**设备未连接时 `--ota` 会转发给已运行实例但 `StartOtaFromFile` 因 `connected_devices_` 为空而放弃**（气泡提示"无已连接设备"）。必须先确保连接。

### A1：命令行触发（自动化首选）

```powershell
desktop\windows\build-x64\VoiceStick.exe --ota firmware\build\voice_stick.bin
```

可选指定设备（多设备时）：`--ota <bin> --device <4位ID>`（如 `D010`，VS-D010 的后四位）。

行为：
- **已运行实例**：命令行实例 `FindWindowW("VoiceStickWindow")` 找到已运行实例，用 `WM_COPYDATA` 转发 `{path, device_id}` 后立即退出（returncode 0）。已运行实例异步发起 OTA。
- **无运行实例**：正常启动 app 占住单例 Mutex，携带 pending 请求，连上设备后 `SetConnectedDevices` 回调自动触发。
- 进度仍由 `FirmwareUpdateDialog` 显示；自动化验证靠设备串口。

returncode 0 = 转发成功（不代表 OTA 完成，需看设备日志确认）。

### A2：GUI 触发

右键托盘 VoiceStick 图标 -> 设备子菜单 -> "从本地文件更新固件..." -> `GetOpenFileNameW` 选 bin -> 推送。

### 验证 OTA 成功

采集设备串口（115200），关注固件端 OTA 日志（`firmware/components/voice_ble/voice_ble.c`）：

```
voice_ble: OTA begin transfer=<id> size=1443264 partition=ota_1
voice_ble: OTA progress 32944/1443264 (2%)
...
voice_ble: OTA progress 1443264/1443264 (100%)
voice_ble: OTA complete transfer=<id>, rebooting
```

`OTA complete, rebooting` = 成功，设备切分区重启。重启后时间戳重置（从 0 开始），新固件生效。

### A 失败时的诊断

- **无反应（设备无 OTA 日志）**：app 未连上设备。读 app 日志确认 `stage=ready`；若没有，等连接或重启 app。命令行实例 returncode 0 只说明转发成功，不代表 OTA 触发。
- **`OTA aborted after disconnect`**：OTA 传输中 BLE 断连。检查设备距离/电量，重试。
- **设备切分区后连不上**：新固件可能未正常运行。串口烧录回退已知好版本。
- **app 日志找不到**：app 日志在 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（**不是** Roaming）。内含 `[BLE] connect stage` 全链路诊断。

---

## 路径 B：串口烧录（回退）

⚠️ **这块板的 Boot/复位/电源由前面板按钮电路接管，esptool 的自动复位无效，必须人工按键。** 详见根记忆 `stick-s3-button-boot-control`。

### 一键编译+烧录+监控

```sh
python scripts/idf_cli.py -cus
```

`idf_cli.py` 自动探测 ESP-IDF 环境、自动选串口（评分制，可 `-p COM17` 指定）。烧录 921600 / 监控 115200。单步：`-c` 编译、`-u` 烧录、`-s` 监控；`--list-ports` 列串口。

### 必须的人工按键时序（不能跳过）

1. **烧录前**：提示用户「请**长按**前面板按钮进入 Boot/下载模式」，等用户确认后再执行 `-u`。否则 esptool 识别不到芯片。
2. **烧录成功后**：提示用户「烧录完成，请**短按**前面板按钮重启」，等确认后再 `-s` 监控。否则芯片留在下载模式，串口能开但无数据（`waiting for download`）。
3. 看到「复位后串口完全无数据 / 读到 0 字节」时，**先怀疑是否漏了人工按键**，不要去调脚本。

### 分区表变更：首次需擦除重刷

设备从旧单应用分区表升级到当前 OTA 分区表时，普通 flash 不够，需先擦除（`idf_cli.py` 无 erase 选项，用原生 idf.py）：

```sh
idf.py -p COMxx erase-flash flash monitor
```

同样要先长按进 Boot。

---

## 速查：关键陷阱

| 现象 | 原因 / 处理 |
|---|---|
| `--ota` returncode 0 但设备无 OTA 日志 | app 未连上设备；读 app 日志确认 `stage=ready`，等连接或重启 app |
| app 日志找不到 | 在 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（非 Roaming） |
| 设备串口 0 字节、`waiting for download` | 漏了人工短按重启；esptool 自动复位在本板无效 |
| esptool 识别不到芯片 | 烧录前没长按进 Boot 模式 |
| 串口采空（设备运行中） | deep sleep 或重启时 USB 重新枚举致 pyserial 句柄失效；重开串口或等稳定 |
| 设备 OTA 重启后 BLE 连不上 | 设备进深睡（USB 未供电时）；USB 供电时不深睡（见 `deep-sleep-usb-logic-correct`）|
| 还在用 VoiceStickCtl ota-pull | v1.8.0 已移除；改用 `VoiceStick.exe --ota` 或托盘菜单 |

## 相关文件与记忆

- `scripts/idf_cli.py` / `scripts/idf_cli.yaml` -- 串口编译/烧录/监控
- `desktop/windows/src/voice_stick_coordinator.cc` -- `UpdateFirmwareFromFile`
- `desktop/windows/src/ble_central_win.cc` -- `UpdateFirmware` / `UpdateFirmwareAsync`（底层 OTA 推送）
- `desktop/windows/src/main.cc` -- `--ota` 命令行解析与 WM_COPYDATA 转发（用 `GetCommandLineW`）
- `firmware/components/voice_ble/voice_ble.c` -- 固件端 OTA 接收（`OTA begin`/`progress`/`complete` 日志）
- `Doc/Plan/windows-local-firmware-ota.md`、`Doc/Plan/windows-ota-cli-trigger.md` -- 设计方案
- 记忆：`firmware-flash-default`（四条路径）、`stick-s3-button-boot-control`（按键时序）、`app-ble-central-scan-failure`（app 日志位置）、`wwinmain-commandline-no-program-name`（--ota 解析坑）
