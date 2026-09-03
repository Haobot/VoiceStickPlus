# 小米遥控器 macOS 移植（P5）：实施与真机联调经验（2026-09-02）

- 日期：2026-09-02
- 分支/提交：`feat/add-MiRemote`，commit `847ca46`
- 相关位置：`desktop/macos/Sources/{VoiceStickCore,VoiceStickApp,COpus}/`、`desktop/macos/Tests/VoiceStickTests/`、方案文档 `Doc/Plan/xiaomi-remote-2-pro-support.md`（§5.4 已实施、§7.5 macOS 验收清单）
- 前置阅读：`Doc/Expe/xiaomi-remote-2pro-capture-and-f5-lessons-2026-09-02.md`（Windows 侧采集/F5 排障，设备行为事实来源）
- 时效声明：文中文件:行号、毫秒常量均为记录时点结论，引用前以当前源码为准。

## 1. 成果速览

Windows 端小米遥控器 2 Pro（ATVV）支持完整移植到 macOS 并真机打通（配对 Bond、按住说话识别粘贴）：

- 新增三 target：`VoiceStickCore` 库（ATVV 协议/ADPCM/后处理/Opus 编码/会话状态机，逐分支对齐 Windows）、`COpus`（vendored libopus v1.5.2，与 `desktop/windows/third_party/opus` 同源）、`VoiceStickTests`（executable 测试 runner，375 断言，含 golden fixtures 逐样本回放与 Opus 回环 SNR 51dB）。
- 平台差异方案：RC-XXXX 由 `CBPeripheral.identifier` 的 SHA-256 派生（macOS 拿不到 BLE MAC）；F5 抑制的钩内忙等改为「临时吞+100ms 回放」（CGEvent tap 不可阻塞）；不移植心跳探针（macOS 无 90s 拆除机制）。
- 验收状态：`swift build` + `swift run VoiceStickTests` 全绿；真机验收 §7.5 清单余项（双击 Enter、F5 不泄漏、睡眠唤醒等）进行中。

## 2. 真机联调两个坑（判据式）

### 坑 1：配对后 RC 永不连接——依赖注入闭包读到陈旧配置快照

- 症状：配对成功（菜单出现 RC-XXXX、F5 抑制已装载），但按语音键只有 HID 通道的 F5 刷屏，无任何 ATVV 连接。
- 日志判据：`XiaomiF5Suppressor: event tap installed`（证明配对条目已存在）+ **零** `connected RC-` 行 + F5 全部 `pass (replayed after 100ms)`（锚点从未被刷新 = ATVV 通道从未开）。
- 根因：`ble.pairedDevicesProvider = { self?.config.pairedDevices ?? [] }` 读的是 VoiceStickCoordinator 持有的 config **启动时快照**；配对流程只刷新了 AppDelegate 的 config 与磁盘，`updatePairedDeviceIDs` 不回灌 coordinator.config。RC 重连完全依赖配对条目反查（遥控器广播可能不带 ATVV UUID，过滤扫描看不到它），快照里没有条目 → 永远连不上。
- 修复：`updatePairedDeviceIDs` 内 `config.pairedDevices = AppConfig.load().pairedDevices`（调用方均已先落盘）。
- 长期记忆：**「写磁盘 + 内存快照」双态配置 + 闭包注入读取**，必须在每个状态变更点核对三方（磁盘/AppDelegate/coordinator）同步；闭包「实时读最新」的承诺取决于被读对象的刷新时机，不取决于闭包本身。

### 坑 2：`retrieveConnectedPeripherals` 取回的外设可能是 disconnected 态——空发 discoverServices 静默卡死

- 症状：app 重启后 RC 依然连不上，无任何报错。
- 日志判据：埋点后看到 `restored peripheral ... state=0 known=RC-78FA`（**state=0=disconnected**），之后 `didDiscoverServices` 回调永远不到——既无成功也无失败日志。「恢复路径日志打住、后续回调静默」即中此坑。
- 根因：遥控器空闲休眠后系统侧连接已断开，但 `retrieveConnectedPeripherals(withServices:)` 仍把它返回（取回瞬间状态已变）。恢复循环假设取回即连接，对 disconnected 外设调 `discoverServices`——CoreBluetooth 不报错也不回调，静默卡死。
- 修复：按 `peripheral.state` 分支——`.connected` 才 discoverServices，否则 `central.connect`（连接会 pending 到遥控器醒来广播）。
- 长期记忆：**macOS CoreBluetooth 任何「取回即恢复」路径都要先判 state**；对未连接外设发 GATT 操作是静默失败，不加日志永远看不出来。Windows 无对应物（WinRT 连接模型不同），这是纯平台坑。

## 3. 设备侧事实补充（配对）

