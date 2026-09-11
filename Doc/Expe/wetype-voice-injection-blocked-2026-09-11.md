# 微信输入法（WeType）语音触发排查定案——静默期毒化 + 用户态 HID 拦截不可达（三轮终版）

- 日期：2026-09-11 ~ 09-12（三轮排查，前两轮结论先后被推翻）
- 背景：`wechat_input_method` 模式按小米遥控器语音键后，WeType 语音面板「没反应」，无识别无上屏；但 StickS3 走同一注入代码路径可正常触发。
- **终版定案：WeType 启动语音会话需要 ①快捷键按住 + ②普通键活动静默 ≥约 0.5~1.5s（活动时间戳判定，防打字误触）。小米语音键是 F5 HID 键，物理按住期间以 ~30ms 间隔持续产生真实按键活动——LL 吞键只挡投递，异步/更底层的活动时间戳照样刷新，静默条件永不满足。这是物理性冲突，注入侧（包括 F5 keyup 中和）无解。HID report 层拦截的 PoC 同时定案：report 流不经过 WUDFHost 用户态任何 IO 系统调用，用户态改写不可达；要改写必须内核驱动（签名/维护成本高）。**

## 三轮结论演进（每轮都推翻了上一轮）

| 轮次 | 结论 | 命运 |
|---|---|---|
| 第一轮（09-11 晨） | 「WeType 校验物理来源，注入被封死」 | ❌ 实验工具自身坏（INPUT 结构 padding 错，SendInput 静默返回 0），被用户 StickS3 实测推翻（4/4 触发）|
| 第二轮（09-11 午） | 「F5 异步按住状态阻塞，注入 F5 keyup 可中和」→ 落地 f20dcb72 `NeutralizeHeldF5` | ❌ 用户实测仍不触发；注入 up 本身也是活动，且毒化源是「活动时间戳」不是「按住状态」 |
| 第三轮（09-11 晚~09-12） | 「静默期毒化（物理性）+ 用户态 HID 拦截不可达」 | ✅ 证据链闭合（见下）|

## 最终机制模型

- **触发条件**：快捷键按住（事件流）+ 普通键活动静默 ≥T_s（约 0.5~1.5s）；触发有约 0.53~1.4s 的内部去抖；单会话占用。
- **小米为何永不触发**：语音键 = F5 HID 键，按住期间遥控器以 ~30ms 持续发 report，每个 report 都是一次真实按键活动。VoiceF5Suppressor 在 LL 层全吞（不投递），但 WeType 透过更底层（RIT/async）感知到活动时间戳持续刷新 → 静默期永不满足。**hold 按住模式下这是物理性冲突。**
- **StickS3/注入为何 100% 触发**：注入配方 = F5 down + 40ms repeat keydown。注入的 repeat 流不毒化静默期（实测 4/4 触发；推断 WeType 的活动判定与键来源/间隔模式相关，机制未深究，以实验为准）。
- **`NeutralizeHeldF5`（f20dcb72）无效**：注入 F5 keyup 只清「按住位」，清不掉「活动时间戳」——且注入本身也是一次活动。该修复待回滚或标记无效。

## PoC 定案：WUDFHost 用户态拦截 HID report 不可行（2026-09-12）

参照 MiVibe-Remote 的 Frida Gadget 注入方案（DLL 注入 HidOverGatt 的 WUDFHost，JS hook `ntdll!NtDeviceIoControlFile`），实证链：

1. **注入链全通**：自提升注入器（SeDebugPrivilege + CreateRemoteThread+LoadLibraryW）→ Gadget interaction script 模式加载 JS → hook 挂载、心跳存活。
2. **report 格式确认**：9 字节 `01 00 00 <usage> 00 00 00 00 00`，字节 3 = 键 usage（F5=0x3E、方向右=0x4F），松开=全零。
3. **IOCTL 0x80018483（BTHLE GATT ReadCharacteristic）是事后缓存读，不是数据正主**：onLeave 改写 `3e→00` 写回成功，但 20ms 后 LL 钩子仍见 F5 keydown，WeType 仍不触发。
4. **全码表观察（决定性）**：按住语音键 3 秒（~100 个重复 report 全部进系统，LL 钩子持续可见）期间，WUDFHost 用户态只发生 **2 次** `NtDeviceIoControlFile`（都是 0x80018483：按下/松开各一次状态变化驱动的缓存读）。`NtDeviceIoControlFileEx` 在 ntdll 无导出，通道不存在。
5. **推论**：bthleenum.sys 收到 GATT notification 后在**内核内**直接把 report 提交给 hidclass，WUDFHost 里的 BthHidEnum 用户态组件只做枚举/配置/偶发状态读——**report 流不经过用户态任何 IO 系统调用，用户态 hook（无论钩哪个 API）都改不了**。MiVibe 的 Gadget tap 只做只读观察（读按键状态变化）而非改写，正是受此架构约束；他们真正的按键拦截同样在 LL 层。

