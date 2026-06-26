# IMU 拿起/晃动亮屏灵敏度设置 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 设置窗口新增 Low/Medium/High 三档 IMU 拿起/晃动灵敏度，通过 BLE 下发具体阈值给固件，并在固件 NVS 持久化，刷机后实测可微调。

**Architecture:** Windows 端保存档位并换算成 LSB 阈值下发；固件端把阈值用于 `bmi270_pickup_detected()` 的合加速度变化判定，并写入 NVS 使设备重启后恢复。整体沿用现有 `show_imu_debug` / `prompt_tone_enabled` 的控制命令模式。

**Tech Stack:** ESP-IDF v5.5.1 (C), Windows C++20 / Win32 / C++/WinRT, CMake/Ninja, BLE control_rx JSON.

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `firmware/components/bmi270/include/bmi270.h` | 声明 `bmi270_set_pickup_threshold` / `bmi270_get_pickup_threshold` |
| `firmware/components/bmi270/bmi270.c` | 用运行时可写阈值替换硬编码宏 |
| `firmware/main/main.c` | NVS 读写阈值、BLE `imu_wake_sensitivity` 事件解析、启动时加载阈值 |
| `desktop/windows/src/app_config.h` | 新增 `ImuWakeSensitivity` 枚举与 `AppConfig` 字段 |
| `desktop/windows/src/app_config.cc` | 解析/序列化灵敏度档位，阈值换算辅助函数 |
| `desktop/windows/src/ble_protocol.h/.cc` | 新增 `ImuWakeSensitivityPayload(int threshold_lsb)` |
| `desktop/windows/src/voice_stick_coordinator.h` | `BleCentral` 接口新增 `SendImuWakeSensitivity` |
| `desktop/windows/src/voice_stick_coordinator.cc` | 连接成功/配置更新时转发阈值 |
| `desktop/windows/src/ble_central_win.h/.cc` | 实现 `SendImuWakeSensitivity` |
| `desktop/windows/src/settings_dialog.h/.cc` | 设置窗口新增灵敏度下拉框 |
| `desktop/windows/src/localization.h/.cc` | 新增本地化文案 |
| `desktop/windows/tests/core_tests.cc` | 更新 `FakeBleCentral`，补充档位解析/协议payload测试 |
| `Doc/Ref/protocol.md` | 更新 Control Event 表格 |

---

### Task 1: 固件 bmi270 阈值运行时 API

**Files:**
- Modify: `firmware/components/bmi270/bmi270.c:87-89`
- Modify: `firmware/components/bmi270/bmi270.c:386-410`
- Modify: `firmware/components/bmi270/bmi270.c:411-451` 之后
- Modify: `firmware/components/bmi270/include/bmi270.h:29-33`

- [ ] **Step 1: 删除硬编码宏，改为静态变量**

在 `bmi270.c` 中，把
```c
// 拿起判定阈值：合加速度幅值变化量（单位：LSB）。
// 降低到 800 LSB（~0.2g）增强灵敏度，并配合首次基线建立后小幅运动也触发。
#define BMI270_PICKUP_DELTA_THRESHOLD 800.0f
```
替换为
```c
// 拿起判定阈值：合加速度幅值变化量（单位：LSB）。
// 默认 800 LSB（~0.2g），可通过 bmi270_set_pickup_threshold 运行时调整。
static float s_pickup_threshold = 800.0f;
```

- [ ] **Step 2: 在判定函数中使用运行时可写阈值**

把 `bmi270_pickup_detected()` 中
```c
    if (delta >= BMI270_PICKUP_DELTA_THRESHOLD) {
```
改为
```c
    if (delta >= s_pickup_threshold) {
```

- [ ] **Step 3: 新增 setter/getter 实现**

