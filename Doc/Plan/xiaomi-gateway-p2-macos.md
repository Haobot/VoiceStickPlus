# P2 设计 — macOS 端网关适配

> 状态：**设计稿（待用户定案）**。承 `Doc/Plan/xiaomi-gateway-followup-roadmap.md` P2。
> 前置：P1（切换器）代码已落地；本文只覆盖 macOS 作为**目标端**需要补的东西。
> 核实时间 2026-09-20 06:4x，结论以当时源码行号为准。
> **2026-09-22 更新**：发现并修复 macOS 端自 `bffa235`（2026-09-08）起构建损坏的问题
>（见 §8）；§1/§2 据此补两个设计稿遗漏交付物。Mac 测试环境已确认可用（本机 arm64），
> roadmap §3 决策点 3 的「无 Mac 可测」前置不再成立。

## 1. 目标与交付物（承 roadmap P2）

1. `gateway_key` 事件解析（网关软件路由键）——现状 macOS 静默忽略。
2. HOGP 按键直通的**系统配对引导**——macOS 没有 Windows 式 `PairAsync`，需先 spike。
3. 语音链路真机验证（预期零改动，但必须实测）。
4. **`gateway_target_info` 主机名上报（2026-09-22 补，原稿遗漏）**——P1 目标表靠
   这条命令给目标机起名（固件绑定到当前连接对端并存 NVS）；macOS 不发，切换器
   屏幕上 Mac 目标就没有名字。Windows 参照 `voice_stick_coordinator.cc:162`（连接
   ready 即发）。
5. **`gateway_keymap_set` 路由表下发（2026-09-22 补，原稿遗漏）**——`gateway_key`
   事件只对被路由为 `software` 的键产生，而路由表默认全部 `passthrough` 且持久化在
   固件 NVS：macOS 若从不下发路由，软件路由键在 Mac 上是死功能；若只靠 Windows
   配置对话框设置，Mac 会继承 Windows 的路由表。对齐 Windows `PushGatewayKeymapRoutesFor`
   （连接 ready 时按本机按键映射幂等重发；两端各自连接时重发 = 当前活跃目标的配置
   生效，语义自洽）。macOS 侧映射源 = `[device.<id>.buttons]`（见 `Doc/Ref/desktop-config.md`）。

**验收**：Mac 上语音、按键直通、软件路由键三项均可用；配对引导不给用户制造「配两遍」的新负担。

## 2. 现状核实（2026-09-20，逐条带证据）

| 事实 | 证据 |
|---|---|
| macOS 端**零网关代码** | `desktop/macos` 全树 grep `gateway|Gateway` = 0 命中 |
| **macOS 端构建自 2026-09-08 起损坏（2026-09-22 发现，已修复，见 §8）** | `bffa235` 提交了「使用方」（AppDelegate 引用约 15 个 VoiceStickCore/app 类型）却从未提交「定义方」——`Package.swift` 无 VoiceStickCore target，`KeySpec`/`sendKeyCombo`/`RemoteButton`/`ButtonsSettings`/`ButtonMappingWindowController`/`FrontmostAppProvider` 等在全仓历史零定义；`swift build` 失败于 `import VoiceStickCore` |
| 状态事件白名单式解析，未知事件被丢弃 | `VoiceStickCoordinator.swift:398-416` `handleStateEvent` 的 `switch event.event`，`gateway_status`/`gateway_key` 走 `default: break` |
| 事件模型缺网关字段 | `BleProtocol.swift:13-30` `StateEvent` 只有 `event/button/hardware/firmware_version/buttons` |
| 控制通道已就绪，可直接下发新命令 | `BleCentral.swift:119-160` `sendUIState` 等一律 `writeValue(..., for: controlCharacteristic, type: .withoutResponse)` |
| 注入层只有硬编码按键 | `InputInjector.swift:35-61` 仅 `sendEnter()`/`sendCommandV()`，无通用 keymap 注入 |
| macOS 已有「遥控器直连 Mac」的按键拦截器（**另一种拓扑**） | `XiaomiButtonInterceptManager.swift`（IOHID + tap，见 `Doc/Plan/xiaomi-remote-button-mapping.md` §4.2）——遥控器直接配到 Mac 时用它；网关拓扑下按键走 HOGP 直通，不经此路 |

