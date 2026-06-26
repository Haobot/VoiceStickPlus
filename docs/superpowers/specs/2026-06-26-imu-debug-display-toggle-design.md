# IMU 加速度调试数值显示开关

## 背景

VoiceStick 固件目前每 200 ms 读取一次 BMI270 三轴加速度，并刷新到屏幕顶部的 `s_imu_label`：

```c
snprintf(buf, sizeof(buf), "X:%+.2f g\nY:%+.2f g\nZ:%+.2f g", x_g, y_g, z_g);
ui_status_set_imu_text(buf);
```

该数值对终端用户没有实际意义，主要用于开发调试。当前行为会常驻显示，导致屏幕顶部被调试信息占据。用户希望默认隐藏，需要调试时可通过 Windows 桌面端设置打开。

## 目标

- 默认不在屏幕上显示 IMU 加速度数值。
- 通过 Windows 桌面端设置窗口提供一个可见复选框，需要调试时可打开。
- 开关状态通过 BLE `control_rx` 下发给固件，固件实时生效。
- 不持久化到固件 NVS：重启后恢复默认隐藏。
- 不影响方向自动旋转等仍依赖 IMU 数据的其他功能。

## 方案选择

### 方案 A：Windows 设置窗口持久化复选框（推荐）

- 在 `AppConfig` 中新增 `show_imu_debug` 布尔字段，保存到 `config.toml`。
- 设置窗口新增复选框；连接建立或配置变更时通过 BLE 下发 `show_imu_debug` 命令。
- 固件维护内存标志，控制是否把加速度值刷新到屏幕。
- **优点**：用户偏好持久化，体验与 `prompt_tone_enabled` 一致；发现性好。
- **缺点**：改动面稍大（Windows core + app + 固件 + 文档）。

### 方案 B：设置窗口临时复选框，不保存到 `config.toml`

- 仅在 `SettingsDialog` 中提供复选框，点击即发送 BLE 命令，但关闭窗口后状态丢失。
- **优点**：实现最小，不改配置解析。
- **缺点**：每次打开设置都要重新勾选，调试体验差。

### 方案 C：设备上下文菜单入口

- 在已连接设备的上下文菜单里加一项"显示 IMU 调试数值"。
- **优点**：不占用设置窗口空间。
- **缺点**：发现性差，与"配置选项"的表述不太吻合。

**推荐方案 A**。

## 设计细节

### 1. BLE 协议

在 `control_rx` 事件中新增：

```json
{"event":"show_imu_debug","enabled":true}
```

- `enabled` 为布尔值。
- 默认 `enabled=false`。
- 固件收到后立即更新内存标志，无需回复确认帧。

### 2. 固件端

修改 `firmware/main/main.c`：

- 新增静态变量：
  ```c
  static bool s_show_imu_debug = false;
  ```
- 在 `ble_control_cb` 中新增分支：
  ```c
  } else if (cJSON_IsString(event) && strcmp(event->valuestring, "show_imu_debug") == 0 &&
             cJSON_IsBool(enabled)) {
      s_show_imu_debug = cJSON_IsTrue(enabled);
      ESP_LOGI(TAG, "show_imu_debug %s", s_show_imu_debug ? "enabled" : "disabled");
  }
  ```
- 在 `imu_poll_timer_cb` 中：
  - 继续读取 BMI270 并调用 `update_display_orientation(x_g)`，保证方向自动旋转不受影响。
  - 仅在 `s_show_imu_debug` 为真时格式化 XYZ 并调用 `ui_status_set_imu_text(buf)`。
  - 为假时调用 `ui_status_set_imu_text("")`，清空顶部调试行。
  - 串口日志仍继续输出加速度值，便于没有屏幕时通过日志调试。

### 3. Windows 桌面端

#### 3.1 配置层

- `desktop/windows/src/app_config.h`：
  ```cpp
  bool show_imu_debug = false;
  ```
- `desktop/windows/src/app_config.cc`：
  - 解析 `config.toml` 中的 `show_imu_debug`。
  - 序列化时写出 `show_imu_debug = true/false`。

#### 3.2 协议层

- `desktop/windows/src/ble_protocol.h` / `.cc`：
  ```cpp
  static ByteVector ShowImuDebugPayload(bool enabled);
  ```
  生成 JSON：`{"event":"show_imu_debug","enabled":true/false}`。

- `desktop/windows/src/ble_central_win.h` / `.cc`：
  ```cpp
  void SendShowImuDebug(bool enabled,
                        const std::optional<std::string>& device_id);
  ```
  实现与 `SendPromptToneEnabled` 同构：按 `device_id` 定向发送或广播给所有已连接设备。

#### 3.3 协调器

- `desktop/windows/src/voice_stick_coordinator.cc`：
  - 在 `Start()` 的 `on_connection_change` 回调中，连接建立后调用 `ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt)`。
  - 在 `UpdateConfig()` 中调用 `ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt)`。

#### 3.4 设置窗口

- `desktop/windows/src/localization.h` / `.cc`：
  - 新增字符串 ID `kSettingsShowImuDebug`。
  - 英文："Show accelerometer debug values"
  - 中文："显示加速度调试数值"

- `desktop/windows/src/settings_dialog.h`：
  - 新增 `HWND show_imu_debug_check_ = nullptr;`
  - 新增控件 ID `kIdShowImuDebug`。

- `desktop/windows/src/settings_dialog.cc`：
  - 在 `debug_audio_check_` 附近创建复选框。
  - 在 `LoadConfigIntoControls()` 中根据 `config_.show_imu_debug` 设置勾选状态。
  - 在 `SaveSettings()` 中读取勾选状态写入 `config_.show_imu_debug`。
  - 窗口高度 `kClientHeight` 适当增加以容纳新控件。

### 4. 文档

- 更新 `Doc/Ref/protocol.md` 中 `control_rx` 事件表，加入 `show_imu_debug`。
- 在 `AGENTS.md` 的配置项示例列表中补充 `show_imu_debug`。

## 测试计划

- **固件**：运行 `idf.py build` 确保编译通过。
- **Windows**：运行 `build_win.bat` 构建，再运行 `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 通过核心测试。
- **手工验证**：
  1. 默认状态下设备屏幕顶部不显示 XYZ 加速度值。
  2. 在 Windows 设置窗口勾选"显示加速度调试数值"并保存，已连接设备屏幕立即显示数值。
  3. 取消勾选并保存，设备屏幕数值立即消失。
  4. 设备重启后恢复默认隐藏。
  5. 方向自动旋转功能在数值隐藏/显示两种状态下均正常工作。
