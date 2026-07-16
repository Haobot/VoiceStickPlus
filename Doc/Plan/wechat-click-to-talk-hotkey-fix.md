# 第三方输入法点按式热键修复（ralt 不弹 + 停止竞态）

## 根因（已证实）

### Bug1：点按式按第一下完全不弹

- 现象：`hotkey=ralt` + 点按式，按设备主键，Typeless 语音面板完全不弹出。
- 证据：
  - 物理按右ALT（按下+松开）能弹 Typeless，且 Typeless 热键就是右ALT（用户确认）。
  - 日志 75671/75782：桌面端 `SendDown end (popup triggered)`，`SendInput` 调用返回成功。
  - 代码 `wechat_input_method_hotkey.cc:118`：点按式第一次设备 click 调 `SendDown()`，只发右ALT **按下**（`key_up=false`），**不发松开**；`SendUp()` 要等第二次设备 click（`StopWechatInputMethodSession`）。
- 根因：Typeless 是"按一下进入录音"的点按式语义，需要完整的**按下+松开**才算一次点击。VoiceStick 把按下/松开拆到两次设备 click，第一次只按下不松开，Typeless 等不到松开，不触发。

### Bug2：点按式按第二下停止会被误判为启动新会话（又弹一次）

- 现象：点按式按第二下想停止，结果又弹一次（toggle 失控）。
- 证据：日志 75672-75677：
  - 75672 第二次 `button_click`(停止, session=17) 到达
  - 75674 `recording->ready reason=wechat_audio_end`（audio_end 先到停了会话）
  - 75677 又 `button_down arrived`（停止 click 被误判为启动新会话）
- 根因：固件 click_to_talk 停止时 `stop_recording()`（产生 audio_end 音频帧，走 audio_tx）先于 `voice_ble_send_button_click`（走 state_tx）。audio_end 帧先到桌面端，`HandleWechatInputMethodAudioFrame` 的 `end_of_stream` 分支（coordinator.cc:554-559）先 `StopWechatInputMethodSession` 把 `wechat_input_method_active_=false`；随后到达的"停止"button_click 在 `HandleButtonClick`（786-791）因 `IsWechatInputMethodActive()=false` 走 else 分支，误判为启动新会话。

## 修复方案

### 修复1：点按式发完整 click（down+up）

`wechat_input_method_hotkey.h/cc`：

- `IWechatInputMethodHotkey` 新增纯虚 `virtual bool SendClick() const = 0;`（发 down+up 序列，模拟一次物理点击）。
- `WechatInputMethodHotkey::SendClick()`：构造两个 INPUT（同 vk，一 `key_up=false`、一 `key_up=true`，扩展键仍带 `KEYEVENTF_EXTENDEDKEY`），一次 `SendInput` 发出。
- `FakeWechatInputMethodHotkey`（core_tests.cc）加 `SendClick()` 计数 `send_click_count`。

`voice_stick_coordinator.cc`：

- 首帧后弹框（523-531）：`config_.interaction_mode == kClickToTalk` 调 `SendClick()`，否则 `SendDown()`（hold 模式不变）。
- `StopWechatInputMethodSession`（659-661）：`kClickToTalk` 调 `SendClick()`，否则 `SendUp()`。`wechat_hotkey_sent_down_` 语义复用（"启动热键已发，停止需补发"）。

语义对齐：
- hold_to_talk（微信长按式）：启动 SendDown（按下持续），停止 SendUp（释放）。不变。
- click_to_talk（Typeless 点按式）：启动 SendClick（down+up 进入录音），停止 SendClick（down+up 停止）。每次设备 click 对应一次完整物理点击。

### 修复2：toggle 竞态用 session_id + 时间窗口忽略迟到停止 click

固件 `button_click` 带 session_id，启动与停止 click 是**同一** session_id（stop_recording 返回当前会话 id）。日志 75660/75672 均为 17 实证。

`voice_stick_coordinator.h`：新增成员

- `std::optional<std::uint32_t> last_stopped_wechat_session_id_;`
- `std::optional<std::chrono::steady_clock::time_point> last_stopped_wechat_at_;`

`voice_stick_coordinator.cc`：