- **遥控器只在配对模式接受绑定**：平时广播可被发现但拒绝 SMP 配对——app 内和 macOS 系统蓝牙都配不上时，先怀疑设备侧而非 app。进入配对模式 = **同时长按「主页键（房子）+ 菜单键（三横）」3~5 秒**（指示灯快闪）。已绑定其他主机（本例为 Windows 机）时也拒绝新绑定，先解绑/关旧主机蓝牙。顽固情况抠电池 10 秒硬复位蓝牙栈。
- **OS HID 连接与 ATVV 人格可共存（macOS 实测）**：macOS 系统持 HID 链路（方向键可用），app 同时拿到完整 ATVV service（`AB5E0001-…` 正常发现）——与 Windows 一致，「语音人格只授给第一个连上的主机」不包括 OS 自己的 HID 连接。
- 配对窗口 15s 超时偏紧于 Windows 的 30s（系统弹确认框时用户迟疑会超时，有提示文案兜底）；建议「先进配对模式再点 Pair」。

## 4. 平台工程坑（macOS 移植方法论）

- **CLT-only 无 Xcode 的机器上 `swift test` 不可用**：XCTest 与 swift-testing 的 `Testing.framework` 虽存在于 CLT，但 SwiftPM 不把其加入编译路径，`import XCTest`/`import Testing` 都在 import 阶段报 no such module。解法：纯逻辑拆库 target + **executable 测试 runner**（无框架断言 harness，`swift run VoiceStickTests`），并先把 `main.swift` 顶层代码改 `@main struct`（testTarget/runner 依赖 executable 的前提；`@main` 不能放在名为 main.swift 的文件里）。
- **GitHub 直连不通时 SwiftPM 依赖走系统代理**：`scutil --proxy` 查端口，`https_proxy=http://127.0.0.1:<port> swift build`（已入 Hub 提示）。
- **Swift 调不了可变参数 C 函数**：`opus_encoder_ctl` 必须在 C target 里包非可变参数 shim（`vs_opus_*`）。
- **AudioToolbox 的 Opus 编码需 macOS 14+**（解码 iOS 11/macOS 10.13 起，编码晚得多）：部署目标 12 不可用，vendored libopus 是唯一稳妥解；SwiftPM C target 编 libopus 要点——排除 `silk/fixed`（与 float 符号冲突）、arch 子目录（celt/x86 等）、demo/tests，define `OPUS_BUILD`/`USE_ALLOCA`/`HAVE_ALLOCA_H`/`HAVE_LRINT(F)`。
- **Swift `Data` 切片的 `startIndex` 不归零**：`removeFirst(n)` 后或传入切片时 `data[0]` 越界崩溃。入口归一化 `Data(data)`（实测归零）或内部缓冲用 `[UInt8]`。
- **`Character.isHexDigit` 收全角字符**（Unicode Hex_Digit 属性），与 C-locale `isxdigit` 不同：设备 ID 校验要 `$0.isASCII && $0.isHexDigit`。同理 `trimmingCharacters(.whitespacesAndNewlines)` 与 `uppercased()/lowercased()` 都是 Unicode 感知，比 C-locale 宽——现实设备名全是 ASCII/中文，无实际影响，列为已知差异。
- **CGEvent tap 回调不可阻塞**（会被系统禁用 tap）：Windows 钩内忙等 80ms 的「关联等待」层在 macOS 必须改成「临时吞 + ~100ms 延迟回放」，回放事件打 `eventSourceUserData` 标记防自吞；`eventSourceUnixProcessID != 0` 等价 Windows `LLKHF_INJECTED`（其他进程注入的按键不干预）。

## 5. 方法论收获

- **移植保真 ≠ 集成正确**。四路并行独立审查（状态机/纯逻辑/BLE 接入/app 层）确认状态机 14 要点零偏差，但 3 个 major 全在集成层与边界：`unsupported_codec` 一刀切拆链→无退避重连死循环（Windows 只拆 caps_timeout）；配对后缺 `retrievePeripherals` 补偿；配对窗红叉关闭无清理（后台扫描常驻+迟到落库）。纯逻辑对照逐行一致不代表端到端正确，**集成点（错误处理策略、生命周期清理、状态刷新时机）要单独审**。
- **无头环境排查 = 先埋点后重启观察**：两轮定位法——第一轮粗埋点（恢复计数/跳过分支原因）缩小到函数，第二轮细埋点（外设 state/回调是否到达）定位根因。比读代码推演快得多。
- SwiftPM 重构核心库时，「编译器驱动 public 化」顺畅：`swift build` 报错逐个加 `public`/显式 `public init`（public struct 的 memberwise init 默认 internal）即可。

## 6. 遗留 / 观察项

- 真机验收 §7.5 余项：双击 Enter、F5 不泄漏与真实 F5 回放、与 StickS3 同连、遥控器睡眠唤醒、配对窗红叉清理、`[device.x.xiaomi]` 热改生效（重连后）。
- AppConfig 新逻辑（paired CSV/xiaomi 表/normalizedDeviceID）无 runner 覆盖——runner 只链 VoiceStickCore，app 层靠 build + 人工推演；如需覆盖须再拆库或引入 Xcode。
- 既有问题（本次未动）：VS 配对不写 `paired_device` 条目；VS 分支配对用开窗时的 config 快照落库（并发设置改动会被覆盖）。
- macOS 端无电量显示、无「遥控器设置」对话框（仅 TOML）、无心跳探针——均为有意裁剪，见方案文档 §5.4。
