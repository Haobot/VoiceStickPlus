import Foundation
import VoiceStickCore

// N1 闸9：协调器状态机只读快照——整类迁 Core 后（类型依赖清零）由 runner 直接构造，
// 全协议假物吸收副作用；快照初态断言 + updateConfig 往返。状态枚举保持 private，
// 快照以字符串外泄最小面（构造即验证 9 参注入链全通）。

private final class FakeStatusSink: VoiceStickStatusSink {
    var receivedStatuses: [String] = []
    var receivedConnected: [[ConnectedVoiceStickDevice]] = []
    func setStatus(_ text: String) { receivedStatuses.append(text) }
    func setPairedDeviceIDs(_ deviceIDs: [String]) {}
    func setConnectedDevices(_ devices: [ConnectedVoiceStickDevice]) { receivedConnected.append(devices) }
    var receivedFirmwareInfo: [[String: DeviceFirmwareInfo]] = []
    func setFirmwareInfo(_ infoByDeviceID: [String: DeviceFirmwareInfo]) {
        receivedFirmwareInfo.append(infoByDeviceID)
    }
    func setDeviceEncoderPresent(_ deviceID: String, present: Bool) {}
    func setDeviceBattery(_ deviceID: String, level: Int, charging: Bool, usbPowered: Bool) {}
    func setAirMouseActive(_ active: Bool, deviceID: String) {}
    func setHasRecoverableInput(_ hasRecoverableInput: Bool) {}
    func showListening(deviceID: String?) {}
    func showPartial(_ text: String, deviceID: String?) {}
    func showRefining(_ text: String, deviceID: String?) {}
    func appendPartial(_ text: String, deviceID: String?) {}
    func showPausedFinal(_ text: String, deviceID: String?) {}
    var shownFinalTexts: [String] = []
    func showFinal(_ text: String, deviceID: String?, onHidden: (() -> Void)?) {
        shownFinalTexts.append(text)
        // 模拟真机浮窗确认倒计时结束（OverlayController 延时回调 onHidden → 提交粘贴）。
        DispatchQueue.main.async { onHidden?() }
    }
    /// 语音编辑 toast 断言用：记录 timed message 文案。
    var timedMessages: [String] = []
    func showTimedMessage(_ text: String, duration: TimeInterval, deviceID: String?,
                          onHidden: (() -> Void)?) {
        timedMessages.append(text)
    }
    func showError(_ text: String, deviceID: String?, onHidden: (() -> Void)?) {}
    func hideOverlay(onHidden: (() -> Void)?) {}
    func hideOverlay(deviceID: String?, onHidden: (() -> Void)?) {}
}

private final class FakeBleServing: VoiceStickBleServing {
    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?
    var onControlChannelReady: ((String) -> Void)?
    var onStateEvent: ((UUID, StateEvent) -> Void)?
    var onAudioFrame: ((UUID, AudioFrame) -> Void)?
    var onMotionFrame: ((UUID, MotionFrame) -> Void)?
    var onPowerLogFragment: ((UUID, PowerLogFragment) -> Void)?
    var onPowerMgmtEvent: ((UUID, PowerMgmtEvent) -> Void)?
    var pairedDevicesProvider: (() -> [PairedDeviceEntry])?
    var xiaomiOptionsResolver: ((String) -> XiaomiAtvvSession.Options)?
    let micOpenAnchor = XiaomiMicOpenAnchor()

    func start() {}
    func updatePairedDeviceIDs(_ deviceIDs: [String]) {}
    /// 网关推送链测试用：置位后 updateConfig 的逐台单播循环视为有已连接 StickS3。
    var stickDeviceIDs: [String] = []
    func connectedStickDeviceIDs() -> [String] { stickDeviceIDs }
    /// 控制通道下发记录（label, deviceID）——就绪补推/热更下发断言用。
    var sentControlLabels: [(label: String, deviceID: String)] = []
    func deviceClass(for peripheralID: UUID) -> DeviceClass { .stickS3 }
    var stubDeviceID: String? = "TEST"
    func deviceID(for peripheralID: UUID) -> String? { stubDeviceID }
    func peripheralID(forDeviceID deviceID: String) -> UUID? { nil }
    func isConnected(_ peripheralID: UUID) -> Bool { false }
    func isConnected(deviceID: String) -> Bool { false }
    var sentInteractionModes: [InteractionMode] = []
    func sendInteractionMode(_ mode: InteractionMode, to peripheralID: UUID?) {
        sentInteractionModes.append(mode)
    }
    func sendPowerLogCommand(_ data: Data, to deviceID: String) {}
    func sendRemoteButton(action: String, deviceID: String, requestID: UInt32) {}
    func sendShowIMUDebug(_ enabled: Bool, to peripheralID: UUID?) {}
    func sendStickControlPayload(_ data: Data, label: String, deviceID: String) {
        sentControlLabels.append((label: label, deviceID: deviceID))
    }
    func sendUIState(_ state: String, text: String, to peripheralID: UUID?) {}
    func cancelFirmwareUpdate() {}
    func updateFirmware(image: Data, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {}
}

private final class FakeInput: VoiceStickInputServing {
    var onAccessibilityPermissionMissing: (() -> Void)?
    /// 已粘贴文本记录（B6 精修预算断言用；真机由 OverlayController 倒计时结束驱动注入）。
    var pastedTexts: [String] = []
    /// 每次 paste 的 pressEnter 值（targeted 替换断言必须 false）。
    var pastedEnterFlags: [Bool] = []
    /// targeted 协调器测试 hook：注入副作用落到模拟焦点字段。
    var onPaste: ((String, Bool) -> Void)?
    var onSendKeyCombo: ((KeySpec) -> Void)?
    func paste(text: String, pressEnter: Bool) {
        pastedTexts.append(text)
        pastedEnterFlags.append(pressEnter)
        onPaste?(text, pressEnter)
    }
    func clickLeftButton() {}
    func moveMouse(dx: Int, dy: Int) {}
    func sendArrowDown() {}
    func sendArrowUp() {}
    func sendEnter() {}
    /// gateway_key 注入断言用：记录收到的组合键。
    var sentKeyCombos: [KeySpec] = []
    func sendKeyCombo(_ spec: KeySpec) {
        sentKeyCombos.append(spec)
        onSendKeyCombo?(spec)
    }
}

private final class FakeSubtitle: VoiceStickSubtitleServing {
    func show(text: String, deviceID: String, color: OverlayThemeColor) {}
    func hideAll() {}
}

private final class FakeRecorder: VoiceStickDebugRecordServing {
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String) {}
    func append(_ data: Data) {}
    func finish() {}
    func discard() {}
}

