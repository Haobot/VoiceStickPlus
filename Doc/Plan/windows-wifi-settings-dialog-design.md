# Windows WifiSettingsDialog 设计

> 状态：已确认设计
> 日期：2026-06-25
> 范围：`desktop/windows/`；依赖已落地的 BLE 协议字段与固件 `voice_net`

## 背景

固件侧已具备完整的运维网络链路：

- BLE 下发 Wi-Fi STA 凭据；
- `wifi_status` 状态上报；
- mDNS / SNTP；
- HTTPS pull OTA；
- 局域网 HTTP pull OTA + SHA256 校验；
- rollback / `pending_verify` 健康签到。

Windows 桌面端已有协议层基础：

- `BleProtocol::WifiSetPayload` / `WifiClearPayload` / `WifiStatusRequestPayload` / `OtaPullPayload` / `OtaCommitPayload`；
- `StateEvent::wifi` 和 `WifiStatusSnapshot`；
- core tests 已覆盖 payload 与 `wifi_status` 解析。

本设计补齐 Windows UI：从托盘设备菜单打开 per-device 的 `WifiSettingsDialog`，完成配网、状态查看、OTA pull 与健康确认。

## 目标

1. 每个设备一个独立 Wi-Fi/OTA 设置窗口。
2. 从托盘菜单的设备子菜单进入。
3. 支持下发 Wi-Fi SSID / 密码，并显示固件回报的 `wifi_status`。
4. Windows 端用 Credential Manager 保存每设备 Wi-Fi 密码。
5. 支持手动填写 OTA URL + SHA256 并下发 `ota_pull`。
6. 支持 `ota_commit`，用于手动确认 pending_verify 健康状态。
7. 不在本轮实现一键本机 HTTP server、不自动选择 bin、不自动计算 SHA256。

## 非目标

- 不做 macOS UI；
- 不做 Linux UI；
- 不做设备端 HTTP server；
- 不在本轮做“选择 bin → 自动启动 HTTP server → 自动填 URL/SHA256”；
- 不修改固件协议。

## UI 入口

入口放在托盘菜单的每个 paired device 子菜单里：

```text
托盘菜单
└─ VS-D010
   ├─ Theme Color
   ├─ Theme Size
   ├─ Overlay Position
   ├─ Translation
   ├─ Firmware Update...
   ├─ Wi-Fi 与 OTA...       ← 新增
   └─ Forget Device
```

选择某个设备的 `Wi-Fi 与 OTA...` 后打开 `WifiSettingsDialog(device_id)`。

## UI 布局

采用单窗口分组布局，不引入 TabCtrl，与现有 `SettingsDialog` 的手动 `SetWindowPos` 风格一致。

### 顶部设备信息

显示：

- 设备名：`VS-D010`
- 设备 ID：`D010`
- BLE 状态：已连接 / 未连接
- 固件版本（如果 `DeviceInfo` 已知）

### Wi-Fi 凭据组

字段与按钮：

- SSID 输入框；
- 密码输入框（默认 password style）；
- “显示密码”复选框；
- “应用并连接”按钮；
- “清空凭据”按钮；
- “刷新状态”按钮。

行为：

- 点击“应用并连接”：
  1. 校验 SSID 非空且 ≤32；密码 ≤63；
  2. 保存 SSID 到 `config.toml` 的 `[device.<id>.wifi]`；
  3. 保存密码到 Windows Credential Manager；
  4. 调 `BleCentral::SendWifiSet(device_id, ssid, password)`；
  5. UI 进入 `connecting` 展示。
- 点击“清空凭据”：
  1. 调 `BleCentral::SendWifiClear(device_id)`；
  2. 删除 Credential Manager 中该设备密码；
  3. 清除 `config.toml` 中该设备 Wi-Fi SSID；
  4. UI 等待固件回 `disabled`。
- 点击“刷新状态”：调 `BleCentral::SendWifiStatusRequest(device_id)`。

### 当前状态组

只读展示最新 `WifiStatusSnapshot`：

- `state`：disabled / configured / connecting / connected / disconnected / error；
- SSID；
- IP；
- RSSI；
- `last_error` 本地化文案；
- `park_locked`；
- `ota_pending_verify`。

RSSI 显示建议：

