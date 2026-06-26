# Windows Settings 窗口显示已连接 Wi-Fi 信息 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 主设置窗口里增加一个配置开关，勾选后在 IMU 灵敏度选项下方显示当前已连接设备的 Wi-Fi SSID 与 IP，并把 SSID/IP 持久化到 `config.toml`。

**Architecture:** 扩展 `AppConfig` 保存 `show_device_wifi_info` 开关和每个设备的 `device_wifi_infos`；`Win32App` 在收到 `wifi_status` 时把 SSID/IP 写回配置；`SettingsDialog` 负责渲染复选框和两个只读文本框。

**Tech Stack:** C++20, Win32, ESP-IDF component style (C), CMake, CTest

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `desktop/windows/src/app_config.h` | 新增 `DeviceWifiInfo` 结构、`show_device_wifi_info` 与 `device_wifi_infos` 字段 |
| `desktop/windows/src/app_config.cc` | 读写 `show_device_wifi_info` 与 `[device.<id>.wifi_info]`；`RemovePairedDevice` 清理；增加 `Save(path)` / `Load(path)` 重载供测试 |
| `desktop/windows/tests/core_tests.cc` | 新增 `TestAppConfigWifiInfoRoundTrip` |
| `desktop/windows/src/localization.h` | 新增 `StringId` 枚举 |
| `desktop/windows/src/localization.cc` | 新增中英文文案 |
| `desktop/windows/src/settings_dialog.h` | 新增控件句柄、控件 ID、辅助方法声明 |
| `desktop/windows/src/settings_dialog.cc` | 创建/布局新控件、加载/保存开关状态、显示/隐藏 Wi-Fi 信息行 |
| `desktop/windows/src/win32_app.cc` | 在 `SetDeviceWifiStatus` 中持久化 SSID/IP |

---

### Task 1: 扩展 AppConfig 数据模型与 TOML 持久化

**Files:**
- Modify: `desktop/windows/src/app_config.h`
- Modify: `desktop/windows/src/app_config.cc`

- [ ] **Step 1: 在 `app_config.h` 中新增 `DeviceWifiInfo` 与配置字段**

在 `struct WifiDeviceProfile` 之后、`struct AppConfig` 之前插入：

```cpp
struct DeviceWifiInfo {
    std::string ssid;
    std::string ip;

    bool operator==(const DeviceWifiInfo& other) const = default;
};
```

在 `struct AppConfig` 中 `ImuWakeSensitivity imu_wake_sensitivity = ImuWakeSensitivity::kLow;` 之后插入：

```cpp
    bool show_device_wifi_info = false;
    std::map<std::string, DeviceWifiInfo> device_wifi_infos;
```

在 `void Save() const;` 下方新增重载：

```cpp
    void Save(const std::filesystem::path& path) const;
    static AppConfig Load(const std::filesystem::path& path);
```

- [ ] **Step 2: 在 `app_config.cc` 中实现 TOML 读写**

在 `ApplyConfigValue` 中 `imu_wake_sensitivity` 处理之后插入：

```cpp
    if (key == "show_device_wifi_info") config.show_device_wifi_info = BoolValue(value, config.show_device_wifi_info);
```

在 `AppConfig::Load()`（基于 toml::parse 的版本）中 `imu_wake_sensitivity` 读取之后插入：

```cpp
        if (auto value = TomlBool(table, "show_device_wifi_info")) config.show_device_wifi_info = *value;
```

在同函数的 `for (const auto& [key, node] : *devices)` 循环内、`wifi` 表解析之后插入：

```cpp
                if (const auto* wifi_info = (*device_table)["wifi_info"].as_table()) {
                    DeviceWifiInfo info;
                    if (auto value = TomlString(*wifi_info, "ssid")) info.ssid = Trim(*value);
                    if (auto value = TomlString(*wifi_info, "ip")) info.ip = Trim(*value);
                    config.device_wifi_infos[device_id] = std::move(info);
                }
```

在 `AppConfig::Save()` 中 `imu_wake_sensitivity` 输出之后插入：

```cpp
    output << "show_device_wifi_info = " << (show_device_wifi_info ? "true" : "false") << "\n";
```

在同函数 `device_wifi_profiles` 循环之后、`output << "\n[output]\n";` 之前插入：

```cpp
    for (const auto& [device_id, info] : device_wifi_infos) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        output << "\n[device." << device_id << ".wifi_info]\n";
        output << "ssid = \"" << TomlEscape(info.ssid) << "\"\n";
        output << "ip = \"" << TomlEscape(info.ip) << "\"\n";
    }
```

在 `AppConfig::RemovePairedDevice` 中 `device_wifi_profiles.erase(device_id);` 之后插入：

```cpp
    device_wifi_infos.erase(device_id);
```

