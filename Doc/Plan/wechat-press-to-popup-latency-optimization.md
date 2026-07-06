# wechat 模式按下到弹框延迟优化

## 背景

用户反馈：从按下录音键到微信输入法弹出识别框，过程比直接按微信输入法语音快捷键迟缓。需分析各段延迟并优化。

## 延迟分布（基于代码证据）

从物理按下到微信弹框，分固件段与桌面段：

### 固件段（按下 → button_down notify 发出）

| 环节 | 耗时 | 证据 |
|---|---|---|
| ① 提示音播放 | 80ms 同步阻塞 | `play_prompt_tone` → `audio_pipeline_play_tone(freq, 80, 50)`（main.c:250） |
| ② hold 阈值等待 | **300ms** | `DOUBLE_CLICK_MAX_PRESS_MS=300`（main.c:51），`handle_primary_down:945` 启定时器，300ms 后才进 `start_recording` |
| ③ audio_pipeline_start | 数十~上百 ms | `init_i2s`+`init_codec`（esp_codec_dev_open，含 ES8311 reconfig）+`init_opus`+建任务（audio_pipeline.c:463-528） |
| ④ conn interval 切换 | 0~数百 ms 异步 | `voice_ble_request_fast_interval()` 只在按下 300ms 后的 `start_recording`（main.c:529）才调。注释明说"耗时可达数百毫秒"。**按下时不请求**，button_down notify 发出时 interval 可能仍是 slow |
| ⑤ button_down notify 传输 | 1 个 conn interval | `ble_gatts_notify_custom`（voice_ble.c:1103）下个 event 发出。slow interval=100~400ms，fast=7.5ms |

固件段合计约 **380~780ms**（300ms 阈值 + 几十~上百 ms audio init + 0~400ms notify 传输）。

### 桌面段（button_down 收到 → 微信弹框）

| 环节 | 耗时 | 证据 |
|---|---|---|
| ⑥ BLE notify → coordinator | ~即时 | ValueChanged 回调 → on_state_event → HandleStateEvent 同步直调（ble_central_win.cc:1363 → coordinator.cc:363） |
| ⑦ WASAPI renderer.Start | **几十 ms 同步阻塞** | `StartWechatInputMethodSession` 先 `renderer->Start()`（coordinator.cc:511）。CoInitializeEx+OpenDevice（枚举设备）+InitializeStream（Activate+Initialize+GetService+Start）+起线程（wasapi_virtual_mic_renderer.cc:89-114） |
| ⑧ hotkey.SendDown | ~即时 | SendInput 注入 Ctrl+Win（coordinator.cc:519）。**在 ⑦ 之后**，弹框被 WASAPI Start 推迟 |
| ⑨ 微信响应热键 | 微信侧 | 不可控，与直接按快捷键一致 |

桌面段合计约 **几十 ms**（主要 ⑦），串在链路末端。

### 对比

- VoiceStick 链路：~460~880ms
- 直接按微信快捷键：~几十 ms（系统热键直弹）

三大可优化点：**300ms hold 阈值**、**按下时不预请求 fast interval**、**WASAPI Start 在 SendDown 之前**。

## 优化方案

### 优化 A：按下即请求 fast conn interval

`handle_primary_down`（main.c:942-950）hold_to_talk 分支，在播提示音之前立即调 `voice_ble_request_fast_interval()`。conn update 异步，提前 300ms 请求，到 button_down 发出时多半已切 7.5ms，消除 ⑤ 的 100~400ms notify 传输延迟。

### 优化 B2：wechat 模式跳过 hold 阈值（新增 hold_to_talk_instant 模式）

固件不知道输出模式（wechat 是桌面端概念）。新增 `interaction_mode = "hold_to_talk_instant"`：按下即 `start_recording` + `button_down`，无 300ms 阈值；button_up 短按仍进双击窗口（双击发 Enter 保留）。

**桌面端下发策略**（关键简化）：桌面端 `InteractionMode` 枚举不变（仍 kHoldToTalk/kClickToTalk），仅在 wechat 模式 + hold_to_talk 时把下发字符串改为 `"hold_to_talk_instant"`。桌面端 `config_.interaction_mode` 仍是 kHoldToTalk，所有 `== kHoldToTalk`/`== kClickToTalk` 判断不受影响，只改 `SendInteractionMode` 两处调用点（coordinator.cc:79/170）的下发值。

