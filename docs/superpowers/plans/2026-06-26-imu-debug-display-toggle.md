# IMU Debug Display Toggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `show_imu_debug` configuration option that hides the on-screen accelerometer values by default and lets Windows users enable them from the Settings dialog.

**Architecture:** Reuse the existing `prompt_tone_enabled` pattern: persist the boolean in `AppConfig`/`config.toml`, expose a checkbox in `SettingsDialog`, forward the value through `VoiceStickCoordinator` over BLE, and have the firmware interpret the new `show_imu_debug` control event to decide whether to render XYZ values.

**Tech Stack:** ESP-IDF C (firmware), C++20 / Win32 / C++/WinRT (Windows desktop).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `firmware/main/main.c` | Parse new BLE control event; maintain `s_show_imu_debug` flag; gate IMU label updates in timer callback. |
| `desktop/windows/src/app_config.h` | Declare `show_imu_debug` field. |
| `desktop/windows/src/app_config.cc` | Parse/serialize `show_imu_debug` from/to TOML. |
| `desktop/windows/src/localization.h` / `.cc` | Add `kSettingsShowImuDebug` string ID and translations. |
| `desktop/windows/src/ble_protocol.h` / `.cc` | Add `ShowImuDebugPayload(bool)` builder. |
| `desktop/windows/src/ble_central_win.h` / `.cc` | Add `SendShowImuDebug(...)` dispatcher. |
| `desktop/windows/src/voice_stick_coordinator.cc` | Send the value on connect and on config update. |
| `desktop/windows/src/settings_dialog.h` / `.cc` | Add checkbox, load/save its state, adjust dialog height. |
| `Doc/Ref/protocol.md` | Document the new `show_imu_debug` control event. |
| `AGENTS.md` | Update the sample config-item list. |

---

### Task 1: Firmware — Parse `show_imu_debug` control event

**Files:**
- Modify: `firmware/main/main.c:56-60` (add static flag near other statics)
- Modify: `firmware/main/main.c:675-680` (add branch in `ble_control_cb`)

- [ ] **Step 1: Add the static flag**

```c
static bool s_show_imu_debug = false;
```

Place it near `s_prompt_tone_enabled` (around line 59).

- [ ] **Step 2: Add the event handler**

Find the `prompt_tone` branch in `ble_control_cb`. After it, add:

```c
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "show_imu_debug") == 0 &&
               cJSON_IsBool(enabled)) {
        s_show_imu_debug = cJSON_IsTrue(enabled);
        ESP_LOGI(TAG, "show_imu_debug %s", s_show_imu_debug ? "enabled" : "disabled");
```

- [ ] **Step 3: Build the firmware**

Run:

```bash
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py build
```

Expected: build succeeds with no new warnings.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): parse show_imu_debug control event"
```

---

### Task 2: Firmware — Gate IMU label updates on the flag

**Files:**
- Modify: `firmware/main/main.c:1292-1315` (`imu_poll_timer_cb`)

- [ ] **Step 1: Update the timer callback**

Replace the body that formats and displays XYZ with:

```c
static void imu_poll_timer_cb(void *arg)
{
    (void)arg;

    if (!bmi270_present()) {
        ui_status_set_imu_text("IMU: n/a");
        (void)esp_timer_stop(s_imu_poll_timer);
        return;
    }

    float x_g = 0.0f;
    float y_g = 0.0f;
    float z_g = 0.0f;
    if (bmi270_read_acc_g(&x_g, &y_g, &z_g) != ESP_OK) {
        return;
    }

    update_display_orientation(x_g);

    ESP_LOGI(TAG, "IMU acc X=%+.2f Y=%+.2f Z=%+.2f g", x_g, y_g, z_g);

    if (!s_show_imu_debug) {
        ui_status_set_imu_text("");
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "X:%+.2f g\nY:%+.2f g\nZ:%+.2f g", x_g, y_g, z_g);
    ui_status_set_imu_text(buf);
}
```

- [ ] **Step 2: Build the firmware**

Run:

```bash
cd firmware
idf.py build
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): hide IMU values unless show_imu_debug is enabled"
```

---

### Task 3: Windows — Add `show_imu_debug` to `AppConfig`

**Files:**
- Modify: `desktop/windows/src/app_config.h:110-130`
- Modify: `desktop/windows/src/app_config.cc:240-280`
- Modify: `desktop/windows/src/app_config.cc:320-390`
- Modify: `desktop/windows/src/app_config.cc:410-440`

- [ ] **Step 1: Declare the field**

In `app_config.h`, add inside `struct AppConfig`:

```cpp
    bool show_imu_debug = false;
```

Place it near `prompt_tone_enabled`.

- [ ] **Step 2: Parse from legacy key-value pairs**

In `app_config.cc` inside the legacy key-value loop (around line 268), add:

```cpp
    if (key == "show_imu_debug") config.show_imu_debug = BoolValue(value, config.show_imu_debug);