- [ ] **Step 3: 增加 `Save(path)` 与 `Load(path)` 重载以支持测试**

把现有 `void AppConfig::Save() const { ... }` 函数体改为：

```cpp
void AppConfig::Save() const {
    Save(ConfigPath());
}

void AppConfig::Save(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open config for writing");
    }
    // 原 Save() 函数体从此处开始，把原有的 ConfigPath() 调用保持为 path 已处理
    ...
}
```

把现有 `AppConfig AppConfig::Load()` 函数体改为：

```cpp
AppConfig AppConfig::Load() {
    return Load(ConfigPath());
}

AppConfig AppConfig::Load(const std::filesystem::path& path) {
    AppConfig config = Defaults();
    std::ifstream input(path);
    if (!input) return config;

    try {
        auto table = toml::parse(input, path.native());
        // 原 Load() 函数体
        ...
        return config;
    } catch (const toml::parse_error&) {
        input.clear();
        input.seekg(0);
        return LoadLegacyConfig(input);
    }
    return config;
}
```

> 注意：原函数体中出现的 `ConfigPath()` 替换为 `path`；`toml::parse(input, ConfigPath().native())` 替换为 `toml::parse(input, path.native())`。

- [ ] **Step 4: 编译核心库检查签名**

Run:
```powershell
cmake -S desktop/windows -B desktop/windowsuild-x64 -G Ninja
cmake --build desktop/windowsuild-x64 --target voicestick_core
```

Expected: `voicestick_core` 编译成功，无签名/重载错误。

---

### Task 2: 为核心配置新增 round-trip 测试

**Files:**
- Modify: `desktop/windows/tests/core_tests.cc`

- [ ] **Step 1: 在 `core_tests.cc` 匿名命名空间中新增测试函数**

在 `TestAppConfig()` 函数之前或之后插入：

```cpp
void TestAppConfigWifiInfoRoundTrip() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_wifi_info_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids = {"5D74"};
    config.show_device_wifi_info = true;
    config.device_wifi_infos["5D74"] = DeviceWifiInfo{"newhome_iot", "192.168.3.160"};
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.show_device_wifi_info == true);
    assert(loaded.device_wifi_infos.contains("5D74"));
    assert(loaded.device_wifi_infos.at("5D74").ssid == "newhome_iot");
    assert(loaded.device_wifi_infos.at("5D74").ip == "192.168.3.160");

    std::filesystem::remove(temp);
}
```

- [ ] **Step 2: 在 `main()` 中调用新测试**

在 `TestAppConfig();` 调用之后插入：

```cpp
    TestAppConfigWifiInfoRoundTrip();
```

- [ ] **Step 3: 运行核心测试**

Run:
```powershell
ctest --test-dir desktop/windowsuild-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: 测试通过。

---

### Task 3: 新增本地化文案

**Files:**
- Modify: `desktop/windows/src/localization.h`
- Modify: `desktop/windows/src/localization.cc`

- [ ] **Step 1: 在枚举中新增 `StringId`**

在 `kSettingsImuWakeSensitivityHigh` 之后、`kSettingsDebugDir` 之前插入：

```cpp
    kSettingsShowDeviceWifiInfo,
    kSettingsDeviceWifiSsid,
    kSettingsDeviceWifiIp,
    kSettingsDeviceWifiIdle,
```

- [ ] **Step 2: 在英文表中填入文案**

在 `kSettingsImuWakeSensitivityHigh` 条目之后插入：

```cpp
    table[Index(StringId::kSettingsShowDeviceWifiInfo)] = "Show connected Wi-Fi info";
    table[Index(StringId::kSettingsDeviceWifiSsid)] = "Wi-Fi SSID";
    table[Index(StringId::kSettingsDeviceWifiIp)] = "IP Address";
    table[Index(StringId::kSettingsDeviceWifiIdle)] = "WIFI Idle";
```

- [ ] **Step 3: 在中文表中填入文案**

在 `kSettingsImuWakeSensitivityHigh` 条目之后插入：

```cpp
    table[Index(StringId::kSettingsShowDeviceWifiInfo)] = "显示已连接 Wi-Fi 信息";
    table[Index(StringId::kSettingsDeviceWifiSsid)] = "Wi-Fi SSID";
    table[Index(StringId::kSettingsDeviceWifiIp)] = "IP 地址";
    table[Index(StringId::kSettingsDeviceWifiIdle)] = "WIFI Idle";
```

- [ ] **Step 4: 运行测试确认文案表完整**

`LocalizationTablesAreComplete()` 已覆盖在 `TestAppConfig()` 中，运行：

```powershell
ctest --test-dir desktop/windowsuild-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: 测试通过，无缺失文案。

---

### Task 4: SettingsDialog 增加复选框与只读 SSID/IP 显示

