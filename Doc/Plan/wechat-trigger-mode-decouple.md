# 第三方输入法触发模式与全局 interaction_mode 解耦

## 背景

用户反馈：输出目标从"第三方输入法"切到"当前应用"后，若第三方输入法的触发方式是"点按式"，切到当前应用后**无法长按录音**；若是"长按式"则正常。

## 根因

`settings_dialog.cc` 的"触发方式"单选（仅第三方输入法模式显示）直接写入**全局** `config_.interaction_mode`（注释明说"点按式联动全局 interaction_mode=click_to_talk"）：

```cpp
config_.interaction_mode =
    click_trigger ? InteractionMode::kClickToTalk : InteractionMode::kHoldToTalk;
```

切换输出目标走 `UpdateConfig`，只重发 `InteractionModeToSend()`，**不改 `interaction_mode`**。于是：

1. wechat 选"点按式" -> `interaction_mode=kClickToTalk`
2. 切到 focused_app -> `interaction_mode` 仍 `kClickToTalk`
3. `InteractionModeToSend()` 下发 `kClickToTalk` 给固件
4. 固件 click_to_talk 下主键按下发 `button_click`（toggle），不发 `button_down`（`firmware/main/main.c:1023-1025`）
5. 桌面端 `HandlePrimaryButtonDown`（focused_app 长按录音入口）不被调用 -> 长按无法录音

wechat 选"长按式"时 `interaction_mode=kHoldToTalk`，切到 focused_app 仍 hold，固件发 `button_down`，长按正常。

本质：wechat 的"触发方式"复用了全局 `interaction_mode`。commit `d9e523b` 已把 wechat **热键**分别记忆（`hotkey_hold`/`hotkey_click`），但**触发模式本身**没解耦。

## 修复方案

新增 wechat 专属触发模式字段，wechat 设置区的"触发方式"只影响 wechat 模式；focused_app/字幕仍用全局 `interaction_mode`（托盘菜单入口 `kMenuHoldToTalk`/`kMenuClickToTalk` 保留）。

### 1. 配置（`app_config.h` / `app_config.cc`）

- `WechatInputMethodConfig` 增 `InteractionMode trigger_mode = InteractionMode::kHoldToTalk;`
- TOML 序列化 `[wechat_input_method].trigger_mode`，复用 `InteractionModeName` / `InteractionModeFromName`
- 旧配置迁移：加载时若该字段缺失，`trigger_mode = config.interaction_mode`（继承旧选择），并把顶层 `interaction_mode` 重置为 `kHoldToTalk`（focused_app 不再继承 wechat 的点按式），首次 `Save()` 后落定

### 2. UI（`settings_dialog.cc`）

- 触发方式 radio、热键编辑框、`loaded_hotkey_mode_` 读写 `wechat_input_method.trigger_mode`，不再读写全局 `interaction_mode`

### 3. 协调器（`voice_stick_coordinator.cc`）

wechat 上下文改用 `config_.wechat_input_method.trigger_mode`：

| 行号 | 用途 | 改动 |
|---|---|---|
| 528 | `click_mode`（首帧后 SendClick/SendDown） | 读 `trigger_mode` |
| 606/607 | `ActiveHotkey(...)` 参数 | 传 `trigger_mode` |
| 676 | 停止时 SendClick/SendUp | 读 `trigger_mode` |
| 820 | `HandleButtonClick` wechat 分支 | 读 `trigger_mode` |
| 2262-2270 | `InteractionModeToSend()` | wechat 分支按 `trigger_mode` |

`InteractionModeToSend()` 解耦后：

```cpp
if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) {
    return config_.wechat_input_method.trigger_mode == InteractionMode::kHoldToTalk
               ? InteractionMode::kHoldToTalkInstant
               : InteractionMode::kClickToTalk;
}
return config_.interaction_mode;
```

非 wechat 引用（line 808 字幕、842 focused_app、1306/1340 字幕、2285 字幕分段、2350/2364 全局热键）**保持** `config_.interaction_mode` 不变。

托盘菜单（`win32_app.cc:773-780`）保持全局 `interaction_mode`，不改。

### 4. 测试（`core_tests.cc`，TDD）

- 红灯：`TestWechatClickTriggerDoesNotLeakToFocusedApp`--wechat `trigger_mode=kClickToTalk`、全局 `interaction_mode=kHoldToTalk`、切 focused_app，断言 `InteractionModeToSend()==kHoldToTalk` 且 `HandlePrimaryButtonDown` 进录音态
- 现有 wechat click_to_talk 测试（`TestCoordinatorWechatClickToTalkSendsClickOnStart/Stop/AudioEndOvertakes/StaleActive`）把 `config.interaction_mode=kClickToTalk` 改为 `config.wechat_input_method.trigger_mode=kClickToTalk`
- 迁移测试：旧配置（仅顶层 `interaction_mode=click_to_talk`）加载后 `trigger_mode=kClickToTalk`、顶层 `interaction_mode=kHoldToTalk`
- 往返测试：`trigger_mode` 序列化往返

### 5. 文档

固件侧无变化（仍收 `hold_to_talk` / `click_to_talk` / `hold_to_talk_instant`）。`Doc/Ref/protocol.md` 桌面端发送逻辑描述按需同步。README 配置说明补充 `trigger_mode`。

## 迁移策略（已确认）

旧配置顶层 `interaction_mode=click_to_talk` 迁移时**归属 wechat**：`trigger_mode=click_to_talk`，全局 `interaction_mode` 重置为 `kHoldToTalk`。

- wechat 点按式用户：保留选择 + bug 修复 ✓
- 用托盘菜单为 focused_app 选点按式的用户：focused_app 变回 hold，可在托盘菜单重选

## 验证

`build_win.bat` 构建 + `ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿；真机：wechat 选点按式 -> 切 focused_app -> 长按主键应正常录音。
