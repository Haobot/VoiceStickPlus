# macOS 小米遥控器电源键自定义：systemDefined 双形态事件与 Apple 头文件查证路径

日期：2026-10-19（真机验收通过日）
相关文件：`desktop/macos/Sources/VoiceStickCore/VoiceStickButtons.swift`、`RemoteButtonHID.swift`、`desktop/macos/Sources/VoiceStickApp/XiaomiButtonInterceptManager.swift`、`ButtonMappingWindowController.swift`、`desktop/macos/Sources/VoiceStickCore/GatewaySupport.swift`
相关 commit：`3401845`（feat 本体）、本文档（验收沉淀）

## 背景与问题

Windows 端小米蓝牙遥控器 2 Pro 的电源键自按键映射功能合入起就在
`kXiaomiMappableButtons` 12 键全集内（`[xiaomi.keys]` 可配任意 key_spec，LL 钩子
吞原生 VK_SLEEP 后 SendInput 注入）。macOS 端同期按键分级定案却把电源键定为
**C 级"不建议开放"**（`Doc/Plan/xiaomi-remote-button-mapping.md` §3，理由"OS 电源
语义有副作用"），`RemoteButton` 无 `power` case、UI 热区置灰。本次按"双端能力
对齐"原则撤销定案、补齐 macOS 侧，真机验收通过。

## 关键技术查证（本文核心经验）

### 1. macOS 上键盘 power 键的 CGEvent 形态：systemDefined 双形态，不是 keyDown

改协议/拦截层前必须先回答：macOS 把 HID usage 0x66 翻译成什么事件、tap 能否
看到。查证路径与结论：

- **本机 CLT SDK 无 IOKit hidsystem 头文件**（CLT-only 机器，`IOKit.framework/Headers`
  下没有 `hidsystem/`，`grep NX_POWER_KEY` 全 SDK 为空）——不要在这里卡住，
  换外部权威源。
- **Apple SDK dump 仓库实证常量**（`phracker/MacOSX-SDKs`，
  `IOKit.framework/Versions/A/Headers/hidsystem/ev_keymap.h` + `IOLLEvent.h`，
  这些是几十年稳定的 NeXT 时代常量）：
  - `NX_POWER_KEY = 6`，且**列在 `NX_SPECIALKEY_POST_MASK` 里**——即像音量键
    （`NX_KEYTYPE_SOUND_UP/DOWN = 0/1`）一样经 special-key 通道 post 成
    `NX_SYSDEFINED`（kCGEventOther=14）事件，**不产生 keyDown**（`usageToMacKeyCode`
    不能给 power 加映射）；
  - `NX_SUBTYPE_AUX_CONTROL_BUTTONS = 8`（音量键已在用的 aux-control 通道），
    data1=(keyCode<<16)|0xa00(down)/0xb00(up)；
  - `NX_SUBTYPE_POWER_KEY = 1`——**另一种无 keyCode/状态语义的首事件形态**。
- **开源实现交叉佐证**（pkamb/PowerKey `PKPowerKeyEventListener.m`，多年拦截
  电源键的实证代码）：按一次电源键要吞**两类**事件——`subtype==NX_SUBTYPE_POWER_KEY(1)`
  的首事件，以及 `subtype==8 && specialKeyCode==NX_POWER_KEY && keyState∈{down,up}`
  的后续事件。即"单次按下 = 多形态事件序列"。

### 2. 吞除机制选型：多形态事件必须"窗口期"而非"一次性消费"

既有拦截层的吞除标记是**一次性消费**（`pendingSuppressByUsage`：IOHID 按下时
打标，tap 侧命中即清）——对"一按一事件"的键（keyDown 序列、音量 systemDefined
down/up 闩锁配对）正确。但电源键单次按下产生首事件 + down/up 对多个形态：
若首事件把标记消费掉，后续 aux-control 事件就会漏吞直通到 OS → 电源对话框/
睡眠照弹。修复（`XiaomiButtonInterceptManager`）：

- power 单独走 **`powerSuppressUntilMs` 非消耗吞除窗**（300ms，窗内所有电源
  形态一律吞，不消费窗口）；