PoC 环境残留：`C:\ProgramData\VoiceStickHidPoC\`（vs_hid_poc.js v4、注入器、日志；WUDFHost 重启即清注入，无持久影响）。

## 可行路线盘点（截至定案日）

- **方案 A（推荐，纯软件低成本）**：语音键改 click 语义——物理流结束后（静默期开始计时）注入已验证的「F5 down + 40ms repeat」配方，再点一下注入 F5 up 结束。代价：失去原生「按住说话」，触发延迟约 1~2s。
- **内核过滤驱动**：理论上可在 hidclass 层改写 report、保住原生体验，但需驱动签名（WHQL/EV）+ 管理员安装 + 高维护成本 + 杀软误报，不适合当前阶段。
- **物理按住期间叠加注入**：不可行——物理 30ms 活动流继续毒化静默期，注入什么都没用。

## 关键诊断手法（复用价值）

1. **WeType 自带语音诊断日志**：`%TEMP%\WeTypeVoiceDiagnostic_<pid>.log`，`start_received` 精确判定会话启动。**注意日志由前台焦点进程（TSF 宿主，如记事本）写入**——pid 对应的是当时的前台应用，不是 WeType 自己的进程；grep 必须全量扫所有文件。
2. **触发延迟是 WeType 侧固定去抖**：物理/注入均约 0.53-1.4s，「SendDown 后立刻没 start_received」不代表失败。
3. **WeType 单会话占用**：上一个语音会话挂着（等音频）时后续触发被拒；连续实验要间隔或确认会话已结束。
4. **旁观 LL 钩子 + SendInput 返回值 + GetAsyncKeyState 回读**三件套先自证「注入真的到了系统」，再归因上层。
5. **AI 视觉分析截图只能作线索不能作证据**（多次与客观日志矛盾）。
6. **窗口枚举（EnumWindows/C# 委托）**比截图可靠；PS 脚本块不能直接当 EnumWindows 回调指针，必须 C# 委托。
7. **PowerShell P/Invoke 的 INPUT 结构**：必须用显式 union LayoutKind.Explicit 且打印 `Marshal.SizeOf` 核对（x64 应为 40），SendInput 返回值必须检查。**同款坑一轮排查内踩了两次**——中间结论（「X 键毒化」「1.5s 不够」）全部作废重来。
8. **WUDFHost 定位与重置**：注册表 `HKLM\SYSTEM\CurrentControlSet\Enum\BTHLEDevice\{00001812-...}_Dev_VID&012717_PID&32b8_REV&00a4_<addr>\...\Device Parameters\WUDFDiagnosticInfo` 的 `HostPid`；杀宿主需提权（taskkill），重启后新 pid、注入清除。同路径 DLL 重注入只加引用计数不重跑 JS；同进程二次注入不同 Gadget 实例会初始化失败。
9. **观察异步 IO 的坑**：`NtDeviceIoControlFile` 返回 STATUS_PENDING(0x103) 时 buffer 尚无数据；hook 挂载前已 pending 的 IO 完成时 onEnter/onLeave 均不触发（Frida 只见 hook 后发起的调用）。码表计数（onEnter 无条件）比 dump 过滤（retval==0）更能反映真实调用数。
10. **JS（QuickJS）Object.keys 键是字符串**：`.toString(16)` 不做数值转换，须 `parseInt(code).toString(16)`（本 PoC 心跳曾把 0x80018483 打成十进制 2147583107）。

## 注入实验最小成功配方（2026-09-11 实证）

- 前台必须是可输入窗口（记事本）：ALT tap（keybd_event VK_MENU down/up）解前台锁 + SetForegroundWindow + 回读前台标题验证。
- 事件带 `wVk + MapVirtualKey(vk, MAPVK_VK_TO_VSC)` 扫描码（LWin/RAlt 等注意扩展键标志）。
- 长按语义：40ms 周期重复注入 keydown，松开时 keyup。
- 键盘状态自检/清理：`GetAsyncKeyState` 逐键回读；实验残留的按住状态（脚本半途崩溃漏发 keyup）会让后续一切实验假阴性。

## 关联

- 40ms 重复注入（7a444c7d）保留且必要：WeType 长按检测依赖持续 keydown 流。
- `NeutralizeHeldF5`（f20dcb72）已被证伪，待回滚/标记；其测试 `TestWechatHotkeySendDownNeutralizesHeldF5` 一并处理。
- 第一轮死锁修复（f4480928）真实有效：解决了 SendDown 发不出的 UI 线程卡死。
- VoiceF5Suppressor 吞键与异步键状态的关系是本案例核心知识点：**吞键 ≠ 状态不可见 ≠ 活动时间戳不刷新**。
- MiVibe-Remote 参考实现：`platforms/windows/source/bridges/xiaomi/hid_tap_runtime.py`（Gadget 运行时）、`hid_tap_injector.py`（注入器蓝本）。
