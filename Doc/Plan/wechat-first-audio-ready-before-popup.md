# 微信模式：首帧音频就绪再弹微信输入法框

## 背景

用户反馈：键盘快捷键直接激活微信语音输入法识别很快，但用 Stick 设备明显卡顿——长句说完了微信还在识别、设备屏幕仍 Listening。已确认键盘方式下 Stick 未录音（微信直接用电脑真实麦，本机直采零链路延迟）。

## 根因（已验证）

`StartWechatInputMethodSession`（`voice_stick_coordinator.cc:499`）当前时序：

1. 设备切换 `auto_switch`（COM 枚举 + `SetDefaultEndpoint`，数十 ms）
2. **`SendDown`**（`SendInput` Ctrl+Win）→ 微信弹框，立即从 CABLE Output 取音
3. WASAPI `renderer.Start`（`OpenAndInitialize` 重建 COM + 起线程，数十 ms）
4. 固件首帧 Opus 经 BLE(7.5ms) 到达 → 解码 → 写 ring_buffer
5. WASAPI 渲染线程首次 `PumpOnce` → CABLE Output 才有 PCM

步骤 2（微信弹框）早于音频链路就绪。微信弹框后到首帧 PCM 到达 CABLE Output 的窗口里，微信读到静音/空数据，首字迟迟识别不出 → 主观卡顿。

代码注释 `voice_stick_coordinator.cc:560-562` 旧权衡“音频晚几十毫秒不影响识别（微信面板有缓冲）”被用户反馈推翻。

固件侧已无优化空间：instant 模式按下即 `start_recording` + `button_down`（`main.c:930-949`），7.5ms fast interval 固定（`voice_ble.c:907`）。

## 方案（方向 A）

反转时序：首帧 PCM 写入 ring_buffer 后才 `SendDown` 弹框。

### 新时序

1. 创建 decoder/ring_buffer/renderer（首次）
2. 设备切换 `auto_switch`
3. WASAPI `renderer.Start`（提前到 `SendDown` 前；失败直接返回，不补 `SendUp`，简化错误路径）
4. 锁内：设置 `active_session_id_` / `wechat_input_method_active_=true` / `kRecording` / `wechat_hotkey_sent_down_=false`
5. `ScheduleRecordingHardTimeout`
6. `SendUiStateForActiveDevice("recording")`
7. **不立即 `SendDown`**（等首帧）
8. `HandleWechatInputMethodAudioFrame` 收到首帧（非 end）解码成功 → 锁外 `SendDown` → `wechat_hotkey_sent_down_=true`

### 新增状态

- `bool wechat_hotkey_sent_down_`：是否已 `SendDown`，决定 `Stop` 时是否配对 `SendUp`。

### `StopWechatInputMethodSession` 调整

- 仅当 `wechat_hotkey_sent_down_` 为 true 才 `SendUp`。
- 末尾重置 `wechat_hotkey_sent_down_ = false`。

### 边界

- 首帧前 button_up：`Stop` 不发 `SendUp`（未 down），正常收尾。设备屏幕已 recording，微信未弹框，用户松开即结束。
- 首帧前断连：同上，断连清理走 `Stop`。
- 首帧解码失败：不 `SendDown`，等下一帧成功。硬超时兜底。
- 首帧是 end（空 payload，极端短按）：不 `SendDown`，正常收尾。
- `SendDown` 失败（系统异常）：锁外 `StopWechatInputMethodSession`（不 `SendUp`）+ `ShowError` + `EnterReady`。
- WASAPI Start 失败：直接返回 false，未 `SendDown` 不补 `SendUp`（删除原 569-570 补 `SendUp`）。

## 代价

微信弹框延迟 ≈ 设备切换 + WASAPI Start + 固件首帧 BLE 延迟（约 100~200ms）。但弹框即有有效音频流，首字立即识别。设备屏幕仍立即显示 recording 提供按下反馈。

## TDD 计划

1. `StartWechatInputMethodSession` 后 `SendDown` 未被调用（`sent_down_=false`），`renderer.Start` 被调用。
2. 收到首帧解码成功 → `SendDown` 被调用一次，`sent_down_=true`。
3. 首帧前 button_up → `Stop` 不 `SendUp`。
4. `StopWechatInputMethodSession` 在 `SendDown` 后 → `SendUp` 被调用。
5. WASAPI Start 失败 → 不 `SendDown`，不补 `SendUp`，返回 false。
6. 首帧解码失败 → 不 `SendDown`。
7. 首帧是 end（空 payload）→ 不 `SendDown`，正常收尾。

## 风险

- 弹框延迟 100~200ms，若用户感觉“按了没弹”需评估。但首字快是用户痛点，且设备屏幕立即 recording 提供反馈。
- `SendDown` 移到首帧路径，须确保锁外调用（`SendInput` 同步不持 `audio_mutex_`）。