在 `bmi270.c` 文件末尾、`bmi270_enable_pickup_wake()` 之后追加：
```c
void bmi270_set_pickup_threshold(float threshold_lsb)
{
    if (threshold_lsb < 0.0f) {
        threshold_lsb = 0.0f;
    }
    s_pickup_threshold = threshold_lsb;
    ESP_LOGI(TAG, "pickup threshold set to %.0f LSB", s_pickup_threshold);
}

float bmi270_get_pickup_threshold(void)
{
    return s_pickup_threshold;
}
```

- [ ] **Step 4: 在头文件中声明新 API**

在 `firmware/components/bmi270/include/bmi270.h` 中 `bmi270_read_acc_g` 声明之后追加：
```c
// 设置/读取拿起判定阈值（单位：LSB）。BMI270 不在线时写入仍会保存，后续上线生效。
void bmi270_set_pickup_threshold(float threshold_lsb);
float bmi270_get_pickup_threshold(void);
```

---

### Task 2: 固件 NVS 阈值持久化辅助函数

**Files:**
- Modify: `firmware/main/main.c:1-28`（新增 include）
- Modify: `firmware/main/main.c:195-205` 附近（新增辅助函数）

- [ ] **Step 1: 添加 nvs 头文件**

在 `main.c` 顶部 `#include` 区域追加：
```c
#include "nvs.h"
```

- [ ] **Step 2: 实现 NVS 读写函数**

在 `main.c` 的 `is_park_locked_for_ota()` 之后、其他静态函数之前插入：
```c
#define PICKUP_THRESHOLD_NVS_NAMESPACE "voicestick"
#define PICKUP_THRESHOLD_NVS_KEY       "pickup_thr"
#define PICKUP_THRESHOLD_DEFAULT_LSB   800
#define PICKUP_THRESHOLD_MIN_LSB       50
#define PICKUP_THRESHOLD_MAX_LSB       2000

static void load_pickup_threshold_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PICKUP_THRESHOLD_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "pickup threshold not found in nvs, using default %d", PICKUP_THRESHOLD_DEFAULT_LSB);
        bmi270_set_pickup_threshold((float)PICKUP_THRESHOLD_DEFAULT_LSB);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(err));
        bmi270_set_pickup_threshold((float)PICKUP_THRESHOLD_DEFAULT_LSB);
        return;
    }

    int32_t threshold = PICKUP_THRESHOLD_DEFAULT_LSB;
    err = nvs_get_i32(handle, PICKUP_THRESHOLD_NVS_KEY, &threshold);
    nvs_close(handle);

    if (err == ESP_OK) {
        if (threshold < PICKUP_THRESHOLD_MIN_LSB) {
            threshold = PICKUP_THRESHOLD_MIN_LSB;
        } else if (threshold > PICKUP_THRESHOLD_MAX_LSB) {
            threshold = PICKUP_THRESHOLD_MAX_LSB;
        }
        ESP_LOGI(TAG, "pickup threshold loaded from nvs: %" PRId32 " LSB", threshold);
        bmi270_set_pickup_threshold((float)threshold);
    } else {
        ESP_LOGW(TAG, "read pickup threshold failed: %s", esp_err_to_name(err));
        bmi270_set_pickup_threshold((float)PICKUP_THRESHOLD_DEFAULT_LSB);
    }
}

static void save_pickup_threshold_to_nvs(int32_t threshold)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PICKUP_THRESHOLD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open rw failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i32(handle, PICKUP_THRESHOLD_NVS_KEY, threshold);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save pickup threshold failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "pickup threshold saved to nvs: %" PRId32 " LSB", threshold);
    }
}
```

---

### Task 3: 固件 BLE 控制解析 + 启动加载

**Files:**
- Modify: `firmware/main/main.c:645-730`（`ble_control_cb`）
- Modify: `firmware/main/main.c:1472-1480`（`app_main` 中 `voice_ble_init` 之后）

- [ ] **Step 1: 解析 `imu_wake_sensitivity` 事件**

