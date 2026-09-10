# macOS 拦截小米遥控器按键：IOHID 独占 seize 被拒（NotPermitted），须用非独占观察 + CGEvent tap 关联

- 日期：2026-09-03
- 相关文件：`scripts/ref/hid_seize_probe.swift`（spike 探针）、`Doc/Plan/xiaomi-remote-button-mapping.md`（方案 §4.2）
- 相关 commit：`5d54526`（按键自定义一期）
- 时效性：探针使用的 IOKit 返回码与 macOS 系统行为为记录时点结论，引用前以当前系统与源码为准。

## 症状

给小米蓝牙遥控器 2 Pro（VID `0x2717`/PID `0x32B8`，BLE transport）做「逐键自定义」，需要桌面端拿到该遥控器全部按键事件。现状是除语音键（走 ATVV）外，其余 11 键走标准 HID over GATT（service `0x1812`）被 macOS 原生消费，app 不可见。方案 A 拟用 `IOHIDManager` + `kIOHIDOptionsTypeSeizeDevice` 独占抓取该 HID 设备（Karabiner/BetterTouchTool 同源思路），独占后 OS 不再消费其报告，app 收全量报告自行分派。

## 日志判据

用 `scripts/ref/hid_seize_probe.swift` 在已配对真机上实测（遥控器已连接本机，probe 枚举输出 `小米蓝牙语音遥控器 transport=Bluetooth Low Energy vid=0x2717 pid=0x32b8`）：

```text
IOHIDManagerOpen: success
device: 小米蓝牙语音遥控器 transport=Bluetooth Low Energy vid=0x2717 pid=0x32b8
  SEIZE FAILED: 0xe00002c1 — fallback = non-exclusive observe + CGEventTap correlation
```

`0xe00002c1` = `kIOReturnNotPermitted`（`IOReturn` 体系）。

## 根因

macOS 对暴露为「键盘类」的 HID 设备（usage page 键盘/消费控制）实施受控访问：`IOHIDDeviceOpen` 请求 `kIOHIDOptionsTypeSeizeDevice` 独占时会因未获「输入监控」/系统授权被拒（`kIOReturnNotPermitted`），即使设备已通过 BLE 配对本机。这是 TCC/IOKit 层对键盘类外设的默认保护，与 Karabiner 早年依赖内核扩展（kext/DriverKit 虚拟 HID 驱动）绕过的原因一致——纯用户态 `IOHIDDeviceOpen(seize)` 对键盘类蓝牙 HID 设备不可行。（注：本 spike 用 `swift` 解释器运行，进程归属宿主终端 TCC；即便在正式 .app 内，同属键盘类受控访问，结论一致，但授权归属会变。）

## 方案（机制 + 常量）

否决方案 A。macOS 二期采用与 Windows 同构的**非独占观察 + 事件 tap 时序关联**：

- 非独占 `IOHIDManager`（`kIOHIDOptionsTypeNone`，VID `0x2717` 匹配）observe 遥控器报告 → 带设备归因的按键事件（键盘类需「输入监控」权限）。
- 复用现有 CGEvent HID tap（F5 抑制 `XiaomiF5Suppressor.swift`，需辅助功能权限）负责抑制；tap 事件与 IOHID 观察事件按 80ms 邻近窗 + 时序 latch 关联判定来源（与现有 F5 抑制、Windows WH_KEYBOARD_LL 同款启发式）。
- 命中自定义（`[device.<id>.buttons]` 的 `intercept=true` 且该键 action=key/disabled）→ 抑制原键 + CGEvent 注入目标动作（KeySpec，`InputInjector.sendKeyCombo`）；`native` 或 `intercept=false` → 放行（OS 原生消费，不动）。

热点/usage 映射表（实测键码，键名与方案锁定集合一致）：

| usage | 键 | | usage | 键 |
|---|---|---|---|---|
| `0x28` | ok（Enter） | | `0x65` | menu |
| `0x4A` | home | | `0x66` | power（不拦截） |
| `0xF1` | back（非标准） | | `0x7F` | 静音（预留） |
| `0x4F`/`0x50` | right/left | | `0x80`/`0x81` | vol_up/vol_down |
| `0x51`/`0x52` | down/up | | `0x35` | tv |

## 验证

- spike 探针已验证三件事：①设备可被 `IOHIDManager` 枚举；②`IOHIDDeviceOpen(seize)` 确定性返回 `0xe00002c1`；③非独占观察的报告流需人工按遥控器按键复测（probe 已支持打印 report，授权输入监控后按下任一键应见 report 行）。
- 二期拦截层**未实现**（见「遗留」）。本条目首次验证至「zseize 被拒、改走关联式归因」为止，关联式归因的实际匹配率仍需真机回归（方向键连发、音量连按、与真键盘并发敲击）。

## 长期技术记忆 / 经验