- aux-control down 入闩 / up 出闩（闩锁与音量共享 `latchedAuxKeyUsage`，
  长按超窗后抬起仍配对吞除）；
- subtype=1 首事件窗内整体吞（无 keyCode 语义可细分）。
- 归属判定仍免费：窗只在 IOHID 观察到**遥控器 VID 0x2717** 的 0x66 按下沿才
  打开，真键盘/MacBook 电源键只在 300ms 重叠的极端巧合下被误吞。

### 3. 不可合成的原生语义

`GatewaySupport.naturalSpec(.power)` 返回 nil 是**刻意**的：网关上 macOS 没有
OS 级 HID 会话，其他键的"原生"由桌面注入等价自然键实现，但系统电源语义没有
也不应有 CGEvent 合成路径。路由对齐 Windows `PushGatewayKeymapRoutesFor` 的
has_mapping 口径：power 有映射（key/disabled）→ software 由 `gateway_key`
消费，native → passthrough。

## 修复清单（commit `3401845`）

- `RemoteButton` 增 `power`；`usageToButton[0x66]=.power`；B 级键 11→12。
- `volumeSystemKeyToUsage` 扩名 `auxControlKeyToUsage`，加 `6→0x66`。
- `XiaomiButtonInterceptManager`：新增电源键吞除窗/闩锁分支（见上）。
- `GatewaySupport.routes`：power native → passthrough、有映射 → software。
- `ButtonMappingWindowController`：电源键热区解锁（原置灰），附原生行为提示。
- 文档同步：`protocol.md` Key channel、`desktop-config.md`、方案文档 C 级定案
  撤销注记、`config.example.toml` 补 `[buttons]` 示例。

## 验证

- `swift build`（VOICESTICK_ARCHS=arm64）通过；`VoiceStickTests` 1053/1053
  （按键映射相关断言更新 + power 路由/解析新增）。
- **2026-10-19 真机验收通过（用户实测）**：小米遥控器电源键映射自定义后工作
  正常。这同时确证了两个实现前提：①macOS 确实把该键以 keyboard page 0x66
  报到 IOHID 层（`button down usage=0x66` 出现在日志）；②系统原生电源事件被
  tap 成功吞除（否则会观察到睡眠/关机对话框打断）。

## 长期技术记忆

- **macOS 拦截外接键盘电源键（usage 0x66）**：原生事件 = systemDefined 双
  形态（subtype 1 首事件 + aux-control subtype 8 的 NX_POWER_KEY=6 down/up 对），
  不产生 keyDown；吞除必须窗口期覆盖全部形态，一次性消费标记必漏吞。常量实证
  Apple `ev_keymap.h`/`IOLLEvent.h`。
- **查证 Apple 半私有常量的可靠路径**：CLT SDK 缺 IOKit hidsystem 时，用
  `phracker/MacOSX-SDKs` dump 仓库取 `ev_keymap.h`/`IOLLEvent.h`，再用成熟开源
  实现（Karabiner/PowerKey）交叉验证事件序列形态。比搜博客/论坛二手资料快且准。
- **dev 调试包记得自签**：`swift build` 产物是 ad-hoc 签名（每次构建 cdhash
  漂移，TCC 授权失效）；测试前用 `security unlock-keychain` + `codesign --force
  --sign "VoiceStick Local Code Signing" --keychain …voicestick-sign.keychain-db`
  重签，再 `open` 启动（勿 CLI 直启，TCC 归因失效静默丢蓝牙）。

## 遗留 / 观察项

- **网关模式 power 映射未真机验收**（本次验收走直连 RC 路径）；固件
  `gateway_keymap_set power=software` + `gateway_key` 消费链路的 power 分支
  待网关场景实测。
- **未映射电源键的 macOS 原生行为未记录**（用户验收聚焦自定义路径）；如遇
  用户问"不按映射时电源键什么效果"，需实测补充（对话框/睡眠/无动作，可能随
  macOS 版本与机型而异）。
- 电源键长按 = 遥控器自身电源管理（设备侧行为），桌面侧重复注入节拍与之
  互不干扰；若未来用户反馈"长按映射键连发与设备关机时序冲突"再评估
  `supportsRepeat(.power)` 是否需要特例。
