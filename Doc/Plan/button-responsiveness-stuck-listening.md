# 录音按钮响应与卡 listening 根治

## 背景与症状

用户报告默认 `hold_to_talk` + `focused_app` 模式下两个间歇性问题：

1. **按下了没唤醒语音识别**：有时按下主键，桌面端不进 recording。
2. **松开了还卡在 listening**：有时松开主键，录音浮窗不消失，且之后再按也没反应。

## 根因

### 根因 A：300ms hold 阈值 + 双击窗口吞掉短按（"没反应"主因）

`firmware/main/main.c:956-964`，默认 `hold_to_talk` 按下后仅置 `s_hold_threshold_pending` 并启动 300ms 定时器（`DOUBLE_CLICK_MAX_PRESS_MS=300`, `main.c:51`），**不启动录音、不发 `button_down`**。按住不足 300ms 松开走 `main.c:1022-1031` 进 500ms 双击窗口，超时后 `main.c:1484-1492` 补发 `button_click`（非 `button_up`）。桌面端 `HandleButtonClick` 在 `focused_app`+非 `click_to_talk` 模式直接 `SendUiState("ready")` 返回（`voice_stick_coordinator.cc:714-716`），**不录音**。用户体感"按了没反应"，且按下到"无反馈"要等 800ms。

### 根因 B：ble_ready 过渡期静默放弃

`main.c:1453-1479`，hold 阈值到点时若 `voice_ble_is_ready()=false`（订阅过渡/连接抖动），`send_state_json` 返回 `INVALID_STATE`（`voice_ble.c:959-963`），`button_down` 丢弃，进入最长 2000ms 重试（`RECORDING_RETRY_WINDOW_MS`, `main.c:59`），仍未就绪则**静默放弃**，用户按住完全无反应。

### 根因 C：focused_app 残留 recording 无自愈 + 无超时兜底（"卡 listening"主因）

`button_up` 与 `audio_end` 均走 BLE notify **无 ACK**（`voice_ble.c:884,1103`）。松开瞬间若 `s_state_subscribed`/`s_audio_subscribed` 瞬变（`voice_ble.c:485-486,549-550`），两帧均可能被以 `INVALID_STATE` 丢弃。此时：

- BLE 连接未必断开（`IsConnected` 仍 true）-> 断连恢复路径 `voice_stick_coordinator.cc:1799` **不触发**。
- `audio_end` 也丢 -> `HandleAudioFrame` 的 IsEnd 兜底（`coordinator.cc:1046-1049`）不触发。
- `BeginWaitingForAudioEnd` 仅在 `button_up` 到达后启动（`coordinator.cc:1028`）-> **从未启动**。
- 桌面端 recording 状态**无独立超时**，永久卡 `kRecording`。

更糟：卡死后 `HandlePrimaryButtonDown` 第 983 行 `if (session_state_ != kReady || active_device_id_.has_value()) { RefreshDeviceUiState; return; }` **吞掉后续所有 `button_down`**--与 wechat 模式已有的"残留 active 先 Stop 再 Start"自愈（见 `wechat-doubleclick-enter-and-stale-recovery.md` 修复 2b）不同，focused_app 路径无此自愈。故"卡 listening"后**再按也没反应**，两症状合一。

### 根因 D：instant 模式短按暂缓 button_up 致固件 s_recording 残留（切 instant 的前置风险）

`hold_to_talk_instant` 按下即 `start_recording`+发 `button_down`（`main.c:930-949`），但松开若 duration<300ms 仍走 `main.c:1063-1071` 进双击窗口**暂缓 `button_up` 且不 `stop_recording`**。双击第二击按下（`main.c:868-876`）也不停止录音。结果 `s_recording` 永久 true，下次按下 `start_recording` 第 508 行 `if (s_recording ...) return 0` -> 按下无反应。wechat 模式靠桌面端残留自愈掩盖了用户侧症状，但**固件侧 `s_recording` 残留未解决**。

**结论：方案②切 instant 必须同时重构固件双击时序（松开即 stop+发 button_up，不暂缓），否则引入根因 D 的新 bug。**

## 方案①：桌面端 recording 超时兜底 + 残留 button_down 自愈（必做，治"卡 listening"）

单文件 `desktop/windows/src/voice_stick_coordinator.cc` + 测试 `core_tests.cc`。

### ①-a：recording 硬超时兜底