在 `ble_control_cb` 的局部变量声明区域，`
const cJSON *enabled = ...` 之后追加：
```c
    const cJSON *threshold_json = cJSON_GetObjectItemCaseSensitive(root, "threshold");
```

在 `show_imu_debug` 分支之后、`battery_status_request` 分支之前插入：
```c
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "imu_wake_sensitivity") == 0 &&
               cJSON_IsNumber(threshold_json)) {
        int32_t threshold = (int32_t)threshold_json->valueint;
        if (threshold < PICKUP_THRESHOLD_MIN_LSB) {
            threshold = PICKUP_THRESHOLD_MIN_LSB;
        } else if (threshold > PICKUP_THRESHOLD_MAX_LSB) {
            threshold = PICKUP_THRESHOLD_MAX_LSB;
        }
        bmi270_set_pickup_threshold((float)threshold);
        save_pickup_threshold_to_nvs(threshold);
```

- [ ] **Step 2: 启动时从 NVS 加载阈值**

在 `app_main()` 中，`voice_ble_init()` 调用块之后、`voice_net_init()` 之前插入：
```c
    // 从 NVS 恢复拿起灵敏度阈值（voice_ble_init 已初始化 nvs_flash）。
    load_pickup_threshold_from_nvs();
```

---

### Task 4: Windows AppConfig 灵敏度枚举与持久化

**Files:**
- Modify: `desktop/windows/src/app_config.h:18-21` 附近
- Modify: `desktop/windows/src/app_config.h:98-132` 附近
- Modify: `desktop/windows/src/app_config.cc:238-277`
- Modify: `desktop/windows/src/app_config.cc:318-384`
- Modify: `desktop/windows/src/app_config.cc:398-475`
- Modify: `desktop/windows/src/app_config.cc:556-787`（末尾追加辅助函数）

- [ ] **Step 1: 新增枚举与字段**

在 `app_config.h` 中，紧接 `enum class InteractionMode` 之后插入：
```cpp
enum class ImuWakeSensitivity {
    kLow,
    kMedium,
    kHigh,
};
```

在 `AppConfig` 结构体中，`bool show_imu_debug = false;` 之后插入：
```cpp
    ImuWakeSensitivity imu_wake_sensitivity = ImuWakeSensitivity::kLow;
```

在 `app_config.h` 末尾、`ParseHotwordList` 声明之前追加辅助函数声明：
```cpp
std::string ImuWakeSensitivityName(ImuWakeSensitivity sensitivity);
ImuWakeSensitivity ImuWakeSensitivityFromName(std::string_view name);
std::string ImuWakeSensitivityDisplayName(ImuWakeSensitivity sensitivity);
int ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity sensitivity);
```

- [ ] **Step 2: 解析 legacy key-value 配置**

在 `ApplyConfigValue()` 中，`show_imu_debug` 分支之后插入：
```cpp
    if (key == "imu_wake_sensitivity") config.imu_wake_sensitivity = ImuWakeSensitivityFromName(value);
```

- [ ] **Step 3: 解析 TOML 配置**

在 `AppConfig::Load()` 的 TOML 解析区域，`show_imu_debug` 分支之后插入：
```cpp
        if (auto value = TomlString(table, "imu_wake_sensitivity")) {
            config.imu_wake_sensitivity = ImuWakeSensitivityFromName(*value);
        }
```

- [ ] **Step 4: 序列化配置**

在 `AppConfig::Save()` 中，`show_imu_debug` 输出行之后插入：
```cpp
    output << "imu_wake_sensitivity = \"" << ImuWakeSensitivityName(imu_wake_sensitivity) << "\"\n";
```

- [ ] **Step 5: 实现辅助函数**