| RSSI | UI |
|---|---|
| ≥ -65 | 绿色：信号良好 |
| -66 ~ -80 | 黄色：信号一般 |
| < -80 | 橙色/红色：信号弱，OTA 可能失败 |

### OTA 组

字段与按钮：

- OTA URL 输入框；
- SHA256 输入框；
- “开始 OTA Pull”按钮；
- OTA 状态行：state / progress / last_error；
- 进度条；
- “确认健康 (Commit)”按钮。

行为：

- URL 以 `http://` 开头时，SHA256 必填且必须为 64 位 hex；否则按钮禁用或点击后给本地错误；
- URL 以 `https://` 开头时，SHA256 可选；
- 其他 scheme 本地拒绝；
- 点击“开始 OTA Pull”：调 `BleCentral::SendOtaPull(device_id, url, sha256)`；
- `ota_pull.state=downloading/finishing/success/failed` 实时更新；
- `ota_pending_verify=true` 时显示红色横幅并启用 “确认健康 (Commit)”；
- 点击 Commit：调 `BleCentral::SendOtaCommit(device_id)`。

### 横幅与提示

- `ota_pending_verify=true`：红色横幅：“新固件等待健康确认。若未确认，下次重启可能回滚。”
- `park_locked=false`：黄色提示：“录音或 OTA 正在进行，暂不可升级。”
- HTTP URL 提示：“HTTP OTA 仅允许局域网私有 IP，且必须填写 SHA256。”
- RSSI 弱提示：“当前 Wi-Fi 信号弱，OTA 可能失败。建议靠近路由器。”

## Credential Manager 设计

新增一个小封装：`wifi_credentials_win.{h,cc}`。

Credential target：

```text
VoiceStick/Wifi/<device_id>
```

API：

```cpp
class WifiCredentialsWin {
public:
    static std::optional<std::wstring> ReadPassword(const std::string& device_id);
    static bool WritePassword(const std::string& device_id, const std::wstring& password);
    static void DeletePassword(const std::string& device_id);
};
```

实现使用 Windows Credential Manager：

- `CredReadW`
- `CredWriteW`
- `CredDeleteW`

CMake 链接：`advapi32` 已存在于 `VoiceStickApp`，如核心库也需要则给相关 target 补链接。

安全约束：

- 不把密码写入 `config.toml`；
- 不在日志、通知、错误文案里打印密码；
- 删除设备时同步删除该 Credential target。

## 配置存储

新增结构：

```cpp
struct WifiDeviceProfile {
    std::string ssid;
    std::string ota_url;
    std::string ota_sha256_hex;
};
```

`AppConfig` 增加：

```cpp
std::map<std::string, WifiDeviceProfile> device_wifi_profiles;
```

TOML：

```toml
[device."D010".wifi]
ssid = "newhome_iot"
ota_url = "http://192.168.3.96:8000/voice_stick.bin"
ota_sha256_hex = "77b11c..."
```

读写模式参考现有 `[device.<id>.output]`。

`RemovePairedDevice(device_id)` 时同步删除：

- `device_wifi_profiles[device_id]`
- Credential Manager target `VoiceStick/Wifi/<device_id>`

## 数据流

### 状态上报

```text
BLE state_tx wifi_status
  ↓
BleProtocol::ParseStateEvent → StateEvent::wifi
  ↓
VoiceStickCoordinator::HandleStateEvent
  ↓
VoiceStickUi::SetDeviceWifiStatus(device_id, snapshot)
  ↓
Win32App::device_wifi_status_map_[device_id]
  ↓
打开中的 WifiSettingsDialog::UpdateStatus(snapshot)
```

### 控制下发

```text
WifiSettingsDialog button
  ↓ callbacks
Win32App / VoiceStickCoordinator
  ↓
BleCentralWin::SendWifiSet / SendWifiClear / SendWifiStatusRequest / SendOtaPull / SendOtaCommit
  ↓
BleCentralWin::WriteControlPayloadAsync
  ↓
firmware control_rx
```

## 代码改动范围

### voicestick_core

- `voice_stick_coordinator.h/.cc`
  - `BleCentral` 接口新增：
    - `SendWifiSet`
    - `SendWifiClear`
    - `SendWifiStatusRequest`
    - `SendOtaPull`
    - `SendOtaCommit`
  - `VoiceStickUi` 接口新增：
    - `SetDeviceWifiStatus(device_id, WifiStatusSnapshot)`
  - `HandleStateEvent` 分发 `event == "wifi_status"`
