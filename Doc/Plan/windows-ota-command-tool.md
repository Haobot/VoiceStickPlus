# Windows 本地 OTA 命令工具方案

## 背景

固件已支持通过 BLE `control_rx` 接收 `ota_pull`，再由设备主动从局域网 HTTP/HTTPS 拉取 `voice_stick.bin`。Windows UI 的 Wi-Fi 与 OTA 窗口已验证可稳定发起 OTA，但开发调试仍需要人工填 SHA、点按钮。直接用 Python/bleak 抢 BLE 连接不稳定，因为 Windows 托盘应用已维护设备连接。

目标是提供一个 Claude/脚本可调用的本地命令入口，复用 Windows 应用已有 BLE 会话，让固件构建后的默认上传路径优先走 OTA。

## 命令接口

新增控制台程序 `VoiceStickCtl.exe`：

```powershell
VoiceStickCtl.exe ota-pull --device 5D74 --url http://192.168.3.96:8000/voice_stick.bin --sha256 <64位hex> --wait healthy --timeout 180
```

参数：

- `ota-pull`：发起设备 HTTP/HTTPS pull OTA。
- `--device`：设备短 ID，支持 `5D74` 或 `VS-5D74`。省略且只有一个已配对设备时自动选择。
- `--url`：OTA 镜像 URL。省略时读取 `[device.<id>.wifi].ota_url`。
- `--sha256`：镜像 SHA256。省略时读取 `[device.<id>.wifi].ota_sha256_hex`。
- `--wait success|healthy`：默认 `healthy`。`success` 等到设备报告 OTA 成功；`healthy` 继续等设备重启、重连并退出 pending verify。
- `--timeout`：总超时秒数，默认 180。
- `--json`：按 JSONL 输出进度，便于自动化解析。
- `--save-config`：显式把本次 URL/SHA 写回配置；默认不写，避免临时局域网地址污染配置。

退出码：

- `0`：成功。
- `2`：参数或配置错误。
- `3`：App/IPC 错误。
- `4`：设备、Wi-Fi 或 park 状态未 ready。
- `5`：固件 OTA 失败。
- `124`：超时。

## IPC

`VoiceStickCtl.exe` 不直接连接 BLE，而是通过本地 IPC 把请求交给 `VoiceStick.exe`。

流程：

1. 控制台工具查找 `FindWindowW(L"VoiceStickWindow", L"VoiceStick")`。
2. 找不到时启动同目录 `VoiceStick.exe`，等待窗口出现。
3. 控制台工具创建唯一 reply pipe，例如 `\\.\pipe\VoiceStick.Ota.<pid>.<request_id>`。
4. 通过 `WM_COPYDATA` 发送 UTF-8 JSON：

```json
{
  "action": "ota_pull",
  "request_id": "...",
  "reply_pipe": "\\\\.\\pipe\\VoiceStick.Ota....",
  "device_id": "5D74",
  "url": "http://192.168.3.96:8000/voice_stick.bin",
  "sha256_hex": "...",
  "wait": "healthy",
  "timeout_sec": 180,
  "json": false
}
```

5. App 连接 reply pipe 并持续写入进度行，最终写入 `done` 或 `error`。

## App 状态机

App 收到 IPC 后不阻塞 UI 线程，而是在 UI 消息循环中推进 pending command：

```text
received
 -> waiting_ble
 -> requesting_wifi_status
 -> waiting_wifi_ready
 -> sending_ota_pull
 -> waiting_ota_progress
 -> waiting_reboot
 -> waiting_reconnect
 -> waiting_healthy
 -> done / failed / timeout
```

关键规则：

- 发 OTA 前先调用 `VoiceStickCoordinator::RequestDeviceWifiStatus(device_id)`。
- 只有 Wi-Fi `state=connected`、IP 非空、`park=true` 时才发送 OTA。
- 发送时复用 `VoiceStickCoordinator::StartDeviceOtaPull(device_id, url, sha)`。
- 发 OTA 后必须看到本轮 `downloading` 或 `finishing`，才接受 `success`/`failed`，避免旧状态误判。
- 收到 `failed` 时返回固件 `ota_pull.last_error`。
- `--wait healthy` 下，`success` 后等待设备断开、重连，再请求 `wifi_status` 并等待 `pending=false`。

## 输出

默认文本输出示例：

```text
ready device=5D74 wifi=connected ip=192.168.3.161
ota downloading 5%
ota downloading 50%
ota finishing 100%
ota success
reconnecting
healthy pending=false partition=ota_1
```

JSONL 输出示例：

```json
{"event":"ready","device":"5D74","wifi":"connected","ip":"192.168.3.161"}
{"event":"ota_progress","state":"downloading","progress":50}
{"event":"ota_success"}
{"event":"healthy","pending":false,"partition":"ota_1"}
{"event":"done","ok":true}
```

## 复用代码

- `Win32App::ShowWifiSettings()` 已经把 UI 的开始 OTA 回调接到 coordinator。
- `VoiceStickCoordinator::StartDeviceOtaPull()` 是业务层入口。
- `BleCentralWin::SendOtaPull()` 负责设备 session ready 后写 control_rx。
- `BleProtocol::OtaPullPayload()` 构造标准 JSON payload。
- `AppConfig::device_wifi_profiles` 读取每设备 `ota_url` 与 `ota_sha256_hex`。
- `Win32App::SetDeviceWifiStatus()` 已接收并缓存 OTA 进度。

## 验证

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

端到端：

```powershell
python -m http.server 8000 --directory firmware\build

desktop\windows\build-x64\VoiceStickCtl.exe ota-pull `
  --device 5D74 `
  --url http://192.168.3.96:8000/voice_stick.bin `
  --sha256 <voice_stick.bin 的 SHA256> `
  --wait healthy `
  --timeout 180
```

期望：命令行显示下载进度、`success`、重连和 `healthy`；Windows UI 同步显示 `success 100%`；设备重启后 OTA 状态回到 `idle`。