在 `app_config.cc` 末尾、`} // namespace voicestick` 之前追加：
```cpp
std::string ImuWakeSensitivityName(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return "medium";
    case ImuWakeSensitivity::kHigh: return "high";
    case ImuWakeSensitivity::kLow:
    default:
        return "low";
    }
}

ImuWakeSensitivity ImuWakeSensitivityFromName(std::string_view name) {
    if (name == "medium") return ImuWakeSensitivity::kMedium;
    if (name == "high") return ImuWakeSensitivity::kHigh;
    return ImuWakeSensitivity::kLow;
}

std::string ImuWakeSensitivityDisplayName(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return "Medium";
    case ImuWakeSensitivity::kHigh: return "High";
    case ImuWakeSensitivity::kLow:
    default:
        return "Low";
    }
}

int ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return 500;
    case ImuWakeSensitivity::kHigh: return 250;
    case ImuWakeSensitivity::kLow:
    default:
        return 800;
    }
}
```

---

### Task 5: Windows BLE 协议 payload 构造

**Files:**
- Modify: `desktop/windows/src/ble_protocol.h:80`
- Modify: `desktop/windows/src/ble_protocol.cc:268-272`

- [ ] **Step 1: 声明 payload 构造器**

在 `ble_protocol.h` 的 `ShowImuDebugPayload` 之后追加：
```cpp
    static ByteVector ImuWakeSensitivityPayload(int threshold_lsb);
```

- [ ] **Step 2: 实现 payload 构造器**

在 `ble_protocol.cc` 的 `ShowImuDebugPayload` 实现之后追加：
```cpp
ByteVector BleProtocol::ImuWakeSensitivityPayload(int threshold_lsb) {
    const auto json = std::string("{\"event\":\"imu_wake_sensitivity\",\"threshold\":") +
                      std::to_string(threshold_lsb) + "}";
    return ByteVector(json.begin(), json.end());
}
```

---

### Task 6: Windows BleCentral 接口与 FakeBleCentral

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.h:65-67`
- Modify: `desktop/windows/tests/core_tests.cc:54-133`

- [ ] **Step 1: 在纯虚接口中新增方法**

在 `voice_stick_coordinator.h` 中 `SendShowImuDebug` 之后追加：
```cpp
    virtual void SendImuWakeSensitivity(int threshold_lsb,
                                        const std::optional<std::string>& device_id) = 0;
```

- [ ] **Step 2: 更新 FakeBleCentral**

在 `core_tests.cc` 的 `FakeBleCentral` 中，`SendShowImuDebug` 实现之后追加：
```cpp
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override {
        sent_imu_wake_sensitivities.push_back(std::pair{threshold_lsb, device_id});
    }
```

并在 `public:` 成员变量区域追加：
```cpp
    std::vector<std::pair<int, std::optional<std::string>>> sent_imu_wake_sensitivities;
```

---

### Task 7: Windows BleCentralWin 实现

**Files:**
- Modify: `desktop/windows/src/ble_central_win.h:42-44`
- Modify: `desktop/windows/src/ble_central_win.cc:402-423`

- [ ] **Step 1: 声明实现方法**

在 `ble_central_win.h` 中 `SendShowImuDebug` 声明之后追加：
```cpp
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override;
```

- [ ] **Step 2: 实现方法**

在 `ble_central_win.cc` 的 `SendShowImuDebug` 实现之后追加：
```cpp
void BleCentralWin::SendImuWakeSensitivity(int threshold_lsb,
                                           const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::ImuWakeSensitivityPayload(threshold_lsb);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}
```

---

### Task 8: Windows VoiceStickCoordinator 转发阈值

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.cc:61-75`
- Modify: `desktop/windows/src/voice_stick_coordinator.cc:130-166`

- [ ] **Step 1: 在连接成功时下发阈值**

在 `VoiceStickCoordinator::Start()` 的 `ble_->on_connection_change` lambda 中，`SendShowImuDebug` 调用之后追加：
```cpp
        ble_->SendImuWakeSensitivity(ImuWakeSensitivityThresholdLsb(config_.imu_wake_sensitivity), std::nullopt);
```

