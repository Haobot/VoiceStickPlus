# 第三方输入法按触发模式分别记忆热键

## 背景与现状（已核实证据）

第三方输入法模式（`wechat_input_method`）支持两种触发方式：长按式（`hold_to_talk`，如微信输入法用 `ctrl+win`）和点按式（`click_to_talk`，如 Typeless 用 `ralt`）。两者针对不同应用、热键不同。设置页"触发方式"单选（`settings_dialog.cc:696-703`）当前只联动全局 `interaction_mode`，**切换时热键编辑框内容不变**。

根因：配置只有一个热键字段，未按模式分别存储。

- `app_config.h:104` - `WechatInputMethodConfig` 仅 `std::string hotkey = "ctrl+win";`
- `app_config.cc:510-511` / `:660-661` - TOML 读/写都只处理单个 `hotkey`
- `settings_dialog.cc:1031` - 加载把 `hotkey` 填进 `wechat_hotkey_edit_`；`:1127` - 保存写回同一字段
- `settings_dialog.cc:1032-1037` / `:1128-1132` - 触发方式单选只联动 `interaction_mode`
- `settings_dialog.cc:202-239` - `WM_COMMAND` 未处理 `kIdTriggerModeHold/kIdTriggerModeClick`
- `voice_stick_coordinator.cc:606` - 用 `config_.wechat_input_method.hotkey` 构造热键发送器

## 需求

切换触发方式时，自动切换到该模式之前保存的热键；两套热键各自记忆（长按式一套、点按式一套）。

## 设计

### 配置模型（`app_config.h`）

`WechatInputMethodConfig` 增加 `hotkey_hold`、`hotkey_click`，保留 `hotkey` 作 legacy 回退：

```cpp
struct WechatInputMethodConfig {
    std::string hotkey = "ctrl+win";                 // legacy，仅向后兼容加载
    std::string hotkey_hold = "ctrl+win";            // 长按式（hold_to_talk）
    std::string hotkey_click = "ralt";               // 点按式（click_to_talk）
    std::string virtual_mic_playback_name = "CABLE Input";
    std::string virtual_mic_capture_name = "CABLE Output";
    bool auto_switch_default_recording_device = false;

    // 按当前交互模式返回应使用的热键。kHoldToTalkInstant 归入长按式。
    std::string ActiveHotkey(InteractionMode mode) const {
        return mode == InteractionMode::kClickToTalk ? hotkey_click : hotkey_hold;
    }
    bool operator==(const WechatInputMethodConfig& other) const = default;
};
```

### 加载（`app_config.cc:509-522`）

读 `hotkey`（legacy）、`hotkey_hold`、`hotkey_click`。`hotkey_hold`/`hotkey_click` 未配置时回退到 legacy `hotkey`（保留旧用户自定义，不丢配置）；三者都缺则用 struct 默认值（`ctrl+win`/`ralt`）。

### 保存（`app_config.cc:660-665`）

写 `hotkey_hold` + `hotkey_click`。不再写 `hotkey`（废弃；加载时作 legacy 回退）。

### UI 联动（`settings_dialog.h` / `settings_dialog.cc`）

- 新增成员 `InteractionMode loaded_hotkey_mode_;` -- 编辑框当前显示的热键所属模式。
- `LoadSettings`（`:1031`）：`loaded_hotkey_mode_ = config_.interaction_mode;` 编辑框填 `ActiveHotkey(loaded_hotkey_mode_)`。
- `SaveSettings`（`:1127`）：编辑框值存到 `loaded_hotkey_mode_` 对应字段（`kClickToTalk`->`hotkey_click`，否则 `hotkey_hold`）。`interaction_mode` 仍按 radio 读。
- `WM_COMMAND` 新增 `case kIdTriggerModeHold` / `case kIdTriggerModeClick`（`BN_CLICKED`）：调 `OnTriggerModeChanged()`。
- `OnTriggerModeChanged()`：读 radio 当前选中得到新模式；若 `!= loaded_hotkey_mode_`，把编辑框当前值存回 `loaded_hotkey_mode_`（旧模式）字段，`loaded_hotkey_mode_ = 新模式`，编辑框填 `ActiveHotkey(新模式)`。

### 协调器（`voice_stick_coordinator.cc:606`）

`wechat_hotkey_factory_` / `make_unique<WechatInputMethodHotkey>` 的入参由 `config_.wechat_input_method.hotkey` 改为 `config_.wechat_input_method.ActiveHotkey(config_.interaction_mode)`。

### `config.template.toml`

`[wechat_input_method]` 段加 `hotkey_hold` / `hotkey_click`（默认值），`hotkey` 标注废弃或移除（加载已兼容其缺失）。

### localization

`wechat_hotkey_label_` 文案不变（"语音热键"）。切换时由 radio 选中态指示当前编辑的是哪套热键。MVP 不新增提示文案。

## TDD 测试清单（`desktop/windows/tests/core_tests.cc`）

红灯先行，`ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests`：

1. 🔴 `TestWechatInputMethodPerModeHotkeyRoundTrip`：设 `hotkey_hold="ctrl+win"`、`hotkey_click="ralt"`，序列化再反序列化，断言两字段独立保留。
2. 🔴 `TestWechatInputMethodLegacyHotkeyFallback`：TOML 仅 `hotkey="ctrl+win"`，加载后 `hotkey_hold=="ctrl+win"` 且 `hotkey_click=="ctrl+win"`（legacy 回退）。
3. 🔴 `TestWechatInputMethodActiveHotkeyByMode`：`ActiveHotkey(kHoldToTalk)==hotkey_hold`、`ActiveHotkey(kClickToTalk)==hotkey_click`。
4. 回归：现有 `TestWechatInputMethodConfigRoundTrip` 保持绿（按新字段调整断言）。

## 实施顺序

1. 本方案文档。
2. 🔴 `core_tests.cc` 加 3 个测试，ctest 确认红。
3. 🟢 `app_config.h/cc` 实现 `hotkey_hold`/`hotkey_click` + `ActiveHotkey` + 加载/保存，转绿。
4. `voice_stick_coordinator.cc:606` 改用 `ActiveHotkey`。
5. `settings_dialog.h/cc` 加 `loaded_hotkey_mode_` + `OnTriggerModeChanged` + `Load/Save` 改造 + `WM_COMMAND` case。
6. `config.template.toml` 更新。
7. `ctest` 全绿。
8. `build_win.bat` 构建，核对 `desktop/windows/build-x64/VoiceStick.exe` 时间戳与体积。
9. `git add -f` 提交（`desktop/windows/` 被 `.gitignore` 忽略）。

## 不改动范围

- 固件、macOS、协议、BLE。
- `hold_to_talk` / `click_to_talk` 运行时 toggle 逻辑（仅热键来源改为按模式选取）。
- `SendDown`/`SendUp`/`SendClick` 机制。
- 微信输入法模式的其他配置项（虚拟麦克风、auto_switch）。