private final class FakeASR: ASRClient {
    var onPartial: ((String) -> Void)?
    var onSegment: ((ASRSegment) -> Void)?
    var onFinal: ((String) -> Void)?
    var onError: ((String) -> Void)?
    var onUpgradeURL: ((URL, String) -> Void)?
    func start(options: ASRSessionOptions) -> Bool { true }
    func sendOggOpusChunk(_ data: Data, isLast: Bool) {}
    func finish() {}
    func cancel() {}
}

private final class FakeTranslator: TranslatorServing {
    func translate(_ text: String, targetLanguage: String, hotwords: [String],
                   completion: @escaping (Result<String, Error>) -> Void) {
        completion(.success(text))
    }
}

private final class FakeRefiner: RefinerServing {
    enum Behavior {
        /// 同步完成（ok=true，fallback=none）。
        case completeImmediately(String)
        /// 延迟完成：故意不检查 cancel.isCancelled，迟到 complete 由协调器侧
        /// RefineCancelToken 守卫丢弃（B2 预算回退路径的回归点）。
        case completeAfter(TimeInterval, String)
        /// 永不回调（预算超时路径）。
        case neverComplete
        /// 立即发一次端点活动信号（模拟推理型模型思考链增量），随后永不 complete
        ///（P1 reasoning 活动喂活路径：首 token 预算不误杀，总预算仍兜底）。
        case activityThenNeverComplete
    }
    var behavior: Behavior = .completeImmediately("精修结果")
    private(set) var callCount = 0
    private(set) var lastText: String?

    func refineStream(_ text: String, promptOverride: String, hotwords: [String]?,
                      corrections: [CorrectionPair],
                      cancel: RefineCancelToken,
                      onToken: @escaping (String) -> Void,
                      onActivity: @escaping () -> Void,
                      onComplete: @escaping (Bool, String, RefineFallback) -> Void) {
        callCount += 1
        lastText = text
        switch behavior {
        case .completeImmediately(let result):
            onComplete(true, result, .none)
        case .completeAfter(let delay, let result):
            DispatchQueue.global().asyncAfter(deadline: .now() + delay) {
                onComplete(true, result, .none)
            }
        case .neverComplete:
            break
        case .activityThenNeverComplete:
            onActivity()
        }
    }
}

