# 微信输入法（WeType）语音触发排查定案——F5 按住状态阻塞（原「注入封死」结论有误）

- 日期：2026-09-11（当日两轮排查：第一轮结论错误，第二轮推翻定案）
- 背景：`wechat_input_method` 模式按小米遥控器语音键后，WeType 语音面板「没反应」，无识别无上屏；但 StickS3 走同一注入代码路径可正常触发。
- **定案结论：SendInput 注入完全能触发 WeType 语音（干净键盘状态下 100% 复现）。真实根因是小米遥控器语音键（F5 HID 键）按住期间，F5 的异步键状态（GetAsyncKeyState）让 WeType 拒绝启动语音会话。修复：SendDown/SendClick 前检测 F5 按住则先注入一发 F5 keyup 中和异步状态。**

## ⚠️ 第一轮错误结论与教训

第一轮曾定论「WeType 全链路校验键盘物理来源，注入无法触发」——**这是错的**，错误根源有三：

1. **实验工具自身坏了没发现**：PowerShell 注入脚本的 `INPUT` 结构体 padding 算错，`SendInput` 因 cbSize 不符**静默返回 0**（脚本未检查返回值），所谓「注入实验」根本没注入。C++ 工具那次则是前台窗口失控（始终=终端窗口，不是可输入目标）。
2. **把「当日所有注入时刻零 start_received」过度归因为「校验物理来源」**：当时小米会话全部被 F5 按住阻塞（见下），裸注入实验全部没真正注入/前台不对——两个独立变量都被当成了「WeType 拒绝注入」的证据。
3. **用户用 StickS3 实测推翻**：同一条 SendDown 代码路径 4/4 触发（gen37-40，注入后约 0.53s）。物理来源校验说不攻自破。

教训：**SendInput 实验必须检查返回值；「零触发」结论前必须先证明注入真的到了系统（旁观 LL 钩子/async 状态检查）；结论与用户实测定论冲突时优先怀疑自己的实验环境。**

## 真实机制（全链路证据）

| 场景 | F5 keydown 去向 | F5 async 状态 | WeType 触发 |
|---|---|---|---|
| StickS3（无 F5） | — | 干净 | ✓ 注入后约 0.53s（4/4）|
| 小米按住期间 | VoiceF5Suppressor 吞（keydown/keyup 配对全吞，不投递）| F5=down | ✗ 阻塞 |
| 小米松开后 0.57s | — | F5=up | ✓ 触发（gen41）|
| 注入 F5 down + Ctrl+Win | 投递（suppressor 对 INJECTED 放行）| F5=down | ✗ 阻塞（且注入 F5 up 后长时间不恢复——注入 F5 会在前台窗口产生真实按键副作用，如记事本 F5 插时间戳，可能毒化 WeType 状态）|
| 干净状态注入 Ctrl+Win（vk+scan+记事本前台）| — | 干净 | ✓ 约 1.2-1.4s（gen43/47）|

机制：LL 钩子吞键只挡「投递」（任何窗口/钩子链下游都看不到），**异步键状态在 RIT 层更新、不受吞键影响**——WeType 启动语音会话前检查「无其他普通键按住」（GetAsyncKeyState 全键扫描），F5 按住即拒绝。遥控器语音键松开瞬间语音也停了，VoiceStick 又在 button_up 后约 58ms 就 SendUp，WeType 约 0.53s 的启动去抖窗口不够——所以 08:32 那两次 0/2：既没在按住期间触发（F5 阻塞），也没在松开后触发（Ctrl+Win 也松了）。

## 修复（已落地）

`WechatInputMethodHotkey::SendDown/SendClick` 入口调用 `NeutralizeHeldF5()`：`GetAsyncKeyState(VK_F5)` 报按住（0x8000 位）则先注入一发 F5 keyup（带 scan code，走既有 BuildKeyboardInput）。物理 F5 keydown 已被 suppressor 吞掉（WeType 钩子无账），所以注入 keyup 只清异步状态、无死账；遥控器随后物理松开产生的 F5 keyup 落在被吞序列里，幂等无害。StickS3 场景 F5 从不按住，行为不变。TDD：`TestWechatHotkeySendDownNeutralizesHeldF5`（GetAsyncKeyState 测试缝伪造按住/未按住两场景）。

## 关键诊断手法（复用价值）

1. **WeType 自带语音诊断日志**：`%TEMP%\WeTypeVoiceDiagnostic_<pid>.log`，`start_received` 精确判定会话启动。**注意日志由前台焦点进程（TSF 宿主，如记事本）写入**——pid 对应的是当时的前台应用，不是 WeType 自己的进程；grep 必须全量扫所有文件。
2. **触发延迟是 WeType 侧固定去抖**：物理/注入均约 0.53-1.4s，「SendDown 后立刻没 start_received」不代表失败。
3. **WeType 单会话占用**：上一个语音会话挂着（等音频）时后续触发被拒；连续实验要间隔或确认会话已结束。
4. **旁观 LL 钩子 + SendInput 返回值 + GetAsyncKeyState 回读**三件套先自证「注入真的到了系统」，再归因上层。
5. **AI 视觉分析截图只能作线索不能作证据**（本轮及第一轮多次与客观日志矛盾）。
6. **窗口枚举（EnumWindows/C# 委托）**比截图可靠；PS 脚本块不能直接当 EnumWindows 回调指针，必须 C# 委托。
7. **PowerShell P/Invoke 的 INPUT 结构**：必须用显式 union LayoutKind.Explicit 且打印 `Marshal.SizeOf` 核对（x64 应为 40），SendInput 返回值必须检查。

## 注入实验最小成功配方（2026-09-11 实证）

- 前台必须是可输入窗口（记事本）：ALT tap（keybd_event VK_MENU down/up）解前台锁 + SetForegroundWindow + 回读前台标题验证。
- 事件带 `wVk + MapVirtualKey(vk, MAPVK_VK_TO_VSC)` 扫描码（LWin/RAlt 等注意扩展键标志）。
- 长按语义：40ms 周期重复注入 keydown，松开时 keyup。
- 键盘状态自检/清理：`GetAsyncKeyState` 逐键回读；实验残留的按住状态（脚本半途崩溃漏发 keyup）会让后续一切实验假阴性。

## 关联

- 40ms 重复注入（7a444c7d）保留且必要：WeType 长按检测依赖持续 keydown 流。
- F5 中和修复：见 `wechat_input_method_hotkey.cc` 的 `NeutralizeHeldF5`（当日提交）。
- 第一轮死锁修复（f4480928）真实有效：解决了 SendDown 发不出的 UI 线程卡死。
- VoiceF5Suppressor 吞键与异步键状态的关系是本案例核心知识点：**吞键 ≠ 状态不可见**。