`HandlePrimaryButtonDown` 进入 recording 时启动"录音最长时长"定时器（建议 `kRecordingHardTimeoutMs = 120s`，覆盖正常最长录音）。超时未收到 `button_up`/`audio_end` 则 `CancelShortRecording()` 回 ready。

复用现有 detach 线程 + generation 模式（参考 `ScheduleAudioEndTimeout`, `coordinator.cc:1662-1678`）：
- `button_down` 进 recording -> 启动硬超时线程。
- `button_up`/`audio_end`/断连/取消 -> 取消硬超时（复用 `CancelAudioEndTimeout` 的 generation 机制或新增）。
- 超时触发 -> 锁内校验 session_id/device_id 仍一致 -> `CancelShortRecording`。

### ①-b：残留 recording 时新 button_down 先 Stop 再 Start（仿 wechat 修复 2b）

`HandlePrimaryButtonDown` 第 983 行由"非 kReady 直接 return"改为"先停旧会话再 Start 新会话"。安全前提同 wechat：固件 hold_to_talk 录音中再按主键走 hold_threshold 分支 return（不发新 `button_down`），故桌面端收到 `button_down` 时若 session_state_!=kReady 必为残留，可安全 Stop+重启。

## 方案②：默认 hold_to_talk_instant + 固件双击时序重构（治"没反应"）

用户已选此方向。改动跨固件 + 桌面端。

### ②-a：桌面端默认下发 instant

`InteractionModeToSend()`（`coordinator.cc:2074-2083`）当前仅 wechat+hold_to_talk 下发 instant，扩展为**所有 hold_to_talk 模式（focused_app/subtitle/wechat）均下发 `hold_to_talk_instant`**。`config_.interaction_mode` 仍是 `kHoldToTalk`，所有 `== kHoldToTalk` 判断不变（与现有 wechat 处理一致）。

### ②-b：固件 instant 松开立即 stop + 发 button_up，不进双击窗口

`handle_primary_up`（`main.c:991`）增加 instant 分支：**松开立即 `stop_recording()` + 发 `button_up`（携带 session_id），不进双击窗口**。消除根因 D 的固件 s_recording 残留。

### ②-c：双击判定改为"松开后时间窗内第二次按下"

原双击由"短按松开"触发（设 `s_double_click_pending`，第二击按下确认）。改为"松发 button_up 后设 `s_double_click_pending`，500ms 内第二次按下确认双击"：

- `handle_primary_up`（instant 分支）：发 button_up 后置 `s_double_click_pending=true`，启动 500ms 定时器。
- `handle_primary_down`（`main.c:868`）：`s_double_click_pending` 仍判第二击，发 `button_double_click`。此时 `s_recording=false`（松开已 stop），无需停止录音。
- `double_click_timer_cb`（`main.c:1484`）：超时仅清 `s_double_click_pending`，**不补发任何帧**（button_up 已发，桌面端已收尾）。

时序：
```
按下 -> start_recording + 发 button_down（桌面端进 recording）
松开 -> stop_recording + 发 button_up（桌面端 finalizing 等 audio_end）+ 设双击窗口
  500ms 内第二次按下 -> 发 button_double_click（桌面端取消+SendEnter）
  500ms 超时 -> 无操作（本次录音正常走完）
```

双击语义变化：第一击会完整走一次录音周期（出结果），第二击取消并发 Enter。`HandleButtonDoubleClick`（`coordinator.cc:757-795`）已有取消活跃录音逻辑，但需确认 `pasted_final_text_` 已粘贴后的取消边界。

## 备选②'：保留 hold_to_talk 缩短阈值（供风险对比，非用户当前选择）

仅改两个常量：`DOUBLE_CLICK_MAX_PRESS_MS 300->120`、`DOUBLE_CLICK_WINDOW_MS 500->300`。按下 120ms 后即录音（比 300ms 快 2.5 倍），双击时序不变、零根因 D 风险。但仍有 120ms 延迟（非即响应），且未根治根因 B。

**对比**：②响应最快（即按即录）但跨端改动大、双击语义变；②'改动极小、双击语义不变但有 120ms 延迟。用户已选②，RFC 按②推进，②'仅作风险备案。

## TDD 计划

### 桌面端（`core_tests.cc`，基于现有 Fake/Mock）

