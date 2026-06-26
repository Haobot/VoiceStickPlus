# Windows Settings 窗口显示已连接 Wi-Fi 信息

> 状态：已批准，待实现
> 日期：2026-06-26
> 范围：`desktop/windows/`

## 背景

固件已通过 BLE `state_tx` 的 `wifi_status` 事件上报设备 STA 的 SSID、IP、RSSI 等信息。Windows 端已在 `WifiSettingsDialog`（每设备独立窗口）中展示这些信息。

本设计在全局 `SettingsDialog` 中增加一个配置选项，使用户无需打开 per-device Wi-Fi 窗口也能在主设置里看到当前设备连上的 Wi-Fi SSID 和 IP。

## 目标

1. 在主 `SettingsDialog` 中增加“显示已连接 Wi-Fi 信息”复选框。
2. 复选框勾选后，在 IMU 灵敏度选项下方显示只读的 SSID 和 IP。
3. 当设备未连接 Wi-Fi 时，SSID 位置显示 `WIFI Idle`。
4. SSID/IP 信息只持久化在 Windows 端（`config.toml`），不写入设备。

## 非目标

- 不在固件侧增加新协议或新字段。
- 不做 per-device 的多行列表；主设置窗口只显示一个设备的信息。
- 不保存 Wi-Fi 密码（继续使用 Credential Manager）。

## 数据源

`VoiceStickCoordinator::HandleStateEvent` 中 `event.event == "wifi_status"` 分支已把 `WifiStatusSnapshot` 通过 `VoiceStickUi::SetDeviceWifiStatus` 交给 `Win32App`。

`Win32App::SetDeviceWifiStatus` 会把最新快照存入 `device_wifi_status_map_[device_id]`。我们将在此基础上把 SSID/IP 写入 `AppConfig` 并持久化。

## 配置模型

在 `AppConfig` 中新增：

```cpp
struct DeviceWifiInfo {
    std::string ssid;
    std::string ip;
};

bool show_device_wifi_info = false;
std::map<std::string, DeviceWifiInfo> device_wifi_infos;
```

`config.toml` 表现：

```toml
show_device_wifi_info = true

[device."5D74".wifi_info]
ssid = "newhome_iot"
ip = "192.168.3.160"
```

写入原则：

- 仅在 `wifi_status` 帧的 `ssid` 或 `ip` 发生变化时更新，避免 OTA 进度刷新导致频繁写盘。
- 仅在 SSID/IP 非空时写入；清空时保留空字符串占位，使 UI 能显示 `WIFI Idle`。
- `AppConfig::RemovePairedDevice` 同步删除对应 `device_wifi_infos[device_id]`。

## UI 布局

在 `SettingsDialog` 现有 IMU 灵敏度行（`imu_wake_sensitivity_combo_`）下方新增：

1. 复选框：`显示已连接 Wi-Fi 信息`（`kIdShowDeviceWifiInfo`）。
2. 勾选后出现的只读行：
   - 标签 `Wi-Fi SSID:` + 只读编辑框。
   - 标签 `IP 地址:` + 只读编辑框。

未勾选时隐藏/禁用下面的 SSID/IP 控件，保持窗口简洁。

## 显示规则

`SettingsDialog` 打开时从当前 `AppConfig` 决定显示内容：

1. 按 `paired_device_ids` 顺序，找第一个有 `device_wifi_infos` 条目的设备。
2. 如果该条目的 `ssid` 非空，显示 `ssid` 和 `ip`；否则 SSID 显示 `WIFI Idle`，IP 显示 `-`。
3. 如果没有任何配对设备或没有保存过 Wi-Fi 信息，显示 `WIFI Idle` / `-`。

> 注：典型场景下用户只有一台设备，因此“第一个有信息的配对设备”即为当前设备。多设备时显示规则明确、简单，不引入额外状态机。

## 数据流