func runCoordinatorFsmTests() {
    let sink = FakeStatusSink()
    let fakeBle = FakeBleServing()
    let fakeInput = FakeInput()
    let coordinator = VoiceStickCoordinator(
        config: .defaults,
        statusController: sink,
        ble: fakeBle,
        makeAsr: { _ in FakeASR() },
        makeTranslator: { _ in FakeTranslator() },
        makeRefiner: { _ in FakeRefiner() },
        inputInjector: fakeInput,
        subtitleController: FakeSubtitle(),
        makeDebugRecorder: { _ in FakeRecorder() }
    )
    // 测试隔离：用户数据文件（纠错对/热词候选）写入会话级临时目录。
    coordinator.userDataDirectoryOverride = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_fsm_userdata_1")
    let snap = coordinator.fsmSnapshot
    check(snap.contains("main=ready"), "N1fsm: 初态 main=ready")
    check(snap.contains("pendingPaste=idle"), "N1fsm: 初态 pendingPaste=idle")
    check(snap.contains("subtitleCycles=0"), "N1fsm: 初态无字幕周期")
    coordinator.updateConfig(.defaults)
    let snap2 = coordinator.fsmSnapshot
    check(snap2.contains("main=ready"), "N1fsm: updateConfig 往返仍 ready")
    check(snap2.contains("subtitleCycles=0"), "N1fsm: 往返后仍无字幕周期")

    // 事件链：start() 才装配 ble 闭包（公共注册 API；副作用=fake ble + 异步 manifest 尝试
    // + 刷新定时器 deinit 复位，测试进程即退不等异步）——随后直调捕获的连接闭包。
    coordinator.start()
    fakeBle.onConnectionChange?([ConnectedVoiceStickDevice(
        name: "VS-TEST", deviceID: "TEST", deviceClass: .stickS3)])
    check(sink.receivedConnected.count == 1, "N1fsm: 连接回调抵达 sink 一次")
    check(sink.receivedStatuses.contains("Ready"), "N1fsm: 有设备连接即置 Ready")
    check(!fakeBle.sentInteractionModes.isEmpty, "N1fsm: 连接即下发交互模式")
    check(coordinator.fsmSnapshot.contains("main=ready"), "N1fsm: 事件链后状态机仍一致")
    // 状态事件链：device_info 经 JSON 解码（StateEvent 为 Decodable，跨模块免成员构造）
    // → handleStateEvent → updateDeviceFirmwareInfo → refresh（manifest 为空亦必经末行）
    // → sink.setFirmwareInfo 可观察。
    let jsonData = #"{"event":"device_info","hardware":"stick_s3","firmware_version":"9.9.9"}"#.data(using: .utf8)!
    let deviceInfo = try! JSONDecoder().decode(StateEvent.self, from: jsonData)
    fakeBle.onStateEvent?(UUID(), deviceInfo)
    let fw = sink.receivedFirmwareInfo.last ?? [:]
    check(fw["TEST"]?.currentVersion == "9.9.9", "N1fsm: device_info 事件落固件字典")
    check(fw["TEST"]?.hardware == "stick_s3", "N1fsm: 硬件字段随事件更新")
    check(coordinator.fsmSnapshot.contains("main=ready"), "N1fsm: 状态事件链后状态机仍一致")
    // 电量透传链：fake-ble 片段 → start() 装配的内部闭包 → deviceID 映射（TEST 桩）
    // → 协调器公共出口 onPowerLogFragment（App 端同款接法）。
    var gotFragment: (String, PowerLogFragment)?
    coordinator.onPowerLogFragment = { dev, frag in gotFragment = (dev, frag) }
    let fragJSON = #"{"seq":1,"offset":0,"total":10,"eof":false,"data":"AQID"}"#.data(using: .utf8)!
    let frag = try! JSONDecoder().decode(PowerLogFragment.self, from: fragJSON)
    fakeBle.onPowerLogFragment?(UUID(), frag)
    check(gotFragment?.0 == "TEST", "N1fsm: 片段经 deviceID 映射到出口")
    check(gotFragment?.1.data == Data([1, 2, 3]), "N1fsm: 片段负载 base64 保真")
    check(gotFragment?.1.total == 10, "N1fsm: 片段元数据保真")
    // 电源管理链：PowerMgmtEvent 直造（r119 public init）→ 同款映射 → 公共出口。
    var gotPower: (String, Bool)?
    coordinator.onPowerMgmtEvent = { dev, ev in gotPower = (dev, ev.usbAutoOff) }
    fakeBle.onPowerMgmtEvent?(UUID(), PowerMgmtEvent(usbAutoOff: true))
    check(gotPower?.0 == "TEST", "N1fsm: 电源事件经映射到出口")
    check(gotPower?.1 == true, "N1fsm: usbAutoOff 负载保真")
    check(coordinator.fsmSnapshot.contains("main=ready"), "N1fsm: 电源链后状态机仍一致")

    // ---- 网关按键自定义恢复链（P1 macOS 补口）----
    // 1) 控制通道就绪补推：真机缺口=didConnect 时刻下发 182/182 被 "(not connected)"
    //    丢弃、特征发现后无人补推（固件永远收不到本机路由表与目标名）。
    fakeBle.sentControlLabels.removeAll()
    fakeBle.onControlChannelReady?("TEST")
    let readyPushes = fakeBle.sentControlLabels.map(\.label)
    check(readyPushes.filter { $0.hasPrefix("gateway_keymap_set") }.count
          == GatewaySupport.routableKeys.count,
          "gwPush: 就绪补推 13 键路由全量")
    check(readyPushes.contains { $0.hasPrefix("gateway_target_info") },
          "gwPush: 就绪补推目标机名")
    check(readyPushes.contains("tap_enabled"),
          "gwPush: 就绪补推按设备交互设置")

    // 2) 全局 [buttons] 映射 → updateConfig 热更逐键重发（软件路由/直通分流）。
    fakeBle.stickDeviceIDs = ["TEST"]
    fakeBle.sentControlLabels.removeAll()
    var gwConfig = AppConfig.defaults
    var gwButtons = ButtonsSettings.default
    gwButtons.setMapping(ButtonMapping(action: .key, key: "backspace"), for: .back)
    gwButtons.setMapping(ButtonMapping(action: .disabled, key: ""), for: .volUp)
    gwConfig.globalButtonsSettings = gwButtons
    coordinator.updateConfig(gwConfig)
    let hotPushes = fakeBle.sentControlLabels.map(\.label)
    check(hotPushes.contains("gateway_keymap_set back=software"),
          "gwPush: 映射键热更下发软件路由")
    check(hotPushes.contains("gateway_keymap_set volume_up=software"),
          "gwPush: 禁用键热更下发软件路由（吞在桌面侧）")
    check(hotPushes.contains("gateway_keymap_set up=software"),
          "gwPush: 未映射键（原生）热更下发软件路由（桌面自然键注入）")

    // 3) gateway_key 回流 → 按全局映射注入 / 禁用键吞掉。
    let gwDown = try! JSONDecoder().decode(
        StateEvent.self,
        from: #"{"event":"gateway_key","key":"back","pressed":true}"#.data(using: .utf8)!)
    fakeBle.onStateEvent?(UUID(), gwDown)
    check(fakeInput.sentKeyCombos.last == KeySpec.parse("backspace"),
          "gwConsume: gateway_key 按全局映射注入")
    let gwVolDown = try! JSONDecoder().decode(
        StateEvent.self,
        from: #"{"event":"gateway_key","key":"volume_up","pressed":true}"#.data(using: .utf8)!)
    let combosBefore = fakeInput.sentKeyCombos.count
    fakeBle.onStateEvent?(UUID(), gwVolDown)
    check(fakeInput.sentKeyCombos.count == combosBefore,
          "gwConsume: 禁用键吞掉不注入")
    // 原生键（未映射）→ naturalSpec 自然键注入（macOS 系统 HID 不可用的原生语义实现）。
    let gwUp = try! JSONDecoder().decode(
        StateEvent.self,
        from: #"{"event":"gateway_key","key":"up","pressed":true}"#.data(using: .utf8)!)
    fakeBle.onStateEvent?(UUID(), gwUp)
    check(fakeInput.sentKeyCombos.last == KeySpec.parse("up"),
          "gwConsume: 原生键按自然映射注入（up→方向上）")
    check(coordinator.fsmSnapshot.contains("main=ready"), "gw: 消费链后状态机仍一致")
}

// ---- B6 精修预算回退 + 短文本跳过（refine-latency-optimization 方案）----

private func makeRefineHarness() -> (coordinator: VoiceStickCoordinator, sink: FakeStatusSink,
                                     ble: FakeBleServing, input: FakeInput,
                                     asr: FakeASR, refiner: FakeRefiner) {
    let sink = FakeStatusSink()
    let fakeBle = FakeBleServing()
    let fakeInput = FakeInput()
    let asr = FakeASR()
    let refiner = FakeRefiner()
    let coordinator = VoiceStickCoordinator(
        config: .defaults,
        statusController: sink,
        ble: fakeBle,
        makeAsr: { _ in asr },
        makeTranslator: { _ in FakeTranslator() },
        makeRefiner: { _ in refiner },
        inputInjector: fakeInput,
        subtitleController: FakeSubtitle(),
        makeDebugRecorder: { _ in FakeRecorder() }
    )
    // 测试隔离：用户数据文件（纠错对/热词候选）写入会话级临时目录。
    coordinator.userDataDirectoryOverride = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_fsm_userdata_2")
    // updateConfig 经 make* 闭包重建 asr/refiner（同实例）并装配 ASR 回调（init 不装配）。
    coordinator.updateConfig(.defaults)
    return (coordinator, sink, fakeBle, fakeInput, asr, refiner)
}

/// 推进主线程 RunLoop：服务 GCD 主队列块与 Timer——预算断言全靠真实定时器触发。
private func pumpMainRunLoop(_ seconds: TimeInterval) {
    RunLoop.current.run(until: Date().addingTimeInterval(seconds))
}

func runRefineBudgetTests() {
    // 与 VoiceStickCoordinator 的 kRefineFirstTokenBudget（2.5s，private）保持一致：
    // 推进 2.9s 必越首 token 预算，且短于任何总预算地板（5s）。
    let budgetProbe: TimeInterval = 2.9

    // 1) 预算超时（fake 永不 complete）→ 首 token 预算触发 → 粘贴原文。
    var h = makeRefineHarness()
    let original = "这段文本足够长用来触发精修预算超时回退路径"
    h.refiner.behavior = .neverComplete
    h.asr.onFinal?(original)
    pumpMainRunLoop(0.05)
    checkEqual(h.refiner.callCount, 1, "refineBudget: 长文本进入精修")
    check(h.input.pastedTexts.isEmpty, "refineBudget: 预算未到期不粘贴")
    pumpMainRunLoop(budgetProbe)
    checkEqual(h.input.pastedTexts.last, original, "refineBudget: 预算超时粘贴原文")
    checkEqual(h.input.pastedTexts.count, 1, "refineBudget: 超时路径仅粘贴一次")

    // 2) 预算内完成（同步 complete）→ 粘贴精修结果（现行为回归）。
    h = makeRefineHarness()
    h.refiner.behavior = .completeImmediately("精修后的结果文本")
    h.asr.onFinal?(original)
    pumpMainRunLoop(0.2)
    checkEqual(h.input.pastedTexts.last, "精修后的结果文本", "refineBudget: 预算内完成粘贴精修结果")
    checkEqual(h.input.pastedTexts.count, 1, "refineBudget: 正常路径仅粘贴一次")

    // 3) 短文本跳过精修：11 字符不调用 refiner，12 字符边界进入精修。
    h = makeRefineHarness()
    h.refiner.behavior = .neverComplete
    h.asr.onFinal?(String(repeating: "短", count: 11))
    pumpMainRunLoop(0.1)
    checkEqual(h.refiner.callCount, 0, "refineBudget: 11 字符跳过精修")
    checkEqual(h.input.pastedTexts.last, String(repeating: "短", count: 11),
               "refineBudget: 短文本直接粘贴原文")

    h = makeRefineHarness()
    h.asr.onFinal?(String(repeating: "长", count: 12))
    pumpMainRunLoop(0.1)
    checkEqual(h.refiner.callCount, 1, "refineBudget: 12 字符边界进入精修")

    // 4) 预算超时后迟到的 complete 被 RefineCancelToken 守卫丢弃（不二次粘贴）。
    h = makeRefineHarness()
    h.refiner.behavior = .completeAfter(3.0, "迟到的精修结果")
    let original2 = "这段文本用来验证预算超时后迟到的精修结果会被守卫丢弃"
    h.asr.onFinal?(original2)
    pumpMainRunLoop(0.05)
    pumpMainRunLoop(budgetProbe) // ≈2.95s：首 token 预算（2.5s）已触发，迟到 complete（3.0s）未到期
    checkEqual(h.input.pastedTexts.last, original2, "refineBudget: 超时先粘贴原文")
    checkEqual(h.input.pastedTexts.count, 1, "refineBudget: 超时后仅一次粘贴")
    pumpMainRunLoop(0.6) // 越过 3.0s 迟到 complete
    checkEqual(h.input.pastedTexts.count, 1, "refineBudget: 迟到 complete 被守卫丢弃")

    // 5) P1 reasoning 活动喂活：思考链增量到达即失效首 token 预算——越过 2.5s
    // 不回退原文；总预算（5s 地板）仍 armed，最终兜底粘贴原文。
    h = makeRefineHarness()
    h.refiner.behavior = .activityThenNeverComplete
    h.asr.onFinal?(original)
    pumpMainRunLoop(0.05)
    pumpMainRunLoop(budgetProbe)
    check(h.input.pastedTexts.isEmpty,
          "refineBudget: 思考活动喂活后首 token 预算不误杀（2.9s 仍无粘贴）")
    pumpMainRunLoop(2.6) // ≈5.55s：越过总预算地板 5s
    checkEqual(h.input.pastedTexts.last, original,
               "refineBudget: 思考活动不豁免总预算（5.5s 兜底粘贴原文）")
    checkEqual(h.input.pastedTexts.count, 1, "refineBudget: 总预算路径仅粘贴一次")
}

// ---- 语音编辑指令：协调器接线（LastInjection / L0-L1 / 异步退格 / toast）----
// 方案 §5.6/§6/§8.1：命令轮不粘贴、不写历史；L0/L1 失败零注入；成功按计划发退格。
// 纯逻辑向量见 VoiceEditCommandTests；本组只验证接线与安全闸行为。

/// 可变焦点字段状态容器（闭包按引用读取，模拟 AX 读取结果变化）。
private final class FieldStateBox {
    var state: FocusedFieldState?
}

private func makeVoiceEditHarness() -> (coordinator: VoiceStickCoordinator, sink: FakeStatusSink,
                                        ble: FakeBleServing, input: FakeInput, asr: FakeASR,
                                        field: FieldStateBox) {
    let sink = FakeStatusSink()
    let fakeBle = FakeBleServing()
    let fakeInput = FakeInput()
    let asr = FakeASR()
    let coordinator = VoiceStickCoordinator(
        config: .defaults,
        statusController: sink,
        ble: fakeBle,
        makeAsr: { _ in asr },
        makeTranslator: { _ in FakeTranslator() },
        makeRefiner: { _ in FakeRefiner() },
        inputInjector: fakeInput,
        subtitleController: FakeSubtitle(),
        makeDebugRecorder: { _ in FakeRecorder() }
    )
    coordinator.userDataDirectoryOverride = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_voice_edit_userdata")
    coordinator.updateConfig(.defaults)
    coordinator.isProcessTrusted = { true }
    coordinator.frontmostAppBundleID = { "com.test.app" }
    // 恒等 trText：toast 文案 = L10nKey 名（String(format:) 对无 %d 的串原样返回），
    // 便于断言走到的文案 key，不依赖 App 模块的 Localization 表。
    coordinator.trText = { $0 }
    let field = FieldStateBox()
    coordinator.readFocusedFieldState = { field.state }
    return (coordinator, sink, fakeBle, fakeInput, asr, field)
}

private func voiceEditBackspaces(_ count: Int) -> [KeySpec] {
    Array(repeating: KeySpec.parse("backspace")!, count: count)
}

func runVoiceEditCoordinatorTests() {
    var h = makeVoiceEditHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    checkEqual(h.input.pastedTexts.count, 1, "voiceEdit: 普通输入注入一次")
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 普通输入不注入退格")

    // U1：field "你好世界\n" + 光标末尾 → 2 字 + 1 换行 = 3 次退格；命令轮不粘贴。
    h.field.state = FocusedFieldState(text: "你好世界\n", caretUTF16: 5, selectionLength: 0)
    h.asr.onFinal?("删掉最后两个字。")
    pumpMainRunLoop(0.5)
    checkEqual(h.input.pastedTexts.count, 1, "voiceEdit: 命令轮不产生粘贴")
    checkEqual(h.input.sentKeyCombos, voiceEditBackspaces(3),
               "voiceEdit: U1 退格 3 次（2 字 + 换行）")
    check(h.sink.timedMessages.contains("voiceEditDeletedChars"), "voiceEdit: 成功 toast key")
    check(h.coordinator.fsmSnapshot.contains("main=ready"), "voiceEdit: 命令后回 ready")

    // 记录更新：remainingText 变为 "你好"、pressedEnter=false；再删 1 字 → 1 次退格。
    h.field.state = FocusedFieldState(text: "你好", caretUTF16: 2, selectionLength: 0)
    h.input.sentKeyCombos.removeAll()
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.5)
    checkEqual(h.input.sentKeyCombos, voiceEditBackspaces(1),
               "voiceEdit: 删除后 remainingText 更新 → 1 次退格")

    // 无注入记录：命令轮吞掉不粘贴、零注入，toast no_injection。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.2)
    checkEqual(h.input.pastedTexts.count, 0, "voiceEdit: 无目标命令不粘贴")
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 无目标零注入")
    checkEqual(h.sink.timedMessages.last, "voiceEditNoInjection", "voiceEdit: 无目标 toast key")

    // 前台应用变化 → app_changed，零注入。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.coordinator.frontmostAppBundleID = { "com.other.app" }
    h.field.state = FocusedFieldState(text: "你好世界\n", caretUTF16: 5, selectionLength: 0)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 切应用零注入")
    checkEqual(h.sink.timedMessages.last, "voiceEditAppChanged", "voiceEdit: 切应用 toast key")

    // auto + 不可验证 + 窗口关闭（unverified_seconds=0）→ verify_unavailable，不盲删。
    h = makeVoiceEditHarness()
    var closedWindowConfig = AppConfig.defaults
    closedWindowConfig.voiceEdit.unverifiedSeconds = 0
    h.coordinator.updateConfig(closedWindowConfig)
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.field.state = nil
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 不可验证且窗口关闭零注入")
    checkEqual(h.sink.timedMessages.last, "voiceEditVerifyUnavailable",
               "voiceEdit: 不可验证 toast key")

    // auto + 不可验证 + 8s 窗口内 → unverified 盲删（1 字 + 换行 = 2 次退格）。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.field.state = nil
    h.input.sentKeyCombos.removeAll()
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.5)
    checkEqual(h.input.sentKeyCombos.count, 2, "voiceEdit: auto 窗口内盲删 2 次退格")
    check(h.sink.timedMessages.last?.contains("voiceEditDeletedChars") == true,
          "voiceEdit: 盲删成功 toast key")
    check(h.sink.timedMessages.last?.contains("voiceEditUnverifiedSuffix") == true,
          "voiceEdit: 盲删标注未校验")

    // 用户追加手打内容（字段尾部不匹配）→ field_mismatch，零注入。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.field.state = FocusedFieldState(text: "你好世界\n吗", caretUTF16: 6, selectionLength: 0)
    h.asr.onFinal?("删除最后两个字")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 字段不匹配零注入")
    checkEqual(h.sink.timedMessages.last, "voiceEditFieldMismatch",
               "voiceEdit: 字段不匹配 toast key")

    // 光标在中间 → caret_not_end；选区非空 → selection_present。
    h.field.state = FocusedFieldState(text: "你好世界", caretUTF16: 2, selectionLength: 0)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.2)
    checkEqual(h.sink.timedMessages.last, "voiceEditCaretNotEnd", "voiceEdit: 光标不在末尾 toast key")
    h.field.state = FocusedFieldState(text: "你好世界", caretUTF16: 0, selectionLength: 4)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.2)
    checkEqual(h.sink.timedMessages.last, "voiceEditSelectionPresent",
               "voiceEdit: 选区存在 toast key")

    // 字幕模式不拦截：命令按普通文本走字幕链路（不粘贴、不注入退格）。
    h = makeVoiceEditHarness()
    var subtitleConfig = AppConfig.defaults
    subtitleConfig.defaultOutputProfile.target = .subtitle
    h.coordinator.updateConfig(subtitleConfig)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.2)
    checkEqual(h.input.pastedTexts.count, 0, "voiceEdit: 字幕模式不粘贴")
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 字幕模式零退格")

    // 功能关闭：命令文本按普通文本正常上屏（完全回到现状）。
    h = makeVoiceEditHarness()
    var disabledConfig = AppConfig.defaults
    disabledConfig.voiceEdit.enabled = false
    h.coordinator.updateConfig(disabledConfig)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.2)
    checkEqual(h.input.pastedTexts.last, "删除", "voiceEdit: 关闭后命令文本正常上屏")

    // 普通文本回归（含「删除」字样但不构成指令）→ 正常粘贴、不注入退格。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("请把删除按钮改成红色")
    pumpMainRunLoop(0.2)
    checkEqual(h.input.pastedTexts.last, "请把删除按钮改成红色", "voiceEdit: 普通文本回归")
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 普通文本零退格")

    // 辅助功能权限缺失 → accessibility_denied，零注入。
    h = makeVoiceEditHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.coordinator.isProcessTrusted = { false }
    h.field.state = FocusedFieldState(text: "你好世界\n", caretUTF16: 5, selectionLength: 0)
    h.asr.onFinal?("删除")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.sentKeyCombos.count, 0, "voiceEdit: 无辅助功能权限零注入")
    checkEqual(h.sink.timedMessages.last, "voiceEditAccessibilityDenied",
               "voiceEdit: 无权限 toast key")
}

