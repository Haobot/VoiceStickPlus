# 第三方输入法重命名 + Typeless 点按式兼容

## 背景与现状（已核实证据）

`wechat_input_method` 输出模式：把 Opus 解码为 PCM 渲染到系统虚拟麦克风（如 VB-CABLE），并模拟 `hotkey` 触发输入法语音。Windows 端独有，macOS 的 `OutputTarget` 仅有 `focusedApp`/`subtitle`。

关键现状：

- 配置块 `[wechat_input_method]`：`hotkey`（默认 `ctrl+win`）、`virtual_mic_playback_name`、`virtual_mic_capture_name`、`auto_switch_default_recording_device`。键名/字符串值 `"wechat_input_method"` 出现在用户 `config.toml` 中。
- `InteractionMode`：`kHoldToTalk`（默认）/ `kHoldToTalkInstant`（wechat+hold 派生，跳 300ms 阈值，不暴露 UI）/ `kClickToTalk`。**`interaction_mode` 当前无 UI 控件，只能改 config.toml。**
- **toggle 逻辑已实现**：`InteractionModeToSend()`（`voice_stick_coordinator.cc:2214`）在 wechat+click_to_talk 时下发 `kClickToTalk`；`HandleButtonClick` 的 wechat 分支（`:781-792`）已 toggle--第一次 click 调 `HandleWechatInputMethodPrimaryButtonDown`（SendDown+渲染），第二次 click 调 `HandleWechatInputMethodPrimaryButtonUp`（SendUp+停渲染）。
- `hotkey` 解析（`wechat_input_method_hotkey.cc` `VkCodeFromName`）：支持 `ctrl/alt/shift/win` + 字母/数字/功能键。`"alt"`->`VK_MENU`。**不支持 `"ralt"`（右ALT）**--`VkCodeFromName("ralt")` 返回 0，单键右ALT 无法配置。
- 显示名 `OutputTargetDisplayName`（`app_config.cc:974`）：`kWechatInputMethod` -> `"WeChat Input Method"`。zh（`localization.cc:256`）：`"微信输入法"`、`"微信语音热键"`。

## 需求

1. 输出目标"微信输入法"->"第三方输入法"（通用化，因该模式也服务 Typeless 等任意第三方输入法）。**只改 UI 显示文案与注释，保留配置键名 `wechat_input_method` 和字符串值**（向后兼容旧 `config.toml`，用户不可见键名）。
2. 兼容 Typeless 工作方式：按一下右ALT进入录音，再按一下停止。在第三方输入法设置区新增"触发方式"选项（长按式 / 点按式），**点按式联动全局 `interaction_mode = click_to_talk`**（复用已实现的 toggle）；`hotkey` 支持 `ralt`。

## 改动点

### 需求1：命名通用化（保留配置键名，向后兼容）

仅改用户可见文案与注释，不动配置键名/字符串值/枚举名：

| 文件 | 位置 | 改动 |
|---|---|---|
| `app_config.cc` | `:974-980` `OutputTargetDisplayName` | `"WeChat Input Method"` -> `"Third-party Input Method"` |
| `localization.cc` | `:55` en | `"WeChat Input Method"` -> `"Third-party Input Method"` |
| `localization.cc` | `:56` en | `"WeChat Voice Hotkey"` -> `"Voice Hotkey"` |
| `localization.cc` | `:256` zh | `"微信输入法"` -> `"第三方输入法"` |
| `localization.cc` | `:257` zh | `"微信语音热键"` -> `"语音热键"` |
| `settings_dialog.cc` | `:469,650,667-668,679` 注释 | "微信输入法" -> "第三方输入法" |
| `config.template.toml` | `:84,86` 注释 | "微信输入法模式"/"触发微信输入法语音" -> "第三方输入法模式"/"触发第三方输入法语音" |
| `app_config.h` | `:103` `WechatInputMethodConfig` 注释 | "触发微信输入法语音输入" -> "触发第三方输入法语音输入" |
| `wechat_input_method_hotkey.h` | `:4` 注释 | "触发微信输入法语音输入快捷键" -> "触发第三方输入法语音输入快捷键" |
| `Doc/Ref/protocol.md` | `:184` | `wechat_input_method mode` 措辞通用化 |

**保留不动**：配置键 `[wechat_input_method]`、字符串值 `"wechat_input_method"`、枚举 `kWechatInputMethod`、结构 `WechatInputMethodConfig`、类名 `WechatInputMethodHotkey`（内部标识符，用户不可见，避免无谓的全局重命名与兼容代码）。

### 需求2a：`hotkey` 支持右ALT（TDD 核心）

