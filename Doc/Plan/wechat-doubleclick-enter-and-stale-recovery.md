# 微信输入法模式双击 Enter 恢复与会话残留自愈

## 背景

v1.9.0 下用户报告两个微信输入法（`wechat_input_method`）输出模式的问题：

1. **录音键双击发送 Enter 失效**：主键双击不再注入 Enter。
2. **长按录音按钮激活微信输入法不稳定**：有时长按完全无反应（设备不进 recording 或微信面板不弹），间歇性、无规律。

## 根因

### 问题 1：双击 Enter 被 wechat 分支吞掉

`desktop/windows/src/voice_stick_coordinator.cc:651-657`，`HandleButtonDoubleClick` 在主键分支开头有：

```cpp
if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) {
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
        EnterReady("double_click_cancel_wechat");
    }
    return;   // ← wechat 模式下双击永远不到 input_injector_->SendEnter()
}
```

wechat 模式引入时把主键双击复用为「取消当前录音」，导致双击永远不发 Enter。但微信输入法把语音转文字送入输入框后，正需要回车发送——双击发 Enter 在 wechat 模式下应保留。

### 问题 2：会话状态残留无自愈

长按链路：固件 300ms hold threshold → `button_down` → Windows `StartWechatInputMethodSession`（renderer.Start + hotkey.SendDown）→ 音频帧 → 松开 `button_up` → `StopWechatInputMethodSession`（hotkey.SendUp + renderer.Stop）。

`button_up` 走 BLE `state_tx` notify，**无应用层 ACK**，闪断瞬间会丢。丢失后：

- `CancelActiveCycleIfDeviceDisconnected`（`voice_stick_coordinator.cc:1630`）**只在 `on_connection_change` 回调触发**（`Start()` 内 76 行），快速闪断重连（`IsConnected` 短暂 false 再 true）会漏掉。
- `HandleWechatInputMethodAudioFrame`（429-435 / 460-464）收到 audio_end 帧时只 `active_session_id_.reset()`，**不清 `wechat_input_method_active_`、不停 renderer、不松热键**。
- `HandleWechatInputMethodPrimaryButtonDown:392` 若 `IsWechatInputMethodActive()` 为 true 直接 `return`。

结果：`button_up` 丢失 → `wechat_input_method_active_` 残留 true → 下次长按 `button_down` 被 392 行 return 吞掉 → 「完全无反应」。只有重连/重启才恢复，正符合用户描述的间歇性。

### 安全前提（关键）

固件 hold_to_talk 录音中再按主键走 `main.c:942` 的 hold_threshold 分支并 `return`（**不发新 button_down**）；click_to_talk 录音中再按发的是 `button_click` 不是 `button_down`。

**故 Windows 收到 `button_down` 时若 `wechat_input_method_active_=true`，必为残留**，可安全 Stop + 重启，不会误杀进行中的录音。

## 目标

1. wechat 模式主键双击始终注入 Enter（若正在录音先 Stop 再发 Enter）。
2. wechat 会话在任何结束信号丢失时自愈：audio_end 帧到达即结束会话；残留 active 时新 button_down 先 Stop 旧会话再 Start。

## 改动范围

单文件：`desktop/windows/src/voice_stick_coordinator.cc`。配套测试：`desktop/windows/tests/core_tests.cc`。无协议/固件/配置改动。

## 设计

### 修复 1：双击 Enter 合并到通用路径

`HandleButtonDoubleClick`（635-679）删掉 651-657 的 wechat 提前 return，把 wechat 模式并入通用双击路径。取消活跃录音处加 wechat 分支：

```cpp
// 主键双击：取消当前活跃录音后注入 Enter。
if (IsWechatInputMethodActive()) {
    StopWechatInputMethodSession();          // 发 hotkey.SendUp，微信把已识别文字送入输入框
} else {
    std::lock_guard lock(audio_mutex_);
    if (active_session_id_.has_value() && active_device_id_ == device_id) {
        CancelAudioEndTimeout();
        asr_->Cancel();
        pending_paste_state_ = {};
        active_session_id_.reset();
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
    }
}
CancelSubtitleCyclesForDevice(device_id, "double_click");
input_injector_->SendEnter();                // 回车发送输入框文字
ble_->SendUiState("ready", "", device_id);
EnterReady("double_click_enter");
```