```text
BLE state_tx wifi_status
  ↓
BleProtocol::ParseStateEvent
  ↓
VoiceStickCoordinator::HandleStateEvent
  ↓
VoiceStickUi::SetDeviceWifiStatus(device_id, snapshot)
  ↓
Win32App::SetDeviceWifiStatus
  ├─ 更新 device_wifi_status_map_[device_id]
  └─ 若 ssid/ip 变化 → 写入 config_.device_wifi_infos[device_id] → config_.Save()

SettingsDialog 打开/复选框切换
  ↓
从 config_ 读取 show_device_wifi_info 和 device_wifi_infos
  ↓
渲染复选框与 SSID/IP 只读文本
```

## 代码改动范围

### `voicestick_core`

- `app_config.h/.cc`
  - 新增 `DeviceWifiInfo`。
  - 新增 `show_device_wifi_info` 字段。
  - 新增 `device_wifi_infos` 字段。
  - 在 `Load`/`Save` 中读写 `show_device_wifi_info` 和 `[device.<id>.wifi_info]`。
  - `RemovePairedDevice` 中同步删除 `device_wifi_infos[device_id]`。
- `core_tests.cc`
  - 新增 `TestAppConfigWifiInfoRoundTrip`：验证 `show_device_wifi_info` 和 `device_wifi_infos` 的保存/加载。

### `VoiceStickApp`

- `localization.h/.cc`
  - 新增中英文文案：
    - `kSettingsShowDeviceWifiInfo`
    - `kSettingsDeviceWifiSsid`
    - `kSettingsDeviceWifiIp`
    - `kSettingsDeviceWifiIdle`
- `settings_dialog.h/.cc`
  - 新增 `show_device_wifi_info_check_` 控件（ID `2019`）。
  - 新增 `wifi_ssid_edit_`、`wifi_ip_edit_` 只读编辑框。
  - 在 `BuildControls` 中把新控件放在 IMU 灵敏度行下方。
  - 在 `LoadConfigIntoControls` 中加载复选框状态和 SSID/IP 文本。
  - 在 `SaveSettings` 中保存复选框状态。
  - 根据复选框状态显示/隐藏 SSID/IP 行。
- `win32_app.cc`
  - 在 `SetDeviceWifiStatus` 中把 SSID/IP 写入 `config_.device_wifi_infos` 并择机 `Save()`。

## 测试策略

- **Core tests**：`TestAppConfigWifiInfoRoundTrip` 覆盖 TOML 读写。
- **手动验证**：
  1. 打开 Settings，勾选“显示已连接 Wi-Fi 信息”。
  2. 连接设备并配网，确认 SSID/IP 显示正确。
  3. 断开 Wi-Fi，确认显示 `WIFI Idle` / `-`。
  4. 关闭设置，重启 App，确认复选框状态和上次信息仍保留。
  5. 取消勾选，确认 SSID/IP 行隐藏。
  6. 忘记设备，确认 `config.toml` 中对应 `[device.<id>.wifi_info]` 被删除。

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 多设备时显示的设备可能与用户预期不同 | 文档化“第一个有 Wi-Fi 信息的配对设备”规则；典型单设备场景无歧义。 |
| 频繁 `wifi_status` 进度帧导致反复写盘 | 仅在 `ssid`/`ip` 变化时写入。 |
| 复选框状态与显示行不同步 | `LoadConfigIntoControls` 统一处理；切换复选框时即时显示/隐藏。 |

## 实施顺序

1. `AppConfig` 新增字段与 TOML 读写。
2. `RemovePairedDevice` 清理 `device_wifi_infos`。
3. `core_tests.cc` 增加 round-trip 测试。
4. 新增本地化文案。
5. `SettingsDialog` 增加复选框与只读 SSID/IP 控件。
6. `Win32App::SetDeviceWifiStatus` 持久化 SSID/IP。
7. 构建并运行 CTest，然后实机验证。