**Files:**
- Modify: `desktop/windows/src/settings_dialog.h`
- Modify: `desktop/windows/src/settings_dialog.cc`

- [ ] **Step 1: 在头文件中声明新控件与方法**

在 `HWND imu_wake_sensitivity_combo_ = nullptr;` 之后插入：

```cpp
    HWND show_device_wifi_info_check_ = nullptr;
    HWND wifi_ssid_label_ = nullptr;
    HWND wifi_ssid_edit_ = nullptr;
    HWND wifi_ip_label_ = nullptr;
    HWND wifi_ip_edit_ = nullptr;
```

在 `static constexpr UINT kIdImuWakeSensitivity = 2018;` 之后插入：

```cpp
    static constexpr UINT kIdShowDeviceWifiInfo = 2019;
    static constexpr UINT kIdWifiSsidEdit = 2020;
    static constexpr UINT kIdWifiIpEdit = 2021;
```

在 `bool IsLabelControl(HWND control) const;` 之后新增私有方法声明：

```cpp
    void UpdateWifiInfoVisibility();
    std::pair<std::wstring, std::wstring> CurrentDeviceWifiInfoText() const;
```

- [ ] **Step 2: 增加窗口高度以容纳新控件**

把 `static constexpr int kClientHeight = 720;` 改为：

```cpp
    static constexpr int kClientHeight = 840;
```

- [ ] **Step 3: 在 `BuildControls` 中创建控件**

在 IMU 灵敏度行末尾（`y += row_h + Dp(10);`）之后、`remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsDebugDir)...)` 之前插入：

```cpp
    remember_label(CreateLabel(hwnd_, L"", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    show_device_wifi_info_check_ = remember(CreateButton(hwnd_,
        TrW(StringId::kSettingsShowDeviceWifiInfo, language).c_str(),
        ctrl_x, y, ctrl_w, Dp(22), kIdShowDeviceWifiInfo, instance_,
        BS_AUTOCHECKBOX));
    y += row_h + Dp(10);

    wifi_ssid_label_ = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsDeviceWifiSsid).c_str(),
                                                  Dp(10), y + Dp(3), label_w,
                                                  Dp(20), instance_));
    wifi_ssid_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(24),
                                          kIdWifiSsidEdit, instance_, ES_READONLY));
    y += row_h + Dp(10);

    wifi_ip_label_ = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsDeviceWifiIp).c_str(),
                                                Dp(10), y + Dp(3), label_w,
                                                Dp(20), instance_));
    wifi_ip_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(24),
                                        kIdWifiIpEdit, instance_, ES_READONLY));
    y += row_h + Dp(10);
```

- [ ] **Step 4: 处理复选框切换消息**

在 `HandleMessage` 的 `WM_COMMAND` switch 中 `case kIdImuWakeSensitivity:` 之前或之后插入：

```cpp
        case kIdShowDeviceWifiInfo:
            if (HIWORD(w_param) == BN_CLICKED) {
                UpdateWifiInfoVisibility();
            }
            return TRUE;
```

- [ ] **Step 5: 实现 `UpdateWifiInfoVisibility`**

在 `SettingsDialog::IsLabelControl` 之后、`SettingsDialog::Dp` 之前插入：

```cpp
void SettingsDialog::UpdateWifiInfoVisibility() {
    const bool show = SendMessageW(show_device_wifi_info_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(wifi_ssid_label_, cmd);
    ShowWindow(wifi_ssid_edit_, cmd);
    ShowWindow(wifi_ip_label_, cmd);
    ShowWindow(wifi_ip_edit_, cmd);
}
```

- [ ] **Step 6: 实现 `CurrentDeviceWifiInfoText`**

在 `UpdateWifiInfoVisibility` 之后插入：

```cpp
std::pair<std::wstring, std::wstring> SettingsDialog::CurrentDeviceWifiInfoText() const {
    const auto language = EffectiveUiLanguage(config_.ui_language);
    for (const auto& device_id : config_.paired_device_ids) {
        auto it = config_.device_wifi_infos.find(device_id);
        if (it != config_.device_wifi_infos.end()) {
            const auto& info = it->second;
            const std::wstring ssid = info.ssid.empty()
                                          ? TrW(StringId::kSettingsDeviceWifiIdle, language)
                                          : Utf16(info.ssid);
            const std::wstring ip = info.ip.empty() ? L"-" : Utf16(info.ip);
            return {ssid, ip};
        }
    }
    return {TrW(StringId::kSettingsDeviceWifiIdle, language), L"-"};
}
```

- [ ] **Step 7: 在 `LoadConfigIntoControls` 中加载状态与文本**

在 `SendMessageW(imu_wake_sensitivity_combo_, CB_SETCURSEL, sensitivity_index, 0);` 之后、`SetWindowTextW(debug_dir_edit_, ...)` 之前插入：