```

- [ ] **Step 3: Parse from TOML table**

In the TOML `[output]` parsing block (around line 377), add:

```cpp
        if (auto value = TomlBool(table, "show_imu_debug")) config.show_imu_debug = *value;
```

- [ ] **Step 4: Serialize to TOML**

In `SerializeToString()` (around line 435), add after `prompt_tone_enabled`:

```cpp
    output << "show_imu_debug = " << (show_imu_debug ? "true" : "false") << "\n";
```

- [ ] **Step 5: Build and run Windows core tests**

Run:

```powershell
build_win.bat
ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: tests pass.

- [ ] **Step 6: Commit**

```bash
git add desktop/windows/src/app_config.h desktop/windows/src/app_config.cc
git commit -m "feat(windows): add show_imu_debug AppConfig field"
```

---

### Task 4: Windows — Add BLE protocol payload

**Files:**
- Modify: `desktop/windows/src/ble_protocol.h:78-82`
- Modify: `desktop/windows/src/ble_protocol.cc:262-270`

- [ ] **Step 1: Declare payload builder**

In `ble_protocol.h`, add after `PromptTonePayload`:

```cpp
    static ByteVector ShowImuDebugPayload(bool enabled);
```

- [ ] **Step 2: Implement payload builder**

In `ble_protocol.cc`, add after `PromptTonePayload`:

```cpp
ByteVector BleProtocol::ShowImuDebugPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"show_imu_debug\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}
```

- [ ] **Step 3: Build and run core tests**

Run:

```powershell
ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: tests pass.

- [ ] **Step 4: Commit**

```bash
git add desktop/windows/src/ble_protocol.h desktop/windows/src/ble_protocol.cc
git commit -m "feat(windows): add ShowImuDebugPayload BLE builder"
```

---

### Task 5: Windows — Add `BleCentralWin::SendShowImuDebug`

**Files:**
- Modify: `desktop/windows/src/ble_central_win.h:55-65`
- Modify: `desktop/windows/src/ble_central_win.cc:379-400`

- [ ] **Step 1: Declare the dispatcher**

In `ble_central_win.h`, add after `SendPromptToneEnabled`:

```cpp
    void SendShowImuDebug(bool enabled,
                          const std::optional<std::string>& device_id);
