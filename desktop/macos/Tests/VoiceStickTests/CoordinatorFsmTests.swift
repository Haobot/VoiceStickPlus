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
    func showFinal(_ text: String, deviceID: String?, onHidden: (() -> Void)?) {}
    func showTimedMessage(_ text: String, duration: TimeInterval, deviceID: String?,
                          onHidden: (() -> Void)?) {}
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
    func paste(text: String, pressEnter: Bool) {}
    func clickLeftButton() {}
    func moveMouse(dx: Int, dy: Int) {}
    func sendArrowDown() {}
    func sendArrowUp() {}
    func sendEnter() {}
    /// gateway_key 注入断言用：记录收到的组合键。
    var sentKeyCombos: [KeySpec] = []
    func sendKeyCombo(_ spec: KeySpec) { sentKeyCombos.append(spec) }
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
    func refineStream(_ text: String, promptOverride: String, hotwords: [String]?,
                      cancel: RefineCancelToken,
                      onToken: @escaping (String) -> Void,
                      onComplete: @escaping (Bool, String) -> Void) {}
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
    check(hotPushes.contains("gateway_keymap_set up=passthrough"),
          "gwPush: 未映射键热更下发直通")

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
    check(coordinator.fsmSnapshot.contains("main=ready"), "gw: 消费链后状态机仍一致")
}