- macOS 上想**独占**（seize/独占读）一个**键盘/消费控制类**蓝牙 HID 设备：纯用户态 `IOHIDDeviceOpen` + `kIOHIDOptionsTypeSeizeDevice` 会被 `kIOReturnNotPermitted` 拒绝。要「拿设备 + 拦按键」只能：①非独占观察拿归因 + OS 级事件 tap（CGEventTap，需辅助功能）做抑制/注入，启发式关联来源；或 ②内核扩展/DriverKit 虚拟 HID 驱动（Karabiner 路线，工程量与签名成本高，本仓库红线外）。
- 「按设备归因」是拦截多键遥控器与真键盘并存时的核心难点；纯 CGEvent tap 无法可靠识别来源设备，必须靠 IOHID/Raw Input 观察层拿到句柄/设备对象再与 tap 事件时序关联。
- 先小步 spike 验证平台能力再铺方案，避免在不可行前提上投入整期工程量——本例 spike 成本 <10 分钟，挽救了方案 A 整个二期的方向性风险。

## 遗留 / 观察项

- 二期双端 HID 拦截层尚未交付：本机 provider token 计划配额耗尽（HTTP 500），子代理无法启动，未能连夜完成。Windows 侧一期亦未编译验证（本机无 MSVC）。均列入明早人工协助/验收项。
- 一期已交付并提交（`5d54526`）：`[device.<id>.buttons]` 配置 schema、语音键双击动作可配（替换硬编码 Enter）、双端罗技式按键映射 UI（macOS `ButtonMappingWindowController` / Windows `button_mapping_dialog`）、实拍图光照均衡底图 + 归一化热点表。macOS `swift build` 通过、`VoiceStickTests` 420/420 全绿；Windows 未编译验证。
- 关联旧文：`Doc/Expe/xiaomi-remote-macos-port-p5-2026-09-02.md`（macOS 端口 BLE/TCC 相关）、`Doc/Expe/xiaomi-remote-2pro-capture-and-f5-lessons-2026-09-02.md`（F5 抑制与捕获）、方案 `Doc/Plan/xiaomi-remote-button-mapping.md`。

## 追加：真机首测「返回键→Delete 无反应」排障（2026-09-04）

二期/三期交付后首次真机验证，用户报告：已授权并重启，返回键设 Delete 保存后按键无任何效果。一夜排障推翻本文上文两处结论，修正如下。

### 症状与判据

- 拦截层门控全部通过（`available=1 connected=[78FA] intercept=1`，IOHID 观察与 tap 均安装），但按返回/音量/方向/主页/菜单键时：IOHID value 回调、IOHID report 回调、CGEvent tap（含 systemDefined 探针）**三层全部零事件**；只有语音键（0x3E/F5）与确认键（0x28/Enter）有报告。
- 判据：按音量键时连**系统音量 OSD 都不变**（`osascript -e 'output volume of (get volume settings)'` 前后同值）——说明不是 app 链路问题，是系统层根本没收到报告。

### 根因（两个，叠加）

1. **macOS 配对缓存损坏**：蓝牙设置里「忽略此设备」→ 遥控器主页+菜单 3-5s 重新配对后，**全部 11 键报告立即恢复**（非独占观察即可收到，seize 根本不需要）。此前「固件配对 Mac 时只发 F5/Enter」的推断是配对缓存异常造成的假象——协议文档「所有键走 Report ID 1」与 Windows 实现一直是对的。报告描述符也证实：全键以 keyboard page usage 装进 Report 1（3×16 位 LE usage 槽），无 consumer collection；Windows 侧的「consumer page」解读来自其 Raw Input 对同报告的另一归卷方式。
2. **处置挂错层**：旧架构把 inject/suppress 决策挂在 tap 的 keyDown 回调里（靠「IOHID 锚点 + 80ms 关联窗」触发），但 0xF1(back)/0x65(menu) 等键 macOS **不产生 keyDown CGEvent**（HID 翻译层丢弃），tap 永远看不到 → 处置永远不触发。这正是「返回键设 Delete 无反应」的直接原因（重配对后依然不会生效）。

### 修复（机制 + commit）

`XiaomiButtonInterceptManager` 重构为「IOHID 观察驱动处置 + tap 只吞除原生事件」（详见方案 §4.2 修订版）：

