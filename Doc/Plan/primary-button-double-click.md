# 主按键双击检测方案

## 概述

在现有多功能主按键（正面按键 / `primary`）上新增**双击检测**手势：用户在空闲态快速双击主按键时，桌面端注入一次 Enter 按键事件。双击不与现有按住录音（`hold_to_talk`）或单击切换录音（`click_to_talk`）功能冲突。

## 设计决策：固件端检测

双击检测在**固件端**完成，桌面端仅响应新 BLE 事件。原因：

1. **固件已持有按键时序**：`s_primary_down_us`、`elapsed_button_ms()` 天然支持高精度按压时长判定。
2. **避免无效音频管线启停**：若在桌面端检测，第一次短按已触发录音开始/停止、提示音播放、Opus 编码启动，造成不必要的功耗和音频伪影。
3. **协议最小侵入**：新增一个 `button_double_click` 事件，桌面端仅需处理该事件并调用已有的 Enter 注入路径。
4. **桌面端保持状态机纯粹**：固件上报原始手势事实（双击），桌面端解释为 Enter 注入，符合"固件报事实、桌面端做决策"的现有架构原则。

## 时序参数

| 参数 | 值 | 说明 |
|---|---|---|
| `DOUBLE_CLICK_HOLD_THRESHOLD_MS` | 300 ms | 按住超过此时间视为长按（正常录音），短于此时间视为"短击" |
| `DOUBLE_CLICK_WINDOW_MS` | 500 ms | 第一次短击释放后，等待第二次按下的最大窗口 |

## 状态机

```
                 ┌──────────────────────────────────┐
                 │              IDLE                 │
                 │  (等待按键)                        │
                 └────┬────────────────────┬─────────┘
                      │                    │
            DOWN(记录时间戳)           DOWN(记录时间戳)
            hold_to_talk            click_to_talk
                      │                    │
                      ▼                    ▼
                 ┌─────────┐         ┌──────────┐
                 │ PRESSED │         │ 正常处理  │
                 │ (计时中) │         │ (现有逻辑) │
                 └─┬───┬───┘         └──────────┘
                   │   │
        按住≥300ms │   │ 释放(<300ms)
          (长按)   │   │ (短击)
                   │   │
                   ▼   ▼
              ┌──────────┐    ┌──────────────────┐
              │ RECORDING │    │ WAIT_DOUBLE      │
              │ (正常录音) │    │ (等第二次按下)     │
              └──────────┘    └──┬───────────┬───┘
                                 │           │
                    500ms内DOWN  │           │ 500ms超时
                    (双击确认)    │           │ (单击超时)
                                 ▼           ▼
                         ┌──────────┐  ┌──────────┐
                         │ 发送      │  │ 发送      │
                         │ double_   │  │ button_  │
                         │ click     │  │ click    │
                         │ + Enter   │  │ (短单击)  │
                         └──────────┘  └──────────┘
```

### hold_to_talk 模式时序

```
双击手势:
  按下 → <300ms释放 → <500ms内再次按下 → 释放
  效果: 桌面端注入 Enter（不触发录音）

正常录音:
  按下 → 持续≥300ms → 录音开始 → 释放 → 录音结束 → ASR
  效果: 与现有行为完全一致，无额外延迟
```

### click_to_talk 模式时序

click_to_talk 模式下固件原本就将 DOWN 和 UP 合并为 `button_click` 事件上报。双击检测在 click_to_talk 模式下同样适用：固件跟踪连续两次 `button_click` 事件的间隔，若 < 500ms 则发送 `button_double_click` 替代第二次 `button_click`。

```
双击手势:
  第一次点击 → button_click (开始录音) → <500ms内第二次点击
  → button_double_click (取消录音 + 注入Enter)

正常单击:
  第一次点击 → button_click (开始录音)
  第二次点击(间隔>500ms) → button_click (停止录音)
```

