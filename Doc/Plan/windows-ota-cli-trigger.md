# Windows 桌面端命令行触发 BLE OTA（--ota）

## 背景与目标

`ce2fd6c` 已加"从本地文件更新固件"GUI 入口（托盘菜单 -> 文件对话框）。但每次推送需手动点菜单选文件，无法被脚本/Claude 自动触发。

目标：支持 `VoiceStick.exe --ota <bin路径> [--device <设备ID>]`，一键发起 BLE OTA，无需 GUI 操作。

- **已运行实例**：命令行实例用 `FindWindowW` + `WM_COPYDATA` 把 `{path, device_id}` 转发给已运行实例，后者异步发起 OTA，命令行实例立即退出。
- **无运行实例**：命令行实例正常启动 app 并占住单例 Mutex，携带 pending OTA 请求；app 连上设备后自动发起。
- 进度仍由现有 `FirmwareUpdateDialog` 显示；自动化场景靠串口验证结果（GUI 子系统无 stdout）。

## 设计要点

### 命令行解析

`main.cc` 现有 `--relaunch`（提权重启）。新增 `--ota <path>` 与可选 `--device <id>`，用 `CommandLineToArgvW` 解析（`--relaunch` 用 `wcsstr` 是因无需取参数值，`--ota` 需取下一个 argv）。

抽一个自由函数便于单测：

```cpp
// 放在 main.cc 或新建 cmd_line.h/.cc（纳入 voicestick_core 以便测试）
struct OtaCliRequest {
    std::string file_path;
    std::optional<std::string> device_id;
};
std::optional<OtaCliRequest> ParseOtaCliArgs(int argc, wchar_t* argv[]);
```

### 跨进程转发：WM_COPYDATA

已运行实例窗口类名 `VoiceStickWindow`（`win32_app.cc:971`）。

```cpp
// main.cc：拿到 --ota 且检测到已有实例时
HWND existing = FindWindowW(L"VoiceStickWindow", nullptr);
if (existing) {
    std::string payload = req.file_path;
    if (req.device_id) payload += "\n" + *req.device_id;
    COPYDATASTRUCT cds{};
    cds.dwData = kOtaCopyDataId;  // 自定义标识，如 'VSOT'
    cds.cbData = payload.size() + 1;
    cds.lpData = payload.data();
    SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
    return 0;  // 转发完即退出
}
```

已运行实例 `HandleMessage` 加 `WM_COPYDATA` case：校验 `dwData==kOtaCopyDataId`，解析 payload（按 `\n` 拆 path / device_id），`DispatchToUi` 异步调 `StartOtaFromFile(path, device_id)`，立即返回（不阻塞 SendMessage）。

### 发起 OTA：StartOtaFromFile

现有 `StartFirmwareUpdateFromFile(device_id)`（弹文件对话框）。抽出"发起"部分为可复用方法：

```cpp
// win32_app.h
void StartOtaFromFile(const std::string& file_path,
                      const std::optional<std::string>& device_id);
```

逻辑：
1. 自动选设备：`device_id` 指定且在 `connected_devices_` 中 -> 用它；否则取 `connected_devices_` 第一个。
2. 无已连接设备 -> 弹气泡"无已连接设备"或记日志，放弃。
3. 弹 `FirmwareUpdateDialog`（version 标 "local file"）。
4. 调 `coordinator_->UpdateFirmwareFromFile(path, device_id, progress, completion)`（复用 `ce2fd6c` 的方法）。

`StartFirmwareUpdateFromFile` 改为：`GetOpenFileNameW` 选文件后调 `StartOtaFromFile`。

### 无运行实例：pending 请求

`main.cc` 拿到 Mutex 后，把 `req` 传给 `Win32App`（构造参数或 setter）。app 存 `pending_ota_request_`。在 `SetConnectedDevices` 回调里检查：若有 pending 且有匹配已连接设备，`DispatchToUi` 触发 `StartOtaFromFile`，清空 pending。

### 进度反馈

GUI 子程序无 stdout。两种反馈：
- **GUI**：`FirmwareUpdateDialog` 进度框（已运行实例转发后仍显示）。
- **自动化验证**：Claude 串口采集（设备切分区重启 + tap poll/IMU acc 消失确认新固件生效）。

暂不做控制台输出（AttachConsole 父进程），后续如需再加。

## 改动文件

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/main.cc` | 解析 `--ota`/`--device`；有实例则 WM_COPYDATA 转发退出；无实例则正常启动并传 pending |
| `desktop/windows/src/win32_app.h` | 加 `StartOtaFromFile`、`pending_ota_request_`、WM_COPYDATA 标识常量 |
| `desktop/windows/src/win32_app.cc` | `HandleMessage` 加 WM_COPYDATA；实现 `StartOtaFromFile`；`SetConnectedDevices` 检查 pending；`StartFirmwareUpdateFromFile` 复用之 |
| `desktop/windows/tests/core_tests.cc` | `ParseOtaCliArgs` 单测（有/无 --device、缺路径、多参数） |

WM_COPYDATA 跨进程部分靠真机验证，不单测。

## 边界与风险

- **单例竞态**：`--ota` 不带 `--relaunch`，走正常 Mutex 路径。无实例时拿 Mutex 正常启动；有实例时 `FindWindow` 转发后退出，不碰 Mutex（不与 [[voice-stick-relaunch-single-instance-race]] 的提权路径冲突）。
- **OTA 进行中再触发**：底层 `UpdateFirmware` 已返回 "A firmware update is already running."（`ble_central_win.cc:587`），`completion(false)` 兜底。
- **设备未连接**：`StartOtaFromFile` 无已连接设备时记日志放弃，不崩溃。
- **路径含中文/空格**：`CommandLineToArgvW` 正确处理引号；WM_COPYDATA 传 UTF-8 字节，跨进程安全。
- **FindWindow 找到非自家窗口**：用类名 `VoiceStickWindow`（非通用名），碰撞概率极低；WM_COPYDATA 处理时校验 `dwData==kOtaCopyDataId`。
- **`--ota` 与 `--relaunch` 同传**：提权场景不应带 `--ota`，解析时以 `--ota` 优先；实际不会出现。

## 验证步骤

1. **构建**：`build_win.bat`，核对 exe 时间戳/体积（[[windows-build-fake-success]]）。
2. **CTest**：`voicestick_windows_tests.exe`（`ParseOtaCliArgs` 测试）。
3. **有实例转发**（真机）：
   - VoiceStick.exe 已运行且设备已连接。
   - `VoiceStick.exe --ota firmware\build\voice_stick.bin`。
   - 预期：命令行实例立即退出，已运行实例弹进度框推送，设备重启。
   - 串口验证新固件生效。
4. **无实例自启动**（真机）：
   - 先关 VoiceStick.exe。
   - `VoiceStick.exe --ota <bin>` 启动，等 BLE 连上后自动发起。
5. **git**：`desktop/windows/` 被 ignore，`git add -f`（[[windows-gitignore-and-signing]]）。

## 非目标

- 不做控制台 stdout 进度输出（靠 GUI + 串口）。
- 不做 OTA 完成后自动退出 app（保持托盘常驻）。
- 不校验 bin 版本（沿用现有不校验语义）。