- IOHID value 回调只认 variable 元素（usage 精确，val=1 按下）+ **page==0x07 过滤**（vendor page 0xFF00 的 OTA/ATVV 120 字节数据流走同一设备，不过滤会把数据字节误判为按键）。
- inject：IOHID 按下即 `sendKeyCombo`；若该键有原生行为，记一次性吞除标记（300ms 窗兜底），tap 按 `RemoteButtonHIDMap.usageToMacKeyCode`（方向/OK/Home/TV→keycode）与 `volumeSystemKeyToUsage`（音量→systemDefined subtype=8，data1=(keyCode<<16)|0xa00 down/0xb00 up）反映射吞掉原生事件防双触发；keyDown 吞后闩锁联动吞自动重复与 keyup，音量 down/up 闩锁配对。
- seize 否决结论**被加固**：app 内（含 device matching 回调抢占时机）`IOHIDDeviceOpen(seize)` 仍恒 0xe00002c1——已连接设备被系统 HID 栈持有即不可 seize，与权限无关。
- core 新增 `usageToMacKeyCode`/`volumeSystemKeyToUsage` 表 + 单测；`swift build` 通过，`VoiceStickTests` 508/508。

### 验证程度（如实记录）

- **已真机验证**：重配对后全键报告到达 IOHID 层（日志逐项比对 usage）；音量键 native 透传调系统音量生效。
- **待真机复测**：重构后 back→Delete 注入、方向键/音量键自定义时的原生事件吞除（防双触发）、TV 键 0x35→grave(0x32) 的 keycode 映射正确性。排障收尾时遥控器休眠未回连（`central.connect` 挂起等广播），注入链路未跑完最后一棒。

### 长期技术记忆 / 经验

- **「按键报告不到 IOHID 层」先重配对**：macOS 蓝牙 HID 配对缓存异常会让设备只剩部分键报告（本例只剩 F5/Enter），「忽略此设备」+ 重新配对是最便宜的判别/修复动作；判据是「连系统原生行为（音量 OSD）都没有」。
- **macOS 上不是所有键盘页 usage 都会产生 keyDown**：0xF1(LANG2)/0x65(Application) 被 HID 翻译层丢弃，0x80/0x81 走 systemDefined（subtype=8）。拦截/注入类功能的事件源必须挂在 IOHID 层，不能挂在 CGEvent tap 层。
- **TCC 按代码签名身份记账，ad-hoc 签名 = 每次构建换身份**：`codesign -s -` 的 designated requirement 锚定 cdhash，重建二进制即失效，系统设置里的勾选成为僵尸记录（unified log 可见 `kTCCServiceListenEvent auth_value=0`）。注入类验收（输入监控/辅助功能）重建后必须重新授权；持久解法是自签证书固定身份（参考 open-voice-bridge 的 "Local Code Signing" 实践）。**从终端直接执行 .app 二进制调试时 TCC 归属终端身份**（preflight 结果与 `open` 启动不一致），权限相关结论必须以 `open` 启动形态复核。
- 排查工具箱：`log stream --predicate 'process == "VoiceStickApp"' --info` 可抓到 TCC 授权判定明细（auth_value/auth_reason），比系统设置 UI 的勾选状态更接近真相。

### 遗留 / 观察项（2026-09-04）

- 自签证书已生成导入登录钥匙串（`~/.voicestick-sign/`，CN="VoiceStick Local Code Signing"），但私钥访问需用户在 SecurityAgent 弹窗点「始终允许」（两次命令行尝试均因弹窗未处理超时）。完成后把 `scripts/build-macos.sh` 签名优先级改为 Developer ID → 自签 → ad-hoc，TCC 权限跨构建才稳。
- 遥控器休眠后 app 的 `central.connect` 重连依赖按键唤醒广播；若长时间不回连，检查遥控器电量与蓝牙设置里的连接状态。
- Windows 端二期/三期代码仍未编译验证（本机无 MSVC），沿用旧结论。

## 追加：交付纪要（三期全部落地，2026-09-03 夜间更新）

一期（`5d54526`）、二期（`6a5427c`）已提交推送；三期「按前台应用切换映射」双端落地后随本纪要提交。最终状态：

- **macOS**：`swift build` 通过；`VoiceStickTests` **494/494** 全绿（一期 420 + 二期 usage/decision + 三期 app 合并单测）。已实现：配置 schema、语音键双击可配、罗技式映射窗、非独占 IOHID 观察 + CGEvent tap 关联拦截、前台应用切换映射（`NSWorkspace.didActivate` → `FrontmostAppProvider`）。
- **Windows**：全部功能代码落地但**未编译验证**（本机无 MSVC），靠模式模仿 + 逐块自查；含 Raw Input + `WH_KEYBOARD_LL` 关联拦截、`EVENT_SYSTEM_FOREGROUND` 前台映射。落地前须在 Windows 机 `build_win.bat` + `ctest --test-dir desktop\windows\build-x64 --output-on-failure`。
- **共同待真机验证**（人手按遥控器按钮）：①非独占 IOHID/Raw Input 能否稳定收到该设备报告；②逐键 usage page 归属与实际上报值；③tap/hook 与观察事件的 80ms 时序关联在方向键连发、音量连按、与真键盘并发下的错配率；④拦截开启后未自定义键 1:1 透传与 app 退出后回归原生；⑤多台同型 RC 的按句柄/设备独立映射（现取首台配对 RC 配置）。