> **注意**：click_to_talk 模式双击检测在固件 `handle_primary_down` 中实现。第一次点击正常启动录音；若第二次 DOWN 在 500ms 窗口内到达，取消录音并发送 `button_double_click`。桌面端收到后执行 Enter 注入。

## BLE 协议扩展

### 新增事件：`button_double_click`

```json
{"event":"button_double_click","button":"primary"}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `event` | string | 固定值 `"button_double_click"` |
| `button` | string | 固定值 `"primary"` |

该事件通过 `state_tx`（type=0x10）通知通道上报，与现有 `button_down`、`button_up`、`button_click` 事件同级。

## 涉及改动

### 固件端 (`firmware/`)

| 文件 | 改动 |
|---|---|
| `main/main.c` | 新增双击状态机、定时器；修改 `handle_primary_down` / `handle_primary_up` |
| `components/voice_ble/include/voice_ble.h` | 声明 `voice_ble_send_button_double_click()` |
| `components/voice_ble/voice_ble.c` | 实现 `voice_ble_send_button_double_click()` |

### Windows 桌面端 (`desktop/windows/`)

| 文件 | 改动 |
|---|---|
| `src/voice_stick_coordinator.h` | 新增 `HandleButtonDoubleClick()` 声明；`InputInjector` 新增 `SendEnter()` 纯虚方法 |
| `src/voice_stick_coordinator.cc` | 实现 `HandleButtonDoubleClick()`；在 `HandleStateEvent` 中路由新事件 |
| `src/input_injector_win.h` | `SendEnter()` 提升为 public |
| `src/input_injector_win.cc` | 无改动（`SendEnter()` 已实现） |

### macOS 桌面端 (`desktop/macos/`)

| 文件 | 改动 |
|---|---|
| `Sources/VoiceStickApp/VoiceStickCoordinator.swift` | 新增 `handleButtonDoubleClick()`；在事件分发中路由 |
| `Sources/VoiceStickApp/InputInjector.swift` | 新增 `sendEnter()` public 方法 |

### 文档

| 文件 | 改动 |
|---|---|
| `Doc/Ref/protocol.md` | 补充 `button_double_click` 事件说明 |

## 防抖与边界条件

1. **按住阈值**：按住 ≥ 300ms 视为长按，立即进入录音流程，不会触发双击检测。这意味着正常录音零额外延迟。
2. **双击窗口**：第一次短击释放后 500ms 内未检测到第二次按下，视为单次短击（发送 `button_click`，桌面端因录音时长 < 0.5s 自动丢弃）。
3. **第二次按下忽略录音**：双击确认后，第二次按下的 DOWN/UP 不启动录音管线，不播放提示音。
4. **BLE 断连重置**：BLE 断连时 `s_double_click_pending` 等状态随 `APP_EVENT_BLE_DISCONNECTED` 重置。
5. **与 PendingConfirmation 兼容**：空闲态双击才发 Enter。若当前在 `pending_confirmation` 状态，短按仍走现有确认/暂停逻辑（由 `handle_primary_down` 中的 `s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION` 分支优先处理）。
6. **OTA 期间禁用**：OTA 进行中 (`s_ota_updating`) 不响应按键。
7. **远程按键兼容**：`APP_INPUT_SOURCE_REMOTE`（热键/远程）路径不走双击检测，直接走现有逻辑。

## 测试要点

1. 空闲态双击 → Enter 注入（Windows：`SendInput VK_RETURN`；macOS：`CGEvent 0x24`）
2. 空闲态按住 ≥ 300ms → 正常录音（验证无额外延迟）
3. 空闲态单击（< 300ms 按下，释放后 > 500ms 无第二次按下）→ 短录音被丢弃（< 0.5s），设备回到 ready
4. 双击窗口内第二次按下 → 不启动录音、不播放提示音
5. click_to_talk 模式双击 → Enter 注入，录音取消
6. pending_confirmation 状态下短按 → 仍为确认/暂停（不触发双击 Enter）
7. OTA 期间按键 → 无响应
8. 热键触发 → 不受双击检测影响