- [ ] **Step 2: 在配置更新时下发阈值**

在 `VoiceStickCoordinator::UpdateConfig()` 中，`SendShowImuDebug` 调用之后追加：
```cpp
    ble_->SendImuWakeSensitivity(ImuWakeSensitivityThresholdLsb(config_.imu_wake_sensitivity), std::nullopt);
```

---

### Task 9: Windows SettingsDialog UI

**Files:**
- Modify: `desktop/windows/src/settings_dialog.h:55-64`
- Modify: `desktop/windows/src/settings_dialog.h:75-83`
- Modify: `desktop/windows/src/settings_dialog.cc:228-246`
- Modify: `desktop/windows/src/settings_dialog.cc:289-312`
- Modify: `desktop/windows/src/settings_dialog.cc:314-460`
- Modify: `desktop/windows/src/settings_dialog.cc:462-543`

- [ ] **Step 1: 新增控件成员和常量**

在 `settings_dialog.h` 中，`show_imu_debug_check_` 成员之后追加：
```cpp
    HWND imu_wake_sensitivity_combo_ = nullptr;
```

在 `static constexpr int kClientHeight = 690;` 改为：
```cpp
    static constexpr int kClientHeight = 720;
```

在控件 ID 区域，`kIdShowImuDebug = 2017;` 之后追加：
```cpp
    static constexpr UINT kIdImuWakeSensitivity = 2018;
```

- [ ] **Step 2: 在 WM_DESTROY 中清空控件句柄**

在 `HandleMessage()` 的 `case WM_DESTROY` 中，`show_imu_debug_check_ = nullptr;` 之后追加：
```cpp
        imu_wake_sensitivity_combo_ = nullptr;
```

在 `DestroyControls()` 中，`show_imu_debug_check_ = nullptr;` 之后追加：
```cpp
    imu_wake_sensitivity_combo_ = nullptr;
```

- [ ] **Step 3: 在 BuildControls() 中创建下拉框**

在 `show_imu_debug_check_` 创建代码块之后、`debug_dir_edit_` 创建代码块之前插入：
```cpp
    remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsImuWakeSensitivity).c_str(), Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    imu_wake_sensitivity_combo_ = remember(CreateCombo(hwnd_, ctrl_x, y, ctrl_w, Dp(120),
                                                       kIdImuWakeSensitivity, instance_));
    SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityLow, language).c_str()));
    SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityMedium, language).c_str()));
    SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityHigh, language).c_str()));
    y += row_h + Dp(10);
```

- [ ] **Step 4: 在 LoadConfigIntoControls() 中加载当前值**

在 `LoadConfigIntoControls()` 中，`show_imu_debug_check_` 的 `BM_SETCHECK` 调用之后、`SetWindowTextW(debug_dir_edit_, ...)` 之前插入：
```cpp
    int sensitivity_index = 0;
    if (config_.imu_wake_sensitivity == ImuWakeSensitivity::kMedium) sensitivity_index = 1;
    if (config_.imu_wake_sensitivity == ImuWakeSensitivity::kHigh) sensitivity_index = 2;
    SendMessageW(imu_wake_sensitivity_combo_, CB_SETCURSEL, sensitivity_index, 0);
```

- [ ] **Step 5: 在 SaveSettings() 中保存选择**

在 `SaveSettings()` 中，`config_.show_imu_debug = ...` 之后、`auto dir = ...` 之前插入：
```cpp
    int sensitivity_idx = static_cast<int>(SendMessageW(imu_wake_sensitivity_combo_, CB_GETCURSEL, 0, 0));
    if (sensitivity_idx == 1) {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    } else if (sensitivity_idx == 2) {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    } else {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    }
```

---

### Task 10: Windows 本地化文案

**Files:**
- Modify: `desktop/windows/src/localization.h:36-38`
- Modify: `desktop/windows/src/localization.cc:42-44`
- Modify: `desktop/windows/src/localization.cc:219-221`