- `StopWechatInputMethodSession` 在 reset `active_session_id_` 前，若 `active_session_id_` 有值，记 `last_stopped_wechat_session_id_ = active_session_id_` + `last_stopped_wechat_at_ = now`。
- `HandleButtonClick` wechat toggle 分支（786-791）改为：
  ```cpp
  if (IsWechatInputMethodActive() && active_device_id_ == device_id) {
      HandleWechatInputMethodPrimaryButtonUp(device_id);  // 正常停止
  } else if (IsStaleWechatStopClick(event.session_id)) {
      // audio_end 抢跑已停会话，这是迟到的停止 click，忽略，不启动新会话
      return;
  } else {
      HandleWechatInputMethodPrimaryButtonDown(event.session_id, device_id);  // 启动
  }
  ```
- `IsStaleWechatStopClick`：`session_id` 有值且 == `last_stopped_wechat_session_id_` 且距 `last_stopped_wechat_at_` 在窗口内（2 秒）。

正确性：
- audio_end 先到（竞态）：Stop -> SendClick(停止) + 记 last_stopped；迟到 button_click(停止) 匹配 last_stopped -> 忽略。Typeless 收到一次停止 click。✅
- button_click 停止先到：Up -> Stop -> SendClick(停止) + 记 last_stopped；audio_end 后到 -> `active_session_id_` 已 reset 早退（480-483）。Typeless 收到一次停止 click。✅
- 新启动 click：新 session_id != last_stopped（固件 sid 递增），正常启动。✅

不拆分 `StopWechatInputMethodSession`，不动 renderer/Finish 幂等性，影响面最小。

## TDD 测试清单（core_tests.cc）

红灯先行，`ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests`：

1. 🔴 `TestCoordinatorWechatClickToTalkSendsClickOnStart`：`interaction_mode=kClickToTalk` + wechat + `button_click`(启动) + 首帧 -> `fake_hotkey->send_click_count==1 && send_down_count==0`。当前调 SendDown，红灯。
2. 🔴 `TestCoordinatorWechatClickToTalkSendsClickOnStop`：启动后 `button_click`(停止) -> `send_click_count==2`（启动+停止），`send_up_count==0`。
3. 🔴 `TestCoordinatorWechatClickToTalkAudioEndOvertakesStopClick`：启动 + audio_end（抢跑）+ `button_click`(停止) -> 不启动新会话：`send_click_count==2`（不是 3），无新 recording 状态。
4. 回归：现有 `TestCoordinatorWechatInputMethodButtonDownSendsHotkey`（hold 模式 SendDown/SendUp）、`TestCoordinatorWechatInputMethodAudioEndStopsSessionWithoutButtonUp`、`TestCoordinatorWechatRecordingHardTimeoutRecoversFromLostButtonUp` 等保持绿。
5. `TestWechatInputMethodHotkeyParsing` 加 `SendClick` 接口存在性断言（`WechatInputMethodHotkey("ralt")` 能调 SendClick 返回 true）。

> SendClick 真正发送右ALT down+up 是否被 Typeless 响应，单测只能验证调用次数（fake 计数），实际效果靠真机验证（物理按右ALT 能弹已证明 Typeless 监听右ALT 点击）。

## 实施顺序

1. 🔴 改 `wechat_input_method_hotkey.h/cc` 加 `SendClick`（接口+实现），`core_tests.cc` 的 `FakeWechatInputMethodHotkey` 加 `SendClick`，加测试 1/2/3/5，ctest 确认红灯。
2. 🟢 `voice_stick_coordinator.h/cc`：首帧后 + Stop 按 click_to_talk 调 SendClick；加 `last_stopped_*` 成员 + `IsStaleWechatStopClick` + HandleButtonClick 分支。ctest 转绿。
3. `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿。
4. `build_win.bat` 构建，核对 exe 时间戳/体积。
5. 真机验证：`hotkey=ralt` + 点按式，按一下 Typeless 弹（修复1），再按一下停止不再重复弹（修复2）。

## 不改动范围

- 固件（button_click 带 session_id 已满足，toggle 竞态修复在桌面端）。
- macOS（无 wechat 模式）。
- hold_to_talk 模式行为（SendDown/SendUp 不变，仅 click_to_talk 走 SendClick）。
- 协议（无新字段）。