```cpp
    SendMessageW(show_device_wifi_info_check_, BM_SETCHECK,
                 config_.show_device_wifi_info ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto [ssid_text, ip_text] = CurrentDeviceWifiInfoText();
    SetWindowTextW(wifi_ssid_edit_, ssid_text.c_str());
    SetWindowTextW(wifi_ip_edit_, ip_text.c_str());
    UpdateWifiInfoVisibility();
```

- [ ] **Step 8: 在 `SaveSettings` 中保存复选框状态**

在 IMU 灵敏度保存代码块之后、`auto dir = GetWindowText(debug_dir_edit_);` 之前插入：

```cpp
    config_.show_device_wifi_info = SendMessageW(show_device_wifi_info_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
```

- [ ] **Step 9: 在 `DestroyControls` 和 `WM_DESTROY` 中清理新句柄**

在 `DestroyControls` 中 `imu_wake_sensitivity_combo_ = nullptr;` 之后插入：

```cpp
    show_device_wifi_info_check_ = nullptr;
    wifi_ssid_label_ = nullptr;
    wifi_ssid_edit_ = nullptr;
    wifi_ip_label_ = nullptr;
    wifi_ip_edit_ = nullptr;
```

在 `HandleMessage` 的 `WM_DESTROY` 分支中 `imu_wake_sensitivity_combo_ = nullptr;` 之后插入：

```cpp
        show_device_wifi_info_check_ = nullptr;
        wifi_ssid_label_ = nullptr;
        wifi_ssid_edit_ = nullptr;
        wifi_ip_label_ = nullptr;
        wifi_ip_edit_ = nullptr;
```

- [ ] **Step 10: 编译 `VoiceStickApp` 目标**

Run:
```powershell
cmake --build desktop/windowsuild-x64 --target VoiceStickApp
```

Expected: 编译成功。

---

### Task 5: Win32App 在收到 wifi_status 时持久化 SSID/IP

**Files:**
- Modify: `desktop/windows/src/win32_app.cc`

- [ ] **Step 1: 在 `SetDeviceWifiStatus` 中写回配置**

在 `Win32App::SetDeviceWifiStatus` 的 DispatchToUi lambda 内、更新 `device_wifi_status_map_` 之后、`auto it = wifi_settings_dialogs_.find(device_id);` 之前插入：

```cpp
        bool wifi_info_changed = false;
        auto& info = config_.device_wifi_infos[device_id];
        if (info.ssid != snapshot.ssid) {
            info.ssid = snapshot.ssid;
            wifi_info_changed = true;
        }
        if (info.ip != snapshot.ip) {
            info.ip = snapshot.ip;
            wifi_info_changed = true;
        }
        if (wifi_info_changed) {
            config_.Save();
        }
```

- [ ] **Step 2: 重新编译并运行测试**

Run:
```powershell
cmake --build desktop/windowsuild-x64 --target VoiceStickApp
ctest --test-dir desktop/windowsuild-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: 应用和测试均通过。

---

### Task 6: 完整构建与验证

- [ ] **Step 1: 完整构建 Windows 项目**

Run:
```powershell
build_win.bat
```

Expected: 构建成功，`desktop/windows/build-x64/VoiceStick.exe` 生成。

- [ ] **Step 2: 运行全部核心测试**

Run:
```powershell
ctest --test-dir desktop/windowsuild-x64 --output-on-failure
```

Expected: `voicestick_windows_tests` 通过。

- [ ] **Step 3: 手动验证**

1. 启动 `desktop\windows\build-x64\VoiceStick.exe`。
2. 配对/连接已有 Wi-Fi 的设备。
3. 托盘 → Settings → 勾选“显示已连接 Wi-Fi 信息”。
4. 确认 SSID 和 IP 显示正确。
5. 断开设备 Wi-Fi，确认 SSID 显示 `WIFI Idle`。
6. 取消勾选，确认 SSID/IP 行隐藏。
7. 忘记设备，检查 `%APPDATA%\VoiceStick\config.toml` 中对应 `[device.<id>.wifi_info]` 被删除。

---

## 自我检查

- **Spec 覆盖：**
  - 配置开关 → Task 1/4
  - 显示 SSID/IP → Task 4
  - 未连接显示 `WIFI Idle` → Task 4 `CurrentDeviceWifiInfoText`
  - 只持久化在 Windows → Task 1/5（只写 `config.toml`，不涉及 BLE 下发）
  - 显示在 IMU 下方 → Task 4 布局
- **无占位符：** 所有步骤包含具体代码/命令/期望输出。
- **类型一致性：** `DeviceWifiInfo`、`show_device_wifi_info`、`device_wifi_infos` 命名在 Task 1 定义后在 Task 4/5 中保持一致。