**双击检测天然保留**：instant 模式 `s_hold_threshold_pending=false`，按下走 953 start_recording + 967 button_down；松开走 `handle_primary_up` 的 1043-1058，`stop_recording` 后短按判定进双击窗口（1049-1057 不依赖 hold_threshold_pending）。双击第二击在 880 行确认。无需改 button_up。

**旧固件兼容**：固件 `apply_interaction_mode`（main.c:715-720）只匹配 "click_to_talk"/"hold_to_talk"，旧固件收到 "hold_to_talk_instant" 不匹配则保持原模式（退化为 300ms 阈值），安全。device_info 的 `interaction_modes` 数组新增该值供桌面端协商（可选，旧桌面端不识别也安全）。

### 优化 C：SendDown 提前到 renderer.Start 之前

`StartWechatInputMethodSession`（coordinator.cc:511/519）调换顺序：先 `hotkey->SendDown()` 触发微信弹框，再 `renderer->Start()`。音频晚几十毫秒到达虚拟麦不影响识别（微信面板有缓冲）。失败处理：SendDown 后 renderer.Start 失败需补 `hotkey->SendUp()`（StopWechatInputMethodSession 已含 SendUp，复用）。

### 优化 D：全删提示音

固件 + Windows 端彻底清除提示音：
- **固件**（main.c）：删 `s_prompt_tone_enabled`、`play_prompt_tone`、4 处调用（530/564/944 + 724-727 协议解析）、`audio_pipeline_play_tone`（若仅提示音用）。`audio_pipeline_play_tone` 的实现保留与否视是否他处引用，确认后定。
- **Windows**：删 `prompt_tone_enabled` 配置字段（app_config.h:143/cc:357/517/608）、`BleProtocol::PromptTonePayload`（ble_protocol.cc:204/h:72）、`SendPromptToneEnabled`（ble_central_win.h:40/cc:379/coordinator.h:69）、两处调用（coordinator.cc:80/171）、设置 UI（settings_dialog.cc:485/659/742/h:60/95）、localization（localization.h:34/cc:41/233）、测试（core_tests.cc:81/130/507/879 + main 注册）。
- **macOS**：无 prompt_tone（grep 确认），零改动。
- **协议**：protocol.md 无 prompt_tone 字段（Windows 扩展），无需改文档。

## 改动范围

| 模块 | 文件 | 改动 |
|---|---|---|
| 固件 | main.c | A: 按下请求 fast interval；B2: 新增 instant 模式枚举+handle_primary_down 分支+apply_interaction_mode+device_info；D: 删提示音 |
| 固件 | audio_pipeline.c | D: 确认 play_tone 是否删除 |
| 固件 | voice_ble.c | B2: device_info interaction_modes 加 instant |
| Windows | voice_stick_coordinator.cc | A:无；B2: SendInteractionMode 两处下发覆盖；C: SendDown 提前；D: 删调用 |
| Windows | app_config/ble_protocol/ble_central_win/settings_dialog/localization | D: 删提示音配置/协议/UI |
| Windows | tests/core_tests.cc | D: 删提示音测试；C/B2: 新增测试 |
| 协议 | Doc/Ref/protocol.md | B2: interaction_mode 新增 hold_to_talk_instant 值 |

## TDD 测试（Windows 端）

1. `TestCoordinatorWechatModeSendsInstantInteractionMode`：wechat 模式 + hold_to_talk → `SendInteractionMode` 收到 "hold_to_talk_instant"；非 wechat 模式 → "hold_to_talk"。
2. `TestCoordinatorWechatSessionSendsHotkeyBeforeRendererStart`：C 顺序——SendDown 在 renderer.Start 之前；Start 失败补 SendUp。
3. 删除 `TestCoordinatorSyncsPromptToneOnConnectionAndConfigUpdate` 等提示音测试。

固件无单测，靠 `idf.py build` + 真机。

## 验证

1. 固件 `idf.py build` 编译通过。
2. Windows `build_win.bat` 构建 + `ctest` 全绿。
3. 真机回归：按下到弹框延迟主观对比；双击仍发 Enter；非 wechat 模式提示音已无、交互不受影响；conn interval 切换日志。

## 实施步骤

1. 本方案文档。
2. 固件改动（A+B2+D）。
3. Windows 改动（B2 下发覆盖 + C 顺序 + D 清除）+ 测试。
4. 协议文档更新。
5. 构建 + 测试 + 真机回归。