// ---- targeted 语音编辑：协调器接线（选区执行/读回确认/终验/代际防 stale）----
// Doc/Plan/voice-edit-commands.md targeted 章节：完整调用顺序 select → selected-text
// verify → 一次 delete 或 paste(false) → restore end → final verify → update；
// 命令轮不粘贴指令本身、不 refine、不写历史；任何 verify_mode 下都必须 verified。

/// 模拟焦点应用字段（含选区/光标）：AX/注入副作用都落在 box 上，供终验读回。
private final class TargetedFieldBox {
    var text: String = ""
    /// 当前选区（UTF-16 偏移；caret = location+length 折叠语义由 reader 给出）。
    var selectionLocation: Int = 0
    var selectionLength: Int = 0

    var state: FocusedFieldState? {
        FocusedFieldState(
            text: text,
            caretUTF16: selectionLocation + selectionLength,
            selectionLength: selectionLength
        )
    }

    var selectedText: String {
        guard selectionLength > 0 else { return "" }
        let units = Array(text.utf16)
        guard selectionLocation + selectionLength <= units.count else { return "" }
        return String(
            decoding: units[selectionLocation..<(selectionLocation + selectionLength)],
            as: UTF16.self
        )
    }

    /// 模拟退格：选区非空删选区，否则删最后一个 grapheme。
    func applyBackspace() {
        if selectionLength > 0 {
            text = Self.replaceUtf16(text, selectionLocation, selectionLength, with: "")
        } else {
            text = String(text.dropLast())
        }
        selectionLength = 0
    }