**关键推论**：音频与状态通道**确实零改动**（P1 的音频归一化在固件侧完成，macOS 收到的仍是标准 Opus 帧 + 同样的 `state_tx` 帧）；
缺的只有「网关键事件 → 本机按键注入」和「HOGP 直通所需的系统级配对」。

## 3. 设计

### 3.1 `gateway_key` 解析与注入（交付物 1）

- **协议侧**：`StateEvent` 增 `key: String?`、`pressed: Bool?`、`target: String?`（与 `Doc/Ref/protocol.md` 的 `gateway_key` 帧对齐，字段名以协议文档为准）。
- **协调器侧**：`handleStateEvent` 增 `case "gateway_key"` → 查用户 keymap（`ButtonsSettings` 同款结构，复用 macOS 已有按键映射配置）→ `InputInjector` 注入。
- **注入侧**：`InputInjector` 增通用 `sendKey(usage:modifiers:)`（CGEvent `virtualKey` 表由 usage→macOS keyCode 映射，表放在 `VoiceStickCore` 便于单测）。
- **可选（推荐）**：映射表为空时把键**回落为 HOGP 直通**（固件侧已是默认），避免用户没配映射就完全没反应。

### 3.2 HOGP 直通的系统配对引导（交付物 2，需 spike 先定案）

网关拓扑下，Stick 对目标机扮演 **BLE HID 键盘**；Windows 侧要求目标机与设备有 **OS 级配对**（见 `Doc/Expe/claude-memory-distilled.md` §1.10）。
macOS 的等价物是「系统设置 → 蓝牙 → 连接键盘」，而**核心问题是：系统配对与 app 的 CoreBluetooth 连接能否共存**。

**Spike 待答问题（按优先级）**：

1. macOS 能否同时：app 经 CoreBluetooth 连 `8f2f0b84-…` 服务 + 系统把同一设备当 HID 键盘？
   （Windows 上两者可共存；macOS 是否会在系统配对时抢走连接，或反之，需真机验证。）
2. 若不能共存，是否有 CoreBluetooth 侧的等效路径（访问受保护特征触发系统配对弹窗）？
3. 用户可见的最小引导是什么：`配对` 窗口加一行「若要在 Mac 上使用遥控器按键，请在系统设置里把 VS-XXXX 添加为键盘」（不额外要求配两遍 app）。

**结论落点**：spike 结果写入本文 §5，并据此决定 macOS 是否 v1 只支持「语音 + 软件路由键」，按键直通留待系统配对支持。

### 3.3 语音链路真机验证（交付物 3）

- 复用 Windows 侧的验收口径：小米语音键按下 → Mac 上出文本（ASR）→ 注入到焦点窗口。
- 分段时延实测（承 P3 §5.6 口径），确认网关拓扑不引入额外时延。

## 4. 分步实施顺序（每步可独立验收）

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | `StateEvent` 扩字段 + `gateway_key` 分支 + 单测（`RemoteButtonHIDTests` 同款 host 测试） | 单测覆盖 key/pressed 解析与未知键忽略 |
| 2 | `InputInjector.sendKey` + usage→keyCode 表（`VoiceStickCore` + 单测） | 单测覆盖映射；真机手测注入 |
| 3 | 3.2 的 spike（只写结论，不改产品代码） | 三个问题都有真机答案 |
| 4 | 配对引导文案/流程（依赖 3 的结论） | 用户按引导一次配对即可用按键 |
| 5 | 真机验收（语音 + 按键 + 软件路由键） | §1 验收 |