- [ ] **Step 1: 新增 StringId**

在 `localization.h` 中，`kSettingsShowImuDebug` 之后追加：
```cpp
    kSettingsImuWakeSensitivity,
    kSettingsImuWakeSensitivityLow,
    kSettingsImuWakeSensitivityMedium,
    kSettingsImuWakeSensitivityHigh,
```

- [ ] **Step 2: 添加英文文案**

在 `localization.cc` 的 `EnglishStrings()` 中，`kSettingsShowImuDebug` 行之后追加：
```cpp
    table[Index(StringId::kSettingsImuWakeSensitivity)] = "Wake Sensitivity";
    table[Index(StringId::kSettingsImuWakeSensitivityLow)] = "Low";
    table[Index(StringId::kSettingsImuWakeSensitivityMedium)] = "Medium";
    table[Index(StringId::kSettingsImuWakeSensitivityHigh)] = "High";
```

- [ ] **Step 3: 添加中文文案**

在 `ChineseStrings()` 中，`kSettingsShowImuDebug` 行之后追加：
```cpp
    table[Index(StringId::kSettingsImuWakeSensitivity)] = "拿起灵敏度";
    table[Index(StringId::kSettingsImuWakeSensitivityLow)] = "低";
    table[Index(StringId::kSettingsImuWakeSensitivityMedium)] = "中";
    table[Index(StringId::kSettingsImuWakeSensitivityHigh)] = "高";
```

---

### Task 11: 协议文档更新

**Files:**
- Modify: `Doc/Ref/protocol.md:110-118`

- [ ] **Step 1: 在 Control Event 示例和表格中新增事件**

在示例 JSON 区域，`{"event":"show_imu_debug","enabled":true}` 之后插入：
```json
{"event":"imu_wake_sensitivity","threshold":500}
```

在 Control Event 表格中，`show_imu_debug` 行之后插入：
```markdown
| `imu_wake_sensitivity` | `threshold`: integer (LSB) | Windows -> StickS3 | 设置拿起/晃动亮屏灵敏度阈值。建议范围 50~2000 LSB；数值越小越灵敏。默认 800 LSB。 |
```

---

### Task 12: Windows 核心测试补充与构建验证

**Files:**
- Modify: `desktop/windows/tests/core_tests.cc:54-133`
- Modify: `desktop/windows/tests/core_tests.cc:826-928`

- [ ] **Step 1: 为 FakeBleCentral 添加记录字段和实现**

已在 Task 6 完成；确保 `sent_imu_wake_sensitivities` 字段存在。

- [ ] **Step 2: 在 TestAppConfig 中补充灵敏度测试**

在 `TestAppConfig()` 末尾、`assert(LocalizationTablesAreComplete());` 之前插入：
```cpp
    assert(ImuWakeSensitivityFromName("low") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityFromName("medium") == ImuWakeSensitivity::kMedium);
    assert(ImuWakeSensitivityFromName("high") == ImuWakeSensitivity::kHigh);
    assert(ImuWakeSensitivityFromName("invalid") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kLow) == "low");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kMedium) == "medium");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kHigh) == "high");
    assert(ImuWakeSensitivityDisplayName(ImuWakeSensitivity::kLow) == "Low");
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kLow) == 800);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kMedium) == 500);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kHigh) == 250);
```

- [ ] **Step 3: 新增协调器转发测试**

在 `TestCoordinatorSyncsPromptToneOnConnectionAndConfigUpdate` 测试附近，或文件末尾新增：
```cpp
void TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    assert(ble_ptr->sent_imu_wake_sensitivities.size() == 1);
    assert(ble_ptr->sent_imu_wake_sensitivities.front().first == 250);
    assert(!ble_ptr->sent_imu_wake_sensitivities.front().second.has_value());

    AppConfig updated = config;
    updated.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    coordinator.UpdateConfig(updated);
    assert(ble_ptr->sent_imu_wake_sensitivities.size() == 2);
    assert(ble_ptr->sent_imu_wake_sensitivities.back().first == 500);
}
```

