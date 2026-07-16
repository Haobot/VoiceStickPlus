# 点动模式双击回车（双击不录音直接回车，语义 c）

## 根因（确证）

- 固件 click_to_talk 第一次 click 立即 `start_recording` + 发 `button_click(start)`（`main.c:979-992`）。第二次按下 <500ms 发 `button_double_click`（`:892-898`），≥500ms 发 `button_click(stop)` 走 toggle 停止（`:919-929`）。
- 桌面端 `HandleButtonDoubleClick`（`voice_stick_coordinator.cc:880-918`）对 primary 双击发 `SendEnter`，但第一次 click 已启动录音（Typeless 进入录音态）：双击 <500ms 时若首帧已到则 Stop 发 SendClick OFF（Typeless 短录音无文字可送）；双击 ≥500ms 时固件改发 `button_click(stop)`，桌面端走 `HandleButtonClick` 停止分支（`:824`）不发 Enter。
- 需求 (c)：双击不录音直接回车（发送输入框已有文字，像 hold 模式那样）。

## 方案（固件为主，桌面端无需改）

固件 click_to_talk 第一次 click 延迟双击窗口再确认启动，复用 hold 模式"延迟确认"思路：

- **第一次 click**（按下+抬起）：设 `s_click_to_talk_pending_start` + 启动双击窗口定时器，**不 start_recording、不发任何事件**。
- **双击窗口内第二次 click**：发 `button_double_click`（不启动录音）-> 桌面端 `HandleButtonDoubleClick` 见 `wechat_active=false` 走 else 分支（`:900-910`）直接 `SendEnter`，干净回车。
- **双击窗口超时**（无第二次 click）：确认启动 `start_recording` + 发 `button_click(start)`，进入正常 toggle 录音。
- 录音中第二次 click（toggle 停止）逻辑（`:892-907` / `:919-929`）保留不变。

桌面端 `HandleButtonDoubleClick` 已正确处理 `wechat_active=false -> SendEnter`，**无需改动**。

## 代价（需确认）

**点动模式单击启动延迟 = 双击窗口时长**。当前点动是"按下立即启动"，改后"按下 → 等窗口 → 确认启动"。这是 (c) 语义的必然代价（要区分单击与双击就必须等窗口）。与 memory [[hold-threshold-keep-300ms]] 的 300ms 意图确认同源，但点动模式用户对延迟可能更敏感。

窗口时长权衡：

- 窗口短（如 300ms）：启动延迟小，但用户双击要快（<300ms），容易失败。
- 窗口长（如 500ms）：双击宽容，但启动延迟感明显。

## 固件改动（`firmware/main/main.c`）

1. 新增 `static bool s_click_to_talk_pending_start;`（首次 click 后等双击窗口确认）。
2. `handle_primary_down` click_to_talk 首次按下（`s_recording=false` 且非 pending、非 air_mouse、非远程）：设 `s_click_to_talk_pending_start=true` + 记 `s_click_to_talk_first_click_us` + 启动 `s_double_click_timer`（窗口时长），不发事件、不 start_recording。
3. `handle_primary_down` click_to_talk `s_click_to_talk_pending_start=true` 时第二次按下：发 `button_double_click`，清 pending、停定时器。
4. `double_click_timer_cb` 新增 `s_click_to_talk_pending_start` 分支：确认启动 `start_recording` + 发 `button_click(start)`，清 pending。
5. `handle_primary_up` click_to_talk 段（`:1028-1030` return）保持--pending 期间抬起不影响定时器。
6. 断连/电源关等清理路径（`:1219-1224` 附近）重置 `s_click_to_talk_pending_start`。

## TDD

桌面端无需新测（`HandleButtonDoubleClick` 现有 `TestCoordinatorWechatInputMethodDoubleClickSendsEnter` 已覆盖 wechat_active=false -> SendEnter）。固件靠 `idf.py build` 编译 + 真机验证。

## 真机验证

点动模式 + Typeless（ralt）：
1. 单击：按下 → 等窗口 → 确认启动录音（Typeless 弹框），再单击停止。
2. 双击：快速双击 → 不启动录音 → 直接回车发送输入框已有文字。
3. 回归：长按式（微信 ctrl+win）不受影响。

## 不改动范围

- hold 模式、hold_to_talk_instant。
- 桌面端协调器、UI、配置。
- 录音中 toggle 停止逻辑（`:892-907` / `:919-929`）。
- 协议（无新字段，仍用 button_click/button_double_click）。
