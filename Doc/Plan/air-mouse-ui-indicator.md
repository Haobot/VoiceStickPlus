# 体感鼠标模式 UI 提示方案

## 背景

设备屏幕显示 "Ready" 但按下主键无反应（长按、短按均无反应）。

## 根因

体感鼠标（air mouse）模式激活时，设备 UI 不提示，仍显示 "Ready"，用户不知情；此时主键被映射为鼠标左键而非录音触发键，表现为"按下没反应"。

日志证据（`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`，2026-07-07）：

- `[CRD 22:30:14.230] air mouse enabled on VS-D010` —— 体感态被开启（误触侧键）
- `[BLE 22:47:24.308] {"event":"button_click","button":"primary","duration_ms":3620}`
- `[CRD 22:47:24.309] air mouse primary click on VS-D010, left button` —— 长按主键 3.62 秒被当鼠标左键

代码 bug（`desktop/windows/src/voice_stick_coordinator.cc:724` `ToggleAirMouse`）：

- **开启**体感态：只发 `SendAirMouseEnabled(true)`，**不下发任何 ui_state** ❌
- **关闭**体感态：发 `SendAirMouseEnabled(false)` + `SendUiState("ready")` ✅
- 开/关不对称。固件 `set_air_mouse_enabled`（`firmware/main/main.c:1703`）只启停 IMU，不改 UI；`app_ui_state_t` 枚举（`main.c:150`）无体感态。

桌面端在体感态下：
- `HandlePrimaryButtonDown`（line 844）直接 return，不录音
- `HandleButtonClick`（line 583）把主键点击映射为 `ClickLeftButton()`

## 目标

开启体感态时：① 设备屏幕显示 "Air Mouse" 提示；② 桌面端托盘菜单显示哪些设备处于体感态。关闭时回 "Ready"。让用户明确感知当前态，避免误判设备故障。

## 改动清单

### A. 协议 `Doc/Ref/protocol.md`

- `ui_states` 数组（line 75）加 `"air_mouse"`
- 加示例 `{"event":"ui_state","state":"air_mouse","text":""}`
- 字段表说明 `air_mouse` 状态

### B. 固件

1. `ui_status_icons.h`：`ui_status_icon_scene_t` 加 `UI_STATUS_ICON_AIR_MOUSE`
2. `ui_status_icons.c`：`get_scene_image` 加 `AIR_MOUSE` → 复用 `s_cat_resting`（无需美术资源）
3. `ui_status.h`：加 `void ui_status_set_air_mouse(void);`
4. `ui_status.c`：实现 `ui_status_set_air_mouse()` → `set_scene(UI_STATUS_ICON_AIR_MOUSE, "Air Mouse", "Side key exit")`；`render_scene_locked` 给 AIR_MOUSE 区分配色（蓝色背景）
5. `main.c`：`app_ui_state_t` 加 `APP_UI_STATE_AIR_MOUSE`；`apply_app_ui_state` 加 `else if (strcmp(state, "air_mouse") == 0)` 分支调用 `ui_status_set_air_mouse()`

### C. Windows

1. `voice_stick_coordinator.cc` `ToggleAirMouse` 开启分支（line 740 后）加 `ble_->SendUiState("air_mouse", "", device_id);`（关闭已有 `ready`）
2. `win32_app.cc` `ShowTrayMenu` 加体感态状态项（`MF_DISABLED` 列出开启体感的设备，右键时自然刷新）
3. `localization.cc/h` 加中英字符串

### D. 测试（TDD）

- `core_tests.cc` 扩展 `TestCoordinatorAirMouseToggleViaSecondary`：开启后断言 `HasUiState(ble, "air_mouse", "5A74")`；关闭后断言 `HasUiState(ble, "ready", "5A74")`
- 固件：`idf.py build` 编译通过 + 真机验证

### E. macOS

无 air mouse 代码实现（仅 `config.example.toml` 配置项），无需改代码；协议文档更新已覆盖。

## 设计要点

- **无竞态**：体感态下 `active_device_id_` 为空（不录音），`EnterReady` 的 `SendUiStateForActiveDevice("ready")` 不会发给体感态设备，体感态 UI 不会被覆盖。
- **图标复用**：用 `cat_resting` + "Air Mouse" 文本区分，零美术成本；专属鼠标图标留作后续增强。

## 验证

- Windows：`ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿 + 真机
- 固件：`idf.py build` 通过 + 真机：开启体感态屏幕显示 "Air Mouse"，关闭回 "Ready"

## 即时解决（不改代码）

按一次侧键退出体感态，主键立即恢复录音。