    /// 模拟粘贴：替换选区并移动光标到插入文本之后（pressEnter 另起一行）。
    func applyPaste(_ pasted: String, pressEnter: Bool) {
        text = Self.replaceUtf16(text, selectionLocation, selectionLength, with: pasted)
        selectionLocation += pasted.utf16.count
        selectionLength = 0
        if pressEnter {
            text += "\n"
            selectionLocation += 1
        }
    }

    static func replaceUtf16(_ text: String, _ location: Int, _ length: Int,
                             with replacement: String) -> String {
        var units = Array(text.utf16)
        guard location >= 0, length >= 0, location + length <= units.count else { return text }
        units.replaceSubrange(location..<(location + length), with: Array(replacement.utf16))
        return String(decoding: units, as: UTF16.self)
    }
}

private func makeTargetedHarness() -> (coordinator: VoiceStickCoordinator, sink: FakeStatusSink,
                                       ble: FakeBleServing, input: FakeInput, asr: FakeASR,
                                       refiner: FakeRefiner, field: TargetedFieldBox,
                                       ops: TargetedOps) {
    let sink = FakeStatusSink()
    let fakeBle = FakeBleServing()
    let fakeInput = FakeInput()
    let asr = FakeASR()
    let refiner = FakeRefiner()
    let coordinator = VoiceStickCoordinator(
        config: .defaults,
        statusController: sink,
        ble: fakeBle,
        makeAsr: { _ in asr },
        makeTranslator: { _ in FakeTranslator() },
        makeRefiner: { _ in refiner },
        inputInjector: fakeInput,
        subtitleController: FakeSubtitle(),
        makeDebugRecorder: { _ in FakeRecorder() }
    )
    coordinator.userDataDirectoryOverride = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_targeted_userdata")
    coordinator.updateConfig(.defaults)
    coordinator.isProcessTrusted = { true }
    coordinator.frontmostAppBundleID = { "com.test.app" }
    coordinator.trText = { $0 }
    let field = TargetedFieldBox()
    let ops = TargetedOps()
    coordinator.readFocusedFieldState = { [weak field] in
        ops.record("state")
        return field?.state
    }
    coordinator.writeTargetedSelection = { [weak field] location, length in
        ops.record("select(\(location),\(length))")
        field?.selectionLocation = location
        field?.selectionLength = length
        return true
    }
    coordinator.readTargetedSelectedText = { [weak field] in
        ops.record("selected")
        return field?.selectedText
    }
    fakeInput.onPaste = { [weak field] text, pressEnter in
        ops.record("paste")
        field?.applyPaste(text, pressEnter: pressEnter)
    }
    fakeInput.onSendKeyCombo = { spec in
        ops.record("key:\(spec.displayText)")
        if spec.displayText == "Backspace" || spec.displayText == "backspace" {
            field.applyBackspace()
        }
    }
    return (coordinator, sink, fakeBle, fakeInput, asr, refiner, field, ops)
}