```

- [ ] **Step 2: Implement the dispatcher**

In `ble_central_win.cc`, add after `SendPromptToneEnabled`:

```cpp
void BleCentralWin::SendShowImuDebug(bool enabled,
                                     const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::ShowImuDebugPayload(enabled);
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

- [ ] **Step 3: Build and run core tests**

Run:

```powershell
ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: tests pass.

- [ ] **Step 4: Commit**

```bash
git add desktop/windows/src/ble_central_win.h desktop/windows/src/ble_central_win.cc
git commit -m "feat(windows): add SendShowImuDebug dispatcher"
```

---

### Task 6: Windows — Forward value from coordinator

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.cc:72-74`
- Modify: `desktop/windows/src/voice_stick_coordinator.cc:151-153`

- [ ] **Step 1: Send on BLE connect**

In `Start()`, after:

```cpp
        ble_->SendPromptToneEnabled(config_.prompt_tone_enabled, std::nullopt);
```

add:

```cpp
        ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt);
```

- [ ] **Step 2: Send on config update**

In `UpdateConfig()`, after:

```cpp
    ble_->SendPromptToneEnabled(config_.prompt_tone_enabled, std::nullopt);
```

add:

```cpp
    ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt);
```

- [ ] **Step 3: Build and run core tests**

Run:

```powershell
ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests
```

Expected: tests pass.

- [ ] **Step 4: Commit**

```bash
git add desktop/windows/src/voice_stick_coordinator.cc
git commit -m "feat(windows): forward show_imu_debug on connect and config update"
```

---

### Task 7: Windows — Add settings UI string

**Files:**
- Modify: `desktop/windows/src/localization.h:33-38`
- Modify: `desktop/windows/src/localization.cc:33-45`

- [ ] **Step 1: Add string ID**

In `localization.h`, add inside the `StringId` enum after `kSettingsDebugAudio`:

```cpp
    kSettingsShowImuDebug,
```

- [ ] **Step 2: Add English translation**

In `localization.cc`, add to the English table:

```cpp
    table[Index(StringId::kSettingsShowImuDebug)] = "Show accelerometer debug values";
```

- [ ] **Step 3: Add Simplified Chinese translation**

In the same file, add to the Simplified Chinese table:

```cpp
    table[Index(StringId::kSettingsShowImuDebug)] = "显示加速度调试数值";
```

- [ ] **Step 4: Build the app target**

Run:

```powershell
cmake --build desktop/windows/build-x64 --target VoiceStickApp
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add desktop/windows/src/localization.h desktop/windows/src/localization.cc
git commit -m "feat(windows): add show_imu_debug localization strings"
```

---

### Task 8: Windows — Add checkbox to SettingsDialog

**Files:**
- Modify: `desktop/windows/src/settings_dialog.h:54-56`
- Modify: `desktop/windows/src/settings_dialog.h:63`
- Modify: `desktop/windows/src/settings_dialog.h:75-80`
- Modify: `desktop/windows/src/settings_dialog.cc:426-432`
- Modify: `desktop/windows/src/settings_dialog.cc:481`
- Modify: `desktop/windows/src/settings_dialog.cc:524`
- Modify: `desktop/windows/src/settings_dialog.cc:238-241`
- Modify: `desktop/windows/src/settings_dialog.cc:302-304`

- [ ] **Step 1: Add member and ID**

In `settings_dialog.h`:

```cpp
    HWND show_imu_debug_check_ = nullptr;
```

Add ID:

```cpp
    static constexpr UINT kIdShowImuDebug = 2017;
```

Adjust dialog height:

```cpp
    static constexpr int kClientHeight = 690;  // was 660
```

- [ ] **Step 2: Create the checkbox**

In `settings_dialog.cc` `BuildControls()`, after `debug_audio_check_` block (around line 431):

```cpp
    remember_label(CreateLabel(hwnd_, L"", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    show_imu_debug_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsShowImuDebug, language).c_str(), ctrl_x, y,
                                                  ctrl_w, Dp(22), kIdShowImuDebug, instance_,
                                                  BS_AUTOCHECKBOX));
    y += row_h + Dp(10);
```

- [ ] **Step 3: Reset pointer on destroy**

In `HandleMessage` `WM_DESTROY` (around line 240), add:

```cpp
        show_imu_debug_check_ = nullptr;
```

In `DestroyControls()` (around line 303), add:

```cpp
    show_imu_debug_check_ = nullptr;
```

- [ ] **Step 4: Load config into control**

In `LoadConfigIntoControls()` (around line 481), add after `debug_audio_check_`:

```cpp
    SendMessageW(show_imu_debug_check_, BM_SETCHECK, config_.show_imu_debug ? BST_CHECKED : BST_UNCHECKED, 0);
```

- [ ] **Step 5: Save control to config**

In `SaveSettings()` (around line 524), add after `debug_audio_cache`:

```cpp
    config_.show_imu_debug = SendMessageW(show_imu_debug_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
```

- [ ] **Step 6: Build the app target**

Run:

```powershell
cmake --build desktop/windows/build-x64 --target VoiceStickApp
```

Expected: build succeeds.

- [ ] **Step 7: Commit**

```bash
git add desktop/windows/src/settings_dialog.h desktop/windows/src/settings_dialog.cc
git commit -m "feat(windows): add show_imu_debug checkbox in settings"
```

---

### Task 9: Update protocol documentation

**Files:**
- Modify: `Doc/Ref/protocol.md`

- [ ] **Step 1: Find the `control_rx` events table**

Locate the table that lists events like `ui_state`, `interaction_mode`, `prompt_tone`.

- [ ] **Step 2: Add the new row**

Add a row for `show_imu_debug`:

```markdown
| `show_imu_debug` | `enabled`: bool | 主机 → 设备 | 是否显示屏幕顶部 IMU 加速度调试数值。默认 false。 |
```

Match the exact column style of the existing table.

- [ ] **Step 3: Commit**

```bash
git add Doc/Ref/protocol.md
git commit -m "docs: document show_imu_debug control event"
```

---

### Task 10: Update agent instructions

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: Update sample config list**

Find the config-item example list that contains `prompt_tone_enabled` and add:

```markdown
- `show_imu_debug`：是否在设备屏幕上显示 IMU 加速度调试数值，默认 `false`。
```

- [ ] **Step 2: Commit**

```bash
git add AGENTS.md
git commit -m "docs(agents): document show_imu_debug config option"
```

---

### Task 11: Final verification

**Files:**
- All of the above.

- [ ] **Step 1: Full firmware build**

```bash
cd firmware
idf.py build
```

Expected: build succeeds with no errors.

- [ ] **Step 2: Full Windows build and tests**

```powershell
build_win.bat
ctest --test-dir desktop/windows/build-x64 --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Review git log**

Run:

```bash
git log --oneline -11
```

Expected: 11 commits from this plan, all clean and focused.

- [ ] **Step 4: No-op final commit or tag**

If all verification passes, no further commit is required.

---

## Self-Review Checklist

- **Spec coverage:**
  - BLE event `show_imu_debug` → Task 1, 4.
  - Firmware default hidden, flag-gated render → Task 2.
  - Windows `AppConfig` persistence → Task 3.
  - Settings dialog checkbox → Task 7, 8.
  - Coordinator forward on connect/config update → Task 6.
  - Protocol docs and agent docs → Task 9, 10.
- **Placeholder scan:** No TBD/TODO/fill-in details.
- **Type consistency:** Field name `show_imu_debug` used consistently across C++, C, TOML, BLE JSON, and docs.