1. `TestCoordinatorRecordingHardTimeoutRecoversFromLostButtonUp`：button_down 进 recording，**不发 button_up/audio_end**，推进时钟超过硬超时 -> 最终 `sent_ui_states.back().state == "ready"` 且 `session_state_ == kReady`。
2. `TestCoordinatorRecoveringButtonDownStopsStaleRecording`：button_down 进 recording 后直接发第二个 button_down（模拟残留）-> 旧会话 Stop、新会话 Start（`asr` Cancel 被调、新 session_id 生效）。
3. `TestCoordinatorInstantShortPressSendsButtonUpImmediately`：下发 mode 为 instant 后短按（模拟固件发 button_down + 短按 button_up）-> 桌面端正常进 finalizing，不卡 recording（回归保障）。

### 固件（无自动化测试，`idf.py build` + 真机验证）

固件改动靠真机验证：
- 短按（<300ms）松开：固件日志确认 `stop_recording` + `button_up` 立即发出，`s_recording` 回 false。
- 双击：第一击松开后 500ms 内第二击按下 -> `button_double_click` 发出，无残留 recording。
- 连续短按 10 次：每次 `s_recording` 正确归零，无"按下无反应"。

## 验证

1. `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿（含 3 新测试）。
2. `build_win.bat` 构建，核对 `VoiceStick.exe` 时间戳与体积（历史假成功教训）。
3. 固件 `idf.py build` 编译通过 + 真机验证上述时序。
4. 新测试函数加入 `core_tests.cc` 的 `main()`。
5. desktop/windows 用 `git add -f` 提交。

## 实施步骤

1. 本 RFC 确认。
2. 桌面端：写 3 个失败测试（红）-> 实现 ①-a/①-b/②-a（绿）-> 重构。
3. 固件：实现 ②-b/②-c，`idf.py build` + 真机验证时序。
4. 构建 + ctest 全绿 + 真机回归。
5. 更新记忆：`主键长按/双击时序`（instant 默认化 + 双击时序变更）、新增固件 s_recording 残留教训。
6. `git add -f` 提交。

## 风险

- ②双击语义变化（第一击出结果再取消）可能影响"双击发 Enter"的既有用户习惯，需真机回归双击场景。
- ①-a 硬超时 120s 若用户超长录音会被误杀；可配置或取更大值。
- 固件 ②-b/②-c 改动 `handle_primary_up`/`handle_primary_down`/`double_click_timer_cb` 三处共享入口，须审查对 hold_to_talk（若仍保留）/click_to_talk 路径的副作用（参考 `shared-event-handler-mode-branch-swallows-generic-action` 教训）。

## 实施结果与方向调整（2026-07-08）

### 根因 D 纠正

RFC 原判断"hold_to_talk_instant 短按松开进双击窗口暂缓 button_up 但**不 stop_recording**，致固件 s_recording 残留"**不成立**。重读 `main.c:1057`：instant 松开时 `s_recording=true`，`1057` 行 `if (s_recording) { s_primary_session_id = stop_recording(); }` **总会先 stop**，再进双击窗口暂缓 `button_up`。固件 s_recording 不会残留。故 ②-b/②-c（固件双击时序重构）不是 instant 生效的前提，仅是体验优化，风险/收益比下降，**暂缓**。

### 已落地（保留）

- **①-a recording 硬超时兜底**：`ScheduleRecordingHardTimeout`/`CancelRecordingHardTimeout`，`HandlePrimaryButtonDown` 进 recording 时调度，`EnterReady`/`EnterFinalizing` 取消。默认 120s，构造可注入（测试用 300ms）。根治 button_up/audio_end 都丢致永久卡 listening。
- **①-b 残留 button_down 自愈**：`HandlePrimaryButtonDown` 同设备卡 recording 时先 `CancelShortRecording` 再 Start 新会话。根治卡死后按没反应。

### 已回退（用户反馈）

- **②-a 默认下发 hold_to_talk_instant**：曾实现让所有 hold_to_talk 模式下发 instant（按下即录音）。**用户真机反馈"按住立即进 recording 体验不好，还是需要有 300ms 延迟"**，已回退。`InteractionModeToSend` 恢复为仅 wechat+hold_to_talk 下发 instant（wechat 的 instant 为降低微信弹框延迟的独立优化，保留）。focused_app 恢复 300ms 意图确认阈值。

### 暂缓

- **task4 固件 ②-b/②-c**：根因 D 不成立后非必需，暂缓。若后续真机验证仍需"短按干净丢弃"（当前 instant 短按靠 audio_end 收尾出空结果），再评估。

### 真机验证待办

focused_app 模式（已回退 300ms 阈值 + ①-a/①-b 兜底）真机验证：①卡 listening 是否还会出现 ②卡死后按主键是否恢复 ③正常录音/双击 Enter 回归。