- `app_config.h/.cc`
  - 新增 `WifiDeviceProfile`
  - 新增 `[device.<id>.wifi]` 读写
- `core_tests.cc`
  - 扩 fake UI / fake BLE；
  - 新增 coordinator 分发测试；
  - 新增 app_config wifi profile round-trip 测试。

### VoiceStickApp

- `ble_central_win.h/.cc`
  - 实现 5 个 Send 方法，调用 `BleProtocol` payload。
- `win32_app.h/.cc`
  - 托盘设备子菜单新增 `Wi-Fi 与 OTA...`；
  - 保存 `device_wifi_status_map_`；
  - 管理 `WifiSettingsDialog` 实例；
  - 实现 `SetDeviceWifiStatus`。
- `wifi_settings_dialog.h/.cc` 新增
  - 手动 Win32 dialog 模板；
  - 控件布局；
  - callbacks；
  - 状态刷新。
- `wifi_credentials_win.h/.cc` 新增
  - Credential Manager 读写删除。
- `localization.h/.cc`
  - 新增中英文文案。
- `CMakeLists.txt`
  - 加新源文件；
  - 如需要，补 `advapi32` 链接。

## 错误文案映射

| code | 中文文案 |
|---|---|
| `no_ssid` | 未找到该 Wi-Fi，请检查 SSID 或靠近路由器 |
| `auth_failed` | Wi-Fi 密码错误或认证超时 |
| `timeout` | 连接超时 |
| `payload_too_large` | 输入内容过长 |
| `ota_url_invalid` | OTA URL 不符合规则 |
| `ota_sha256_invalid` | SHA256 必须为 64 位十六进制 |
| `ota_sha256_mismatch` | 固件 SHA256 不匹配，已拒绝升级 |
| `ota_http_failed` | 下载固件失败 |
| `ota_validate_failed` | 固件镜像校验失败 |
| `ota_park_required` | 当前正在录音或升级，暂不可启动 OTA |

## 测试策略

### Core tests

新增/扩展：

- `TestCoordinatorDispatchesWifiStatusToUi`
- `TestBleCentralWifiPayloadForwarding`（如 fake central 可覆盖）
- `TestAppConfigWifiProfileRoundTrip`
- `TestWifiCredentialTargetName`（纯函数生成 target）

### 手动验证

1. 托盘 → VS-D010 → Wi-Fi 与 OTA...
2. 输入 SSID/密码 → 应用并连接
3. 设备回 `connecting` → `connected`，IP/RSSI 更新
4. 关闭窗口再打开，SSID 和密码从 config/Credential Manager 回填
5. 清空凭据 → 设备回 `disabled`，Credential 删除
6. 输入 HTTP OTA URL + SHA256 → 开始 OTA，观察进度与成功重启
7. pending_verify 出现时点击 Commit，横幅消失
8. 弱 RSSI 时显示提示

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| Credential Manager 读写失败 | UI 显示错误，但允许用户临时输入密码并下发；不阻塞状态查看 |
| BLE 写入无 ack | 点击按钮后启动超时提示；依赖固件后续 `wifi_status` 确认 |
| Wi-Fi 启动导致 BLE 单点断开 | UI 保持窗口打开，等待自动重连后的新 `wifi_status` |
| 状态帧丢失 | 提供“刷新状态”按钮，手动发 `wifi_status_request` |
| HTTP OTA URL/SHA 本地校验不完整 | 固件仍是最终安全边界；UI 只做提前提示 |
| 多个设备同时开窗口 | `Win32App` 用 device_id 管理窗口实例，状态按 device_id 分发 |

## 实施顺序

1. `BleCentral` / `BleCentralWin` 五个 Send 方法；
2. `VoiceStickUi::SetDeviceWifiStatus` + Coordinator 分发；
3. `AppConfig` 增加 `WifiDeviceProfile` 读写测试；
4. `WifiCredentialsWin` Credential Manager 封装；
5. `WifiSettingsDialog` 空壳 + 控件布局；
6. `Win32App` 托盘入口 + dialog 生命周期；
7. 接入 Wi-Fi 凭据下发 / 清空 / 刷新状态；
8. 接入 OTA URL/SHA / Commit；
9. 本地化文案；
10. Windows 测试 + 构建 + 实机联调。