/// 平台调用顺序记录（select/selected/paste/key/state）。
private final class TargetedOps {
    private(set) var list: [String] = []
    func record(_ op: String) { list.append(op) }
    func clear() { list.removeAll() }
}

func runVoiceTargetedCoordinatorTests() {
    // 基本删除：注入「我去北京上班」→ 「删除刚才的北京」→ 选区(2,2) → 读回=目标
    // → 一次 Backspace → 恢复末尾(4,0) → 终验 → remainingText 更新。
    var h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    checkEqual(h.input.pastedTexts.count, 1, "targeted: 普通输入注入一次")
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.6)
    checkEqual(h.input.pastedTexts.count, 1, "targeted: 命令轮不粘贴指令本身")
    checkEqual(h.ops.list,
               ["paste", "state", "select(2,2)", "selected", "key:Backspace", "select(4,0)", "state"],
               "targeted: 删除完整调用顺序")
    checkEqual(h.sink.timedMessages.last, "voiceTargetedDeleted", "targeted: 删除成功 toast key")
    check(h.coordinator.fsmSnapshot.contains("main=ready"), "targeted: 命令后回 ready")

    // 记录更新 → 连续第二次编辑可用：删「上班」→ 字段「我去」。
    h.field.text = "我去上班"
    h.field.selectionLocation = 4
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("删除刚才的上班")
    pumpMainRunLoop(0.6)
    checkEqual(h.ops.list,
               ["state", "select(2,2)", "selected", "key:Backspace", "select(2,0)", "state"],
               "targeted: 连续编辑调用顺序（remainingText 已更新）")
    checkEqual(h.field.text, "我去", "targeted: 连续编辑后字段内容")

    // 替换：paste(pressEnter=false) + 终验新文本。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("把刚才的北京改成上海")
    pumpMainRunLoop(0.6)
    checkEqual(h.ops.list,
               ["state", "select(2,2)", "selected", "paste", "select(6,0)", "state"],
               "targeted: 替换完整调用顺序")
    checkEqual(h.input.pastedTexts.last, "上海", "targeted: 替换粘贴内容")
    checkEqual(h.input.pastedEnterFlags.last, false, "targeted: 替换 pressEnter=false")
    checkEqual(h.field.text, "我去上海上班", "targeted: 替换后字段内容")
    checkEqual(h.sink.timedMessages.last, "voiceTargetedReplaced", "targeted: 替换成功 toast key")

    // auto_enter 换行保留：字段「你好世界\n」，删「世界」→ 终验「你好\n」，
    // pressedEnter 保持（记录仍含换行语义）。
    h = makeTargetedHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.field.text = "你好世界\n"
    h.field.selectionLocation = 5   // 末尾换行之后（auto_enter 光标位；"你好世界\n" = 5 UTF-16）
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("删除刚才的世界")
    pumpMainRunLoop(0.6)
    checkEqual(h.sink.timedMessages.last, "voiceTargetedDeleted", "targeted: 换行场景成功 toast")
    checkEqual(h.field.text, "你好\n", "targeted: auto_enter 换行保留")
    checkEqual(h.ops.list,
               ["state", "select(2,2)", "selected", "key:Backspace", "select(3,0)", "state"],
               "targeted: 换行场景调用顺序")

    // remaining 自身以换行结尾（口述「你好\n」+ auto_enter → 字段「你好\n\n」）：
    // 删「你好」→ 终验字段「\n\n」、remaining 为「\n」、原 auto_enter 换行保留。
    h = makeTargetedHarness()
    h.asr.onFinal?("你好\n")
    pumpMainRunLoop(0.1)
    h.field.text = "你好\n\n"
    h.field.selectionLocation = 4
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("删除刚才的你好")
    pumpMainRunLoop(0.6)
    checkEqual(h.field.text, "\n\n", "targeted: remaining 换行结尾 → 终验字段 \n\n")
    checkEqual(h.ops.list,
               ["state", "select(0,2)", "selected", "key:Backspace", "select(2,0)", "state"],
               "targeted: remaining 换行结尾调用顺序")
    // 注：remaining 更新为「\n」后，targeted 无法再以换行为目标（P0 单行约束），
    // 原 auto_enter 换行语义由记录中的 pressedEnter=true 保留。

    // updateConfig 关闭 voiceEdit 清记录：在途 0.2s 终验（旧 generation）不得弹
    // 旧成功 toast、不得更新已被清掉的记录。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.1)   // backspace 已发出，终验（0.2s）未触发
    var disabledConfig = AppConfig.defaults
    disabledConfig.voiceEdit.enabled = false
    h.coordinator.updateConfig(disabledConfig)
    pumpMainRunLoop(0.6)
    check(!h.sink.timedMessages.contains("voiceTargetedDeleted"),
          "targeted: disable 后无旧成功 toast")

    // 选区读回不符：零删除/零粘贴 + 恢复末尾光标 + fieldMismatch + 记录保留。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    // 模拟平台读回漂移：读回内容 ≠ target。
    let opsRef = h.ops
    let fieldBox = h.field
    var drifted = false
    h.coordinator.readTargetedSelectedText = { [weak fieldBox] in
        opsRef.record("selected")
        return drifted ? "北" : fieldBox?.selectedText
    }
    drifted = true
    h.ops.clear()
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.6)
    checkEqual(h.ops.list, ["state", "select(2,2)", "selected", "select(6,0)"],
               "targeted: 读回不符→恢复末尾光标，无 delete/paste")
    checkEqual(h.field.text, "我去北京上班", "targeted: 读回不符零文本修改")
    checkEqual(h.sink.timedMessages.last, "voiceTargetedTextChanged",
               "targeted: 读回不符 toast key")
    // 记录保留：字段未变时同命令可再执行（drift 关闭后成功）。
    drifted = false
    h.ops.clear()
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.6)
    checkEqual(h.ops.list,
               ["state", "select(2,2)", "selected", "key:Backspace", "select(4,0)", "state"],
               "targeted: 修改前失败后记录仍有效")

    // 终验失败（模拟应用未应用退格）：不重试、记录失效、提示检查。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    let fieldRef = h.field
    h.input.onSendKeyCombo = { spec in
        h.ops.record("key:\(spec.displayText)")
        if spec.displayText == "Backspace" || spec.displayText == "backspace" {
            fieldRef.selectionLength = 0  // 吞掉注入：字段保持原样
        }
    }
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.6)
    checkEqual(h.sink.timedMessages.last, "voiceTargetedUnconfirmed",
               "targeted: 终验失败 toast key")
    // 记录已失效：再发 targeted 命令 → no_injection。
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceEditNoInjection",
               "targeted: 终验失败后记录失效")

    // stale generation：命令执行后、终验前发生新注入 → 旧回调不得覆盖新记录。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.1)   // 已执行 backspace（0.2s 终验未到）
    h.field.text = "全新输入"
    h.field.selectionLocation = 4
    h.field.selectionLength = 0
    h.asr.onFinal?("全新输入")
    pumpMainRunLoop(0.6)
    checkEqual(h.input.pastedTexts.last, "全新输入", "targeted: stale 期间新注入正常粘贴")
    check(!h.sink.timedMessages.contains("voiceTargetedDeleted"),
          "targeted: stale 回调不弹旧成功 toast")
    // 新记录未被旧 newRemainingText 覆盖：基于「全新输入」再编辑可用。
    h.field.text = "全新输入"
    h.field.selectionLocation = 4
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("删除刚才的全新")
    pumpMainRunLoop(0.6)
    checkEqual(h.ops.list,
               ["state", "select(0,2)", "selected", "key:Backspace", "select(2,0)", "state"],
               "targeted: 新注入记录未被旧回调污染")

    // verify_mode=off 且字段不可读：targeted 恒 verified → 拒绝（严禁盲改）。
    h = makeTargetedHarness()
    var offConfig = AppConfig.defaults
    offConfig.voiceEdit.verifyMode = .off
    h.coordinator.updateConfig(offConfig)
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.coordinator.readFocusedFieldState = { nil }   // 模拟不可读应用
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.ops.list.filter { $0.hasPrefix("key:") }.count, 0, "targeted: off 模式仍零盲改")
    checkEqual(h.sink.timedMessages.last, "voiceEditVerifyUnavailable",
               "targeted: off 模式不可验证 toast key")

    // L0/L1 拒绝路径：notFound / ambiguous / caret 移开 / 选区非空 / 字段被改。
    // 歧义需要 remainingText 本身多处匹配：注入「北京和北京」。
    h = makeTargetedHarness()
    h.asr.onFinal?("北京和北京")
    pumpMainRunLoop(0.1)
    h.field.text = "北京和北京"
    h.field.selectionLocation = 5
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的上海")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceTargetedNotFound", "targeted: 未找到 toast key")
    checkEqual(h.input.sentKeyCombos.count, 0, "targeted: 未找到零注入")
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceTargetedAmbiguous", "targeted: 歧义 toast key")
    // 指定「最后一个」→ 执行成功（消歧路径）。
    h.ops.clear()
    h.asr.onFinal?("删除刚才最后一个北京")
    pumpMainRunLoop(0.6)
    checkEqual(h.field.text, "北京和", "targeted: 最后一个消歧执行")
    h.field.text = "北京和"
    h.field.selectionLocation = 1   // 光标移开
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceEditCaretNotEnd", "targeted: 光标移开 toast key")
    h.field.text = "北京和"
    h.field.selectionLocation = 0
    h.field.selectionLength = 3     // 已有选区
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceEditSelectionPresent",
               "targeted: 选区非空 toast key")
    // 拒绝路径零注入（notFound/ambiguous/caret/selection 全部零；消歧成功 1 次）。
    checkEqual(h.input.sentKeyCombos.count, 1, "targeted: 拒绝路径零注入（仅消歧成功 1 次）")

    // 手工改变 remaining（旧最长前缀容错不得生效）→ fieldMismatch 类拒绝。
    h = makeTargetedHarness()
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北上班"   // 用户手工删过
    h.field.selectionLocation = 5
    h.field.selectionLength = 0
    h.asr.onFinal?("删除刚才的北京")
    pumpMainRunLoop(0.3)
    checkEqual(h.sink.timedMessages.last, "voiceTargetedTextChanged",
               "targeted: 手工改动 fieldMismatch toast key")

    // 无作用域简写 → 普通文本上屏；invalid 结构 → 吞掉提示不粘贴。
    h = makeTargetedHarness()
    h.asr.onFinal?("把北京改成上海")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.pastedTexts.last, "把北京改成上海", "targeted: 无作用域按普通文本")
    h.asr.onFinal?("我去北京上班")
    pumpMainRunLoop(0.1)
    h.field.text = "我去北京上班"
    h.field.selectionLocation = 6
    h.field.selectionLength = 0
    h.asr.onFinal?("把刚才的改成上海")
    pumpMainRunLoop(0.3)
    checkEqual(h.input.pastedTexts.last, "我去北京上班", "targeted: invalid 不粘贴指令")
    checkEqual(h.sink.timedMessages.last, "voiceTargetedInvalid", "targeted: invalid toast key")

    // 旧解析器优先：「删除刚才的」仍走旧整段删除（退格×N，非 targeted 选区）。
    h = makeTargetedHarness()
    h.asr.onFinal?("你好世界")
    pumpMainRunLoop(0.1)
    h.field.text = "你好世界\n"
    h.field.selectionLocation = 5
    h.field.selectionLength = 0
    h.ops.clear()
    h.asr.onFinal?("删除刚才的")
    pumpMainRunLoop(0.5)
    checkEqual(h.input.sentKeyCombos.count, 5, "targeted: 旧整段删除退格 5 次（4 字+换行）")
    check(!h.ops.list.contains("selected"), "targeted: 旧路径不走 targeted 选区")

    // 命令轮不 refine、不写历史（refiner 零调用）。
    checkEqual(h.refiner.callCount, 0, "targeted: 命令轮不进入精修")
}