并在 `main()` 或测试入口中调用该函数（与 `TestCoordinatorSyncsPromptToneOnConnectionAndConfigUpdate` 同风格）。

- [ ] **Step 4: 运行 Windows 构建和测试**

```powershell
cd C:/Dev/FFE/George/voicestick
build_win.bat
ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: 构建成功，测试通过。

---

### Task 13: 固件构建验证

**Files:**
- 无文件修改；仅运行命令。

- [ ] **Step 1: 编译固件**

```powershell
cd C:/Dev/FFE/George/voicestick/firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

Expected: 编译成功，生成 `build/voice_stick.bin`。

---

### Task 14: OTA 刷机与设备实测

**Files:**
- 无文件修改；使用已有构建产物和 `VoiceStickCtl` / `scripts/idf_cli.py`。

- [ ] **Step 1: 启动本地 HTTP 服务器提供固件**

在 PowerShell 中：
```powershell
cd C:/Dev/FFE/George/voicestick/firmware/build
python -m http.server 8000
```

- [ ] **Step 2: 使用 VoiceStickCtl 触发 BLE OTA pull**

在另一个 PowerShell 中：
```powershell
cd C:/Dev/FFE/George/voicestick/desktop/windows/build-x64
./VoiceStickCtl.exe ota_pull VS-5D74 http://192.168.3.96:8000/voice_stick.bin <sha256_hex>
```

> 如果 VoiceStickCtl 不接受域名，用本机局域网 IP。`sha256_hex` 通过 `certutil -hashfile firmware/build/voice_stick.bin SHA256` 计算。

Expected: 设备下载固件、重启、进入新固件。

- [ ] **Step 3: 实测灵敏度**

1. 启动 Windows 桌面端，等待 `VS-5D74` 连接。
2. 把设备静置到屏幕变暗（约 30 秒）。
3. 打开 Settings，选择 **Medium**，保存；轻轻晃动设备，观察是否比 Low 更容易亮屏。
4. 选择 **High**，保存；更轻微晃动应能亮屏。
5. 选择 **Low**，保存；恢复需要较明显晃动。
6. 关闭 Windows 端或断开蓝牙，重启设备，确认重启后仍为 **High**（或最后设置的档位），验证 NVS 持久化生效。

- [ ] **Step 4: 根据实测结果微调阈值**

如果 Medium/High 手感不合适，修改 `app_config.cc` 中 `ImuWakeSensitivityThresholdLsb()` 的返回值，重新编译 Windows 端（固件端阈值范围 50~2000 已足够）。无需改固件即可测试不同阈值。

---

## 计划自查

**Spec coverage:**
- 三档 Low/Medium/High → Task 4、Task 9。
- Windows 下发具体阈值 → Task 5、Task 7、Task 8。
- 固件运行时调整阈值 → Task 1、Task 3。
- 固件 NVS 持久化 → Task 2、Task 3。
- Windows config.toml 持久化 → Task 4。
- Settings 窗口可见开关 → Task 9。
- 协议文档更新 → Task 11。
- 构建与 OTA 测试 → Task 12、13、14。

**Placeholder scan:** 无 TBD/TODO/"implement later"。

**Type consistency:**
- `BleProtocol::ImuWakeSensitivityPayload(int)` 与 `BleCentral::SendImuWakeSensitivity(int, ...)`、`FakeBleCentral`、`BleCentralWin` 一致。
- `ImuWakeSensitivityThresholdLsb()` 返回 `int`，直接传入 payload。

**已知风险：**
- `High` 档 250 LSB 可能过于灵敏，需实测；调整阈值只需改 Windows 端。
- NVS 读取在 `voice_ble_init()` 之后，此前短暂使用默认值 800 LSB，可接受。