## 5. Spike 结果（待填）

| 问题 | 结论 | 证据 |
|---|---|---|
| 系统配对 + app 连接可否共存 | 待测 | — |
| 能否由 app 触发系统配对 | 待测 | — |
| 引导最小形态 | 待测 | — |

## 8. 2026-09-22 构建修复记录（P2 前置，已交付）

**问题**：`swift build` 自 `bffa235`（2026-09-08，已在 main）起失败——该提交把按键拦截
重构的「使用方」提交到了 main（AppDelegate +574 行引用约 15 个类型），「定义方」却
从未提交到任何分支。核实：`git log --all -S` 对 `KeySpec`/`sendKeyCombo`/`ButtonsSettings`
等全历史零定义命中。

**根因与修复**：这些定义实际完整存在于未合并的 `feat/add-MiRemote` 分支（2026-09-03，
258 文件 +67k 行，含 Package.swift 的 VoiceStickCore/测试 target、Localization、全部
设置窗口控制器）。分歧面量化：merge-base（09-02）以来 main 侧 macos 只改 5 个文件
（就是弄坏构建的 bffa235），**双方共同修改仅 `AppDelegate.swift` 与 `Info.plist`**。
处理：

1. `git merge feat/add-MiRemote`（备份分支 `backup/pre-macos-merge` 先行）。冲突 6 文件：
   AppDelegate 取 HEAD（bffa235 是 MiRemote 版的后续演进，diff 仅 178 行）；其余 5 个
   文档取并集。根 `VERSION`/firmware version.txt 无回退（base 即 2.3.8）。
2. 补齐全历史从未存在的类型（bffa235 架构意图，`RemoteButtonHIDTests` 为规格）：
   `VoiceStickCore/VoiceStickButtons.swift`（`RemoteButton`/`ButtonMapping`/`ButtonsSettings`）、
   `FrontmostAppProvider`、`ButtonMappingWindowController`（托盘「按键映射…」对话框）、
   AppConfig `[device.<id>.buttons]` 读写族 + `effectiveButtonsSettings(for:activeApp:)`
   （三期 app 级覆盖预留，v1 等价设备有效值）+ 协调器语音键双击（A 级）按映射处置。
3. 工具链适配：新 CLT 的 SwiftPM 显式模块构建要求 C target 有链接产物，纯头文件
   的 CZlib target 补 `CZlib_shim.c` 空翻译单元（修 `CZlib.o cannot be found`）。
4. 验证：`swift build` 通过；`swift run VoiceStickTests` **438/438 全过**（含
   `runRemoteButtonHIDTests`，其断言即重建类型的规格来源）。

**影响**：P2 的「第 1 步 StateEvent 扩字段」等实施项现在有了可编译的基线；
`bffa235` 意图的按键映射功能（含 A 级语音键双击动作）在 macOS 端完整可用。

## 6. 风险

| 风险 | 缓解 |
|---|---|
| macOS 抢占连接（系统 HID 与 CoreBluetooth 冲突） | spike 先定案；若冲突，v1 明确「语音优先」，按键直通只承诺 Windows |
| `usage → keyCode` 映射不完整（媒体键/电源键） | 映射表放 `VoiceStickCore` 单测覆盖；未映射键记日志并回落直通 |
| 无 Mac 设备可测 | **本项是排期前置条件**（见 roadmap §3 决策点 3） |

## 7. 待用户定案

1. **Mac 是否本期硬需求**（roadmap §3 决策点 3）——若无 Mac 可测，本文只能停留在设计 + spike 待办。
2. 软件路由键的默认映射（OK/返回/方向键 → 哪些 macOS 按键）是否沿用 Windows 的默认表？
3. HOGP 直通若在 macOS 上确实需要系统配对且体验割裂，是否接受「macOS v1 只支持语音 + 软件路由键」？