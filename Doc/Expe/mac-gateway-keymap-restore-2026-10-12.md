# 小米遥控器网关模式按键自定义界面消失、除录音键外按键全无作用——macOS 四断链定位与恢复（含静默丢表根因）

- 日期：2026-10-12（诊断 + 修复 + 提交完成）
- 相关文件：
  - `desktop/macos/Sources/VoiceStickApp/StatusController.swift`、`AppDelegate.swift`、`ButtonMappingWindowController.swift`、`BleCentral.swift`
  - `desktop/macos/Sources/VoiceStickCore/AppConfig.swift`、`ConfigFileModels.swift`、`ConfigDomain.swift`、`BleServing.swift`、`VoiceStickCoordinator.swift`
  - `desktop/macos/Tests/VoiceStickTests/GlobalButtonsConfigTests.swift`、`CoordinatorFsmTests.swift`
- 相关 commit：`65a94f7`（本次修复）；参照 `b65baf3`（Windows 网关映射 UI 入口先例）、`f7dca2b`（并行发版会话 2.4.10 版本号提交）
- 关联旧文（固件/Windows HID 侧）：`Doc/Expe/xiaomi-gateway-voice-key-and-hid-passthrough-2026-09-18.md`

## 1. 症状

用户报障：「之前有的手柄按钮的自定义界面和功能都不见了，而且目前除了录音键以外，其他按键都没有实际作用」。
现场：macOS 端（本机日志 `~/Library/Application Support/VoiceStick/VoiceStickApp.log`），遥控器经 StickS3 网关模式接入（日志 `gateway status dev=53A8 mode=gateway`），录音键（ATVV 链路）正常。

## 2. 日志判据（本次取证，2026-10-11/12 日志）

1. **入口**：托盘 VS-53A8 子菜单只有交互/编码器/体感/电池项，无「按键映射…」——该入口在 `StatusController.addDeviceItems` 仅 `isXiaomi`（配对 RC 条目）分支可见，网关模式桌面端拿不到 RC 条目 ⇒ 永不可见。
2. **下发**：每次连接出现整批 `BLE send gateway_target_info ... skipped dev=VS-53A8 (not connected)` + 13×`gateway_keymap_set ... skipped`；全日志 gateway_keymap_set 共 **182 条、182 条全 skipped、零成功**。时序铁证：didConnect 的 onConnectionChange 在 00:10:02.168 触发，控制特征到 00:10:02.932 才就绪（首条非 skipped 发送），差 ~0.76s，且就绪后**无人补推**。
3. **消费**：`gateway key ... -> inject` 与 `routed software but mapping native` 日志为零——无 RC 条目时 `config.buttonsSettings` 恒返回 `ButtonsSettings.default`，即便固件以 software 路由回来的键也会被「native 忽略」。
4. **配置回读**（提交后由回归断言钉死）：`AppConfig.parse(cfg.serializedText())` 回读丢 `[output].target` ⇒ 整表解码失败已静默回落 `loadLegacy`。

## 3. 根因（四条独立断链，均于本次修复）

- **A 入口缺失（UI）**：macOS 未移植 Windows `b65baf3` 的「StickS3 设备项也给按键映射入口（网关走全局对话框）」，只做了 RC 条目分支。
- **B 全局存储缺失（配置）**：`AppConfig.buttonsSettings` 是恒返回 `.default` 的计算属性，「无配对 RC 回落全局默认路由」（P2 交付语）没有存储支撑——全局映射无处可写、写了也读不回。
- **C 下发时序竞态（BLE）**：`onConnectionChange` 在 didConnect 触发，而 `sendStickControlPayload` 依赖的 `controlCharacteristics` 要等 `didDiscoverCharacteristicsFor` 才存在——首连时刻全部下发被 `(not connected)` 丢弃且此后无补发路径。固件因此永远收不到本机的 `gateway_target_info` 与 13 键路由表。
- **D 静默丢表（先于本功能的潜伏 bug，「功能不见了」的另一半根因）**：`AirMouseSettings.tomlNumber`（`%.4g`）把整数值 Double 写成 `3`/`80`/`4000`（TOML 整数形态）；TOMLKit 解 `Double` 严格要求 `.float`（`TOMLValueConvertible.double` 先判 `type == .double`，`.int` 直接 nil → `DecodingError.typeMismatch`）→ `ConfigFile` **整表**解码失败 → `AppConfig.parse` 的 `try?` **静默**回落 `loadLegacy`（逐行解析，不认任何 `[表]` 段，且无一行日志）⇒ 每次保存后重启，所有 `[device.*]` 表（xiaomi/interaction/encoder/buttons/output 覆盖）全部丢失。本机 config 自 10-11 起即含 `air_mouse_neutral_deadzone = 3` 等整数形态 ⇒ 每次启动都在 legacy 回落态，期间写入过的任何表段都不会被读到。与 `XiaomiConfigFile.gain_db` 的整数宽容是同族问题——当时只修了 xiaomi 子表，根表 9 个 `air_mouse_*` Double 漏网。

**流程插曲**：修复进行到一半时工作区被并行发版会话 `git stash push -u`（wip 提交写明「网关模式按键映射窗口+控制通道就绪补推+GlobalButtonsConfigTests，发版后 pop 恢复」）——表现为 `git status` 突然干净、刚写的文件全部消失、测试跑回旧基线 768。处置见 §6。

## 4. 修复（机制，commit `65a94f7`）

- **A**：`StatusController` StickS3 else 分支新增 `onOpenGatewayButtonMapping` 入口（标题复用 `menuButtonMapping`，对齐 Windows kMenuXiaomiKeymap 非小米分支）；`AppDelegate.showGatewayButtonMapping` 编辑全局表并 `updateConfig` 热更下发；`ButtonMappingWindowController` 新增 `hidesIntercept`（网关模式隐藏 HID 拦截开关——那是直连 RC 专属能力，网关链路不消费，避免误导）。
- **B**：`AppConfig` 新增存储属性 `globalButtonsSettings`（默认 `.default`）；根 `ConfigFile` 新增 `buttons` 字段；`parse` 接入既有转换器 `VoiceStickCore.buttonsSettings(from:)`；`serializedText` 追加 `globalButtonsText`（与默认一致不写出，键行抽取共用 `buttonsKeyLines` 保证与设备段同形）；`buttonsSettings` 计算属性改返回 `globalButtonsSettings`——协调器路由推导与 `gateway_key` 消费（都走 `buttonsSettings(for: activeXiaomiDeviceID)`）自动同源，直连 RC 设备覆盖仍优先。语义对齐 Windows `[xiaomi.keys]`。
- **C**：`VoiceStickBleServing` 新增 `onControlChannelReady: ((String) -> Void)?`；`BleCentral` 在控制特征 `didDiscoverCharacteristicsFor` 分支触发（仅 StickS3——RC 设备提前走 ATVV 分支不达）；协调器 `start()` 接线，就绪即补推 `sendDeviceInteractionSettings` + `pushGatewayConfig`（幂等，与连接路径重叠多发一轮无害）。
- **D**：读侧 `ConfigFileModels` 新增 `LenientDouble`（Double → Int → 都不是才抛错），根表 9 个 `air_mouse_*` 双精度字段改用它、`parse` 侧 `.value` 解包；写侧 `tomlNumber` 规范 float 形态（无 `.`/`e` 补 `.0`，inf/nan 原样）。双保险：存量整数形态可读、新写出为规范 TOML float。

## 5. 验证

- `VOICESTICK_ARCHS=arm64 swift build` 通过；`swift run VoiceStickTests` **798/798 全绿**（基线 768 + 新增 30：`GlobalButtonsConfigTests` 21 条——load 三态/回落/路由同源/整数宽容下根表与 `[device.*]` 表存活/解码主路径探针/round-trip 恒等+序列化幂等/缺段默认；`CoordinatorFsmTests` 网关三链 9 条——就绪补推 13 键+target_info+交互设置、全局映射热更 software/passthrough 分流、gateway_key 注入与禁用吞键）。
- `python3 scripts/test_release_guard.py` 30/30 OK；`python3 scripts/release_guard.py` 13 检查 12 过，唯一红项 `versions: CHANGELOG 2.4.9 != VERSION 2.4.10` 系并行发版会话在途（`f7dca2b` 只提交了 VERSION/version.txt），与本改动无关。
- **真机 UI 回归与遥控器实按验收：待下次真机窗口**（日志取证与单测充分，但托盘点开→映射→实按的端到端未在真机走）。行号/时间戳为记录时点结论，引用前以当前源码/日志为准。

## 6. 长期技术记忆 / 经验

- **「配置/自定义保存后重启就没了」先查 parse 主路径 vs legacy 回落**：`AppConfig.parse` 的 `try?` 把任何单字段 typeMismatch 变成静默 `loadLegacy`（表段全丢、零日志）。快速判据：拿序列化文本回读比对一个**只存在于表里**的字段（如 `[output].target`——legacy 只读根 `output_target`，回读为默认值即证明回落）。
- **TOMLKit 解 Double 是严格类型**：`.int` 形态必抛错。凡 `%.4g`/`%g` 类格式化写出的数字，整数值必须补 `.0`；新增 Double 配置字段读侧一律走 `LenientDouble` 或同款宽容（先例：`XiaomiConfigFile.gain_db`——同族问题当时只修了子表，根表漏网才酿成本次全表丢失）。
- **「下发全被 (not connected) 丢弃」类竞态判据**：连接日志后同批 `skipped`、且**此后该 label 再无成功行** ⇒ 发送依赖的资源（特征/订阅）就绪晚于触发点且无补发路径。就绪补推必须挂在资源就绪回调上（本次：控制特征 `didDiscoverCharacteristicsFor`），只挂 `onConnectionChange` 必然踩 didConnect 早于服务发现的窗口。
- **网关模式的 UI 入口按「设备项」出，不按 RC 条目出**：桌面端拿不到 RC 条目是常态（遥控器配对在 StickS3），入口/存储/消费三层都要有「无 RC 回落全局」的实现（Windows `b65baf3` 先例，macOS 本次补齐；三者缺一层链路即断）。
- **并行会话工作区突变**：`git status` 突然干净、自己刚写的文件全消失、测试跑回旧基线 → 先 `git stash list`（wip 提交会写明缘由与恢复时机），`git stash show --stat` 核对确是自己的改动再 `git stash pop`；恢复后重跑构建/测试（stash 期间他人可能已提交，基线已变）。

## 7. 遗留 / 观察项

- 真机 UI 回归与遥控器实按验收待做；长按连发（Windows P1 的 `XiaomiGatewayKeyRepeater`）macOS 仍未移植，软件路由键长按只有单次注入。
- `loadLegacy` 依旧不认 `[表]` 段（古早配置兼容所需）——正常配置已不会再落到它，「解码主路径探针」测试守回归。
- 网关模式「遥控器设置」（gain_db/double_click_ms）仍需 RC 条目：roadmap 根因 2 的「RC 条目获取路径」待办保留，本次只处置了按键映射半边（`Doc/Plan/xiaomi-gateway-followup-roadmap.md` 已标注）。
- 遥控器「原生」键走固件 HOGP 直通，需目标机与 StickS3 有 OS 级配对（既有结论见关联旧文）；软件路由键（映射过的键）不受此限制。

## 8. 追加（2026-10-12 深夜二轮）：真机端到端验收暴露固件侧两雷（已修复 + 已烧录）

入口/存储/下发三线修复上真机后，连接出现 **4-5s 崩溃重启循环**（每轮 13 键送达后断链）。串口取证（单次 open、DTR/RTS 压低）+ ELF addr2line（`firmware/build/voice_stick.elf` SHA256 前 9 位与 boot 横幅 `ELF file SHA256: b454859c8` 一致 ⇒ 地址解析逐帧可信）定位两层根因，修复于 commit `7736441`：

1. **host 任务栈压穿崩溃**：`gateway_keymap_set` 回执 `send_gateway_keymap_report()` 在 ATT 写回调内**同步**执行（backtrace：`ble_att_svr_write → control_access_cb → execute_control_cmd → send_gateway_keymap_report → voice_ble_send_gateway_keymap → send_state_json → ble_att_mtu → ble_hs_lock → xQueueSemaphoreTake`），跑在 nimble host 任务（栈 4096B）深处——`json[512]`+分片拼装+大 switch 叠加压穿栈 → 堆上 FreeRTOS 队列对象被毁 → `assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)` → `rst:0xc (RTC_SW_CPU_RST)` 循环。
   - **判据**：串口每 4-5s 一组**同款 assert+backtrace**；app 侧 `Connected VoiceStick` 每 4-5s 一条（连接循环）。A/B（旧包单连接稳定）+ 探针二分（仅停发 13 键批 → 稳定；仅发 target_info → 稳定）锁定触发组。
   - **修复**：SET/GET 置待发标志 + 新 `APP_EVENT_GW_KEYMAP_REPORT` 移交 `app_event_task` 发送（清标先行，同批 13 键合并为一次全表回执；竞争漏发由连接/热更重推自愈）；`app_event_task` 与 host 任务栈 4096→5120。
2. **回执 JSON 缺数组括号（A15 起潜伏）**：分片体是 `{"key":...},{...}` 条目序列，`voice_ble_send_gateway_keymap` 直接嵌入 `"routes":%s` 未包 `[]` → 非法 JSON → 桌面端 JSONDecoder/rapidjson 解码失败**静默丢弃全部回执**。空表兜底 `"[]"` 掩盖了非空路径缺陷；A15 harness 只验分片序、不验 JSON 合法性（**教训：格式类修复的测试必须断言产物可解析**）。
   - **判据**：app 侧 `gateway keymap report` 完成日志恒为 0、设备侧却报 `gateway keymap report seq=...` 已发；临时 DIAG 打出 payload 才见 `"routes":{...},{...}` 畸形。
   - **修复**：发送前包 `[...]`。**Windows 同固件族的键位映射对话框回执同样受害**，随 2.4.10 固件轮一并修复。

**验证（真机，2026-10-12 03:0x）**：macOS 2.4.10 包 = 单连接 ≥85s 零循环、13 键 `gateway_keymap_set` 全送达、`gateway keymap report (13 keys)` 完整回显 12 次；串口单次采集仅脚本自身一次 `USB_UART_CHIP_RESET`、零 assert；mac 测试 798/798、固件 host 7/7。

**新判据/观察项**：
- 该板出 Boot：`hard_reset` 无效、esptool 4.12 无 `usb_jtag_reset` → **`--after watchdog_reset` 可免手动按键**（本次两轮烧录实证）。
- 串口采集脚本 open 自身会触发一次 `USB_UART_CHIP_RESET`——uptime 单调/仅一次 banner = 无扰动（技能既有判据再验证）。
- `gateway_target_info 缺少 name 或无当前对端` 警告（mac 连接在固件目标表登记失败）——侧键切换目标列表可能缺 MacBook，**遗留观察**，不影响按键/语音链路。
- 取证期间疑似见到对端（Windows 机）重连推送的批次——多机同抢一台网关时回执/路由会互踩，**遗留观察**。
