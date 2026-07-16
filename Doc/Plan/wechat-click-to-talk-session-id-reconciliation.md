# 点动模式快速点动错位修复（session_id 校验）

## 根因（确证，代码级）

`HandleButtonClick`（`voice_stick_coordinator.cc:819-835`）的 wechat+click_to_talk 停止判定（:824）：

```cpp
if (IsWechatInputMethodActive() && active_device_id_ == device_id) {
    HandleWechatInputMethodPrimaryButtonUp(device_id);  // 当停止
}
```

**不校验 `event.session_id` 是否等于 `active_session_id_`**。这是 `IsStaleWechatStopClick`（处理同 session_id 迟到停止 click，:826）与这行 active-check 之间的逻辑缺口--stale 机制识别不了"旧会话残留 active + 新 session_id 启动 click"。

错位链：

1. click2(停止, session=N) 的 `button_click` 丢失--固件 `send_state_json` 在 mbuf 耗尽或 `ble_gatts_notify_custom` 失败时返回错误且不重试、`om` 未释放泄漏（`voice_ble.c:976-988`）；`audio_end(N)` 也丢或延迟。
2. `wechat_input_method_active_` 残留 true，`active_session_id_=N`。
3. click3(启动, session=N+1) 到达 -> :824 命中（不看 session_id）-> 被误当停止 -> `HandleWechatInputMethodPrimaryButtonUp` -> Stop 发 SendClick OFF，active reset。
4. click3 的 N+1 音频帧到达 -> `active_session_id_` 已 reset -> :480-483 早退丢弃。
5. 固件仍录 N+1（屏幕 recording），桌面已 ready、Typeless 已关 -> "屏幕 recording 时对方认为结束"。新启动 click 被吞、音频被丢，**不自愈**，等 120s 硬超时。

session_id 单调递增（`main.c:105/515`），新启动 click(N+1) != 残留 active(N)，校验 session_id 能区分"新启动"与"迟到停止"。

## 修复

:824 停止判定加 session_id 匹配：当 `event.session_id` 有值且 ≠ `active_session_id_` 时，不当停止，走 else 启动分支。`HandleWechatInputMethodPrimaryButtonDown`（:427-429）已有残留自愈（`if (IsWechatInputMethodActive()) StopWechatInputMethodSession();` 先停旧再启新），会发 SendClick OFF 对齐 Typeless、再 Start 新会话 N+1。

```cpp
if (IsWechatInputMethodActive() && active_device_id_ == device_id &&
    (!event.session_id.has_value() || !active_session_id_.has_value() ||
     *event.session_id == *active_session_id_)) {
    HandleWechatInputMethodPrimaryButtonUp(device_id);   // 停止当前会话（session_id 匹配）
} else if (IsStaleWechatStopClick(event.session_id)) {
    // 忽略迟到停止 click
} else {
    HandleWechatInputMethodPrimaryButtonDown(event.session_id, device_id);  // 启动（含残留 active 先停旧再启新）
}
```

边界：

- `button_double_click` 不带 session_id（`voice_ble.c:1064-1069`，nullopt）-> `!event.session_id.has_value()` 为 true -> 当"匹配"走停止+Enter。
- `button_click(stop,N)` 正常：N == active(N) -> 停止。✓
- 残留 active(N) + `button_click(start,N+1)`：N+1 != N -> 启动分支 -> 先 Stop 旧(发 SendClick OFF) 再 Start 新(N+1)。✓ Typeless 收到 OFF+ON 对齐。
- `audio_end(N)` 抢跑：Stop + last_stopped=N；迟到 `button_click(stop,N)` -> IsStale -> 忽略。✓
- `audio_end(N)` 抢跑后 active reset + `button_click(start,N+1)`：active=false -> 启动分支 -> 正常启动 N+1。✓

## TDD 测试（`core_tests.cc`）

红灯先行，`ctest -R voicestick_windows_tests`：

1. 🔴 `TestCoordinatorWechatClickToTalkStaleActiveNewClickStartsNew`：click_to_talk+wechat，`button_click(start, N=1)` 启动会话 1（首帧 SendClick ON）后，**不发停止 click、不发 audio_end**（模拟都丢），再发 `button_click(start, N=2)` -> 应启动新会话 2（先 Stop 1 再 Start 2），**不当作停止**。断言 `fake_renderer->start_count >= 2`（会话 1 + 会话 2 都 Start）且最终 `active_session_id` 为 2。
2. 回归：现有 `TestCoordinatorWechatClickToTalkSendsClickOnStart/Stop`、`TestCoordinatorWechatClickToTalkAudioEndOvertakesStopClick`、`TestCoordinatorWechatInputMethodDoubleClickSendsEnter` 等保持绿。

## 实施顺序

1. 本方案文档。
2. 🔴 `core_tests.cc` 加测试 1，ctest 确认红（残留 active 时新 click 被当停止，start_count 不达预期）。
3. 🟢 `voice_stick_coordinator.cc:824` 加 session_id 校验，转绿。
4. `ctest` 全绿。
5. 增量编译 + 核对 exe。
6. `git add -f` 提交。

## 不改动范围

- hold 模式、`audio_end` 抢跑处理、`IsStaleWechatStopClick`、120s 硬超时。
- 固件（`send_state_json` 重试 + `om` 泄漏修复为可选后续，本方案不含，降低停止 click 丢失概率从源头减少触发）。
- 协议、macOS。
