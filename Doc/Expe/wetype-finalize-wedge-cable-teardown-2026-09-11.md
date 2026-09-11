# WeType 语音面板弹出后不消失：keyup 后拆除 CABLE 管道卡死 TSF 宿主（方案 A 修订）

- 日期：2026-09-11（方案 A 见 `Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md`，同系列前置排查见 `Doc/Expe/wechat-click-toggle-acceptance-defects-2026-09-11.md` 与 `Doc/Expe/wetype-voice-injection-blocked-2026-09-11.md`）
- 相关文件：`desktop/windows/src/voice_stick_coordinator.cc`、`wasapi_mic_capture.cc`、`mic_capture.h`、`desktop/windows/tests/core_tests.cc`
- 相关 commit：本文随修复提交「fix(微信输入法): 方案 A 直连默认麦克风——…」一同落地（`git log -- Doc/Expe/wetype-finalize-wedge-cable-teardown-2026-09-11.md` 定位）；d4fbfd13/24418ade（被本修订取代/修正的前两次尝试）

## 症状

用户报告：小米遥控器录音键无法激活第三方输入法；具体现象为「WeType 语音面板弹出后不消失、卡住」。真机两次完整 click-toggle 循环（21:51 gen88、22:13 gen89）均复现：面板弹出、识别文本实时进 composition，但停止击后面板不退、文字不上屏；之后 WeType 对该应用不再响应任何新的语音会话（表现为"无法激活"）。

## 日志判据

- VoiceStick 侧按键链路全程正常：`button_click → auto_switch/renderer.Start → SendDown end (click toggle, immediate) → wechat local mic session started → 第二击 SendUp end → state ready`，无任何错误——**问题不在 VoiceStick 侧事件流**。
- WeType 侧（`%TEMP%\WeTypeVoiceDiagnostic_*.log`，语音服务与 TSF 宿主两进程联合读）：
  - 语音服务进程：`VoiceCompositionTrace start generation=N` 正常，有语音时 `emit utf8_len=...` 持续到达。
  - **TSF 宿主（pid = 当前焦点应用进程，本例 ZCode.exe）**：`start_received` 之后**永无 `terminated composition` / `host_termination`**；宿主日志在 keyup 后 ~9.6s 停笔，之后新会话（gen89）连 `start_received` 都不再出现 = TSF 侧永久卡死。
  - 卡死窗口内 `detach_request reason=mouse_down mode=detaching`（用户点鼠标试图关面板）也永远停在 detaching。
- 对照：物理按住测试（gen77）同场景干净收尾（`terminated composition` + `host_termination`）；旧停止顺序（先停音频再 keyup，gen84）至少 5s `abort reason=composition_commit_timeout` + 8s 收尾——**新顺序反而更糟**（永不收尾）。

## 根因

1. WeType 的 finalize/commit 依赖 keyup 时音频流存活（物理松开时麦克风永远在供电，房间底噪持续）。方案 A 的停止顺序「SendUp → 600ms 宽限 → 停本机麦采集 → 停 CABLE 渲染 → 默认录音设备切回真实麦」在 WeType finalize 进行到一半时拆除了整条音频管道：VB-Cable 采集侧随渲染侧关闭而停滞 + 默认设备中途切换，把 TSF 宿主的 commit 流程卡死（无 abort、无 termination、宿主日志停笔）。
2. 更深一层：方案 A 的 CABLE 绕行（本机麦 → ring buffer → CABLE → WeType）本来就是多余的——WeType 只读「默认录音设备」，而方案 A 的音频源头就是默认设备（真实麦克风）本身（采集器还专门钉住它，等于把默认设备的音频绕一圈送回默认设备）。绕行唯一引入的是 auto_switch 设备切换往返、VB-Cable 停滞面、以及本 bug 的拆除时序炸弹。物理同构测试（gen77：WeType 直接采默认设备）干净收尾，是唯一可靠锚点。

## 修复

方案 A 修订（直连默认麦克风）：小米 + click_to_talk + session_model=hold 组合下，会话**不再启动本机麦采集、不再 auto_switch、不再渲染 CABLE**——点击启动即 SendDown，WeType 弹框后直接采集默认录音设备（真实麦克风）；停止只 SendUp，无任何拆除动作。StickS3 等 BLE 音频路径保持 CABLE 管道不变（`wechat_session_direct_mic_` 门控 Start/Stop 两段）。连带删除已孤儿化的钉扎链路（`SetPreferredEndpointId` 全链——d4fbfd13 的修复在直连模式下失去存在意义）。

## 验证

- 单测改写：直连模式断言零采集/零渲染/零设备切换（红→绿）；停止顺序回归用例改挂 StickS3 BLE 管道（keyup 早于 renderer 停止）；删除采集启动失败回滚用例（直连模式无采集可失败）。
- CTest 2/2 全绿（voicestick_windows_tests + voicestick_integration_tests）。
- 真机：待用户重启 ZCode（TSF 宿主卡死在 ZCode 进程内，最快解除方式是重启该应用；其他应用如记事本的实例不受影响）后执行 click → 说话 → click → 文字上屏 + 面板消失。（结果回填）

## 长期技术记忆

- 「WeType 语音会话」健康判据 = TSF 宿主日志出现 `terminated composition`/`host_termination`，或语音服务日志出现 `abort reason=`；**两进程日志同时停笔 = 宿主卡死**，之后新会话连 `start_received` 都不会有。
- TSF 宿主的 pid 不是输入法进程，而是**当前焦点应用**（WeType 的 TSF DLL 注入宿主应用）；宿主卡死只影响该应用，换一个宿主应用（记事本）即可隔离验证。
- 给第三方语音输入法供音，优先让它直接采真实设备（物理同构）；虚拟麦 + CABLE 绕行只在音频源头本身不是 Windows 设备（BLE 流）时才必要。
- 「会话中拆除」比「会话前拆除」更危险：gen84（keyup 前全拆）尚能 5s abort 自愈，gen88/89（keyup 后 600ms 拆）直接永久卡死——对第三方中间态容忍度未知时，物理语义是唯一可靠锚点。

## 遗留/观察项

- StickS3 + wechat（hold_to_talk，BLE 音频经 CABLE）仍是同一停止顺序（600ms 宽限后拆管道），理论上存在同类卡死风险；本次问题范围是小米方案 A，StickS3 路径未动，若真机出现同样现象再评估异步延迟拆除。
- `wechat_stop_audio_grace_` 600ms 是经验值，仅服务 CABLE 管道模式；直连模式跳过宽限。
- ZCode 进程内的 WeType TSF 卡死需重启 ZCode 解除；重启 VoiceStick 是否连带恢复待真机确认（TSF 宿主随宿主进程生死，理论上是）。