时序：双击 → Stop wechat（Ctrl+Win 抬起，微信输入法把文字送进输入框）→ SendEnter（回车发送）。

### 修复 2a：audio_end 帧结束整个会话

`HandleWechatInputMethodAudioFrame`（422-465）收到 IsEnd 帧时，由「只 reset session_id」改为「结束整个会话」。覆盖「button_up 丢、audio_end 到达」。

无重复 Stop 风险：
- audio_end 先到 → Stop（SendUp #1, wechat_active=false）→ EnterReady；button_up 后到 → `HandleWechatInputMethodPrimaryButtonUp` 检查 `IsWechatInputMethodActive()=false` → return（无第二发 SendUp）。
- button_up 先到 → Stop（SendUp #1, wechat_active=false）→ EnterReady；audio_end 后到 → `active_session_id_` 已 nullopt → 425 行早退 return。

### 修复 2b：残留 active 时先 Stop 再 Start

`HandleWechatInputMethodPrimaryButtonDown:392` 由「残留 active 直接 return」改为「先 Stop 旧会话再继续 Start 新会话」。覆盖「button_up + audio_end 都丢」。

残留 Stop 会 SendUp 收尾旧热键（必要，否则系统热键状态不同步：系统认为 Ctrl+Win 还按着）。

## 边界与不变量

- 双击时若无活跃录音（wechat 不 active），直接 SendEnter + EnterReady，与 focused_app 路径一致。
- audio_end 触发 Stop 与 button_up 触发 Stop 互斥（先到者 Stop，后到者早退），不重复 SendUp / 不重复 renderer.Stop。
- 残留 button_down 恢复：旧会话 Stop（含 SendUp）后 Start 新会话（含 SendDown），热键状态正确翻转。
- 修复不影响断连兜底（`CancelActiveCycleIfDeviceDisconnected` wechat 分支不变）与 Shutdown 清理。
- 现有 `TestCoordinatorWechatInputMethodHandlesEmptyEndFrame`（button_down→audio→end→button_up）仍通过：end 到达时 Stop+EnterReady，button_up 后到时 wechat_active=false 早退，最终 ready 不变。

## TDD 测试

新增 3 个测试到 `core_tests.cc`，加入 `main()`：

1. `TestCoordinatorWechatInputMethodDoubleClickSendsEnter`：wechat 模式双击主键 → `input.send_enter_called==true`；若双击时 wechat 活跃则 `fake_renderer->stop_count >= 1`。
2. `TestCoordinatorWechatInputMethodAudioEndStopsSessionWithoutButtonUp`：wechat 活跃、收到 audio_end 帧、**不发 button_up** → `fake_renderer->stop_count >= 1` 且最终 `sent_ui_states.back().state == "ready"`。
3. `TestCoordinatorWechatInputMethodRecoversFromStaleActive`：button_down 启动后直接发第二个 button_down（模拟残留）→ `fake_renderer->start_count == 2`（旧 Stop、新 Start）。

## 验证

1. `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿（含 3 个新测试）。
2. `build_win.bat` 构建，核对 `desktop/windows/build-x64/VoiceStick.exe` 时间戳与体积。
3. 新测试函数加入 `core_tests.cc` 的 `main()`。

## 实施步骤

1. 本方案文档。
2. 写 3 个失败测试，跑 ctest 确认红。
3. 实现修复 1 / 2a / 2b，跑 ctest 确认绿。
4. `build_win.bat` 构建验证。
5. 更新记忆 `wechat-output-mode-disconnect-cleanup`（补充 audio_end 与残留 button_down 两个新清理入口）。
6. `git add -f` 提交（desktop/windows 被 ignore）。