`wechat_input_method_hotkey.cc`：

- `VkCodeFromName` 新增：`"ralt"` -> `VK_RMENU`（`0xA5`）、`"lalt"` -> `VK_LMENU`（`0xA4`，对称补全）。保留 `"alt"` -> `VK_MENU`（兼容旧配置）。
- `SendInputForKeys`：对 `VK_RMENU`、`VK_RCONTROL` 加 `KEYEVENTF_EXTENDEDKEY` 标志（右ALT/右Ctrl 是扩展键，不加此标志会被系统识别为左ALT，Typeless 监听右ALT将不触发）。

### 需求2b：UI"触发方式"选项（联动全局 interaction_mode）

`settings_dialog.cc` 第三方输入法区（`wechat_hotkey` 行附近）新增一行，沿用现有声明式布局范式（`add(row, {controls}, defer_visibility)`，参考 `windows-dialog-dynamic-layout-pattern` memory）：

- 控件：两个 `BS_AUTORADIOBUTTON`--"长按式（微信输入法）"/"点按式（Typeless 等）"。
- 显隐条件：`output_target_combo_` idx == 2（第三方输入法）时显示，与 `wechat_hotkey` 行一致。
- 加载：根据 `config.interaction_mode` 选中--`kClickToTalk` 选"点按式"，否则选"长按式"。
- 保存：选"点按式"->`config.interaction_mode = kClickToTalk`；选"长按式"->`kHoldToTalk`。
- 新增 `localization` StringId：`kSettingsTriggerMode`、`kSettingsTriggerModeHold`、`kSettingsTriggerModeClick`（中英文）。
- 提示文案：点按式旁可注"联动全局交互模式为 click_to_talk"。

> 语义说明：`interaction_mode` 是全局的，"触发方式"是它在第三方输入法模式下的镜像 UI。用户在此选点按式即把全局主键行为改为 click toggle；切到其他输出目标时该设置继续生效（与"联动全局"方案一致，已与用户确认）。

## TDD 测试清单（红灯先行）

`desktop/windows/tests/core_tests.cc`：

1. **🔴 `TestWechatInputMethodHotkeyParsing` 扩展**（红灯）：
   - `assert(WechatInputMethodHotkey("ralt").KeyCount() == 1)` -- 当前返回 0，红灯。
   - `assert(WechatInputMethodHotkey("lalt").KeyCount() == 1)`。
2. **🟢 实现** `ralt`/`lalt` 解析 + `KEYEVENTF_EXTENDEDKEY`，测试转绿。
3. 现有 `TestCoordinatorWechatInputMethodButtonDownSendsHotkey`、`TestWechatInputMethodConfigRoundTrip` 保持绿（回归）。
4. 触发方式联动 `interaction_mode` 的保存/加载往返测试（`TestWechatInputMethodConfigRoundTrip` 扩展或新增）。

> `ralt` 是否真正发送右ALT（而非左ALT）由 `KEYEVENTF_EXTENDEDKEY` 标志保证，单测只能验证解析成功（`KeyCount`），具体 vk 值与标志位靠代码审查 + **真机验证**（Typeless 实际触发）。

## 实施顺序

1. 🔴 扩展 `TestWechatInputMethodHotkeyParsing` 加 `ralt`/`lalt` 断言，`ctest -R voicestick_windows_tests` 确认红灯。
2. 🟢 `wechat_input_method_hotkey.cc` 实现 `ralt`/`lalt` + `KEYEVENTF_EXTENDEDKEY`，转绿。
3. 需求1 命名通用化：`localization.cc`/`app_config.cc`/`settings_dialog.cc` 注释/`config.template.toml`/`app_config.h`/`wechat_input_method_hotkey.h`/`protocol.md`。
4. 需求2b UI：`localization.h`/`localization.cc` 新增 `kSettingsTriggerMode*`；`settings_dialog.h`/`.cc` 加触发方式单选行 + 加载/保存联动。
5. `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿。
6. `build_win.bat` 构建核对 exe 时间戳/体积。
7. 真机验证：配 `output_target=wechat_input_method` + 触发方式=点按式 + `hotkey=ralt`，验证 Typeless 按一下开始、再按一下停止。

## 不改动范围

- 配置键名 `[wechat_input_method]`、字符串值 `"wechat_input_method"`、枚举 `kWechatInputMethod`（向后兼容）。
- 固件（toggle 逻辑在桌面端，`click_to_talk` 固件已支持，无需新协议字段）。
- macOS（无 wechat 模式）。
- `WechatInputMethodConfig`/`WechatInputMethodHotkey` 等内部标识符类名（避免无谓全局重命名）。
