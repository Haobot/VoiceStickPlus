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
    func setFirmwareInfo(_ infoByDeviceID: [String: DeviceFirmwareInfo]) {}
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
    func connectedStickDeviceIDs() -> [String] { [] }
    func deviceClass(for peripheralID: UUID) -> DeviceClass { .stickS3 }
    func deviceID(for peripheralID: UUID) -> String? { nil }
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
    func sendStickControlPayload(_ data: Data, label: String, deviceID: String) {}
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
    func sendKeyCombo(_ spec: KeySpec) {}
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
    let coordinator = VoiceStickCoordinator(
        config: .defaults,
        statusController: sink,
        ble: fakeBle,
        makeAsr: { _ in FakeASR() },
        makeTranslator: { _ in FakeTranslator() },
        makeRefiner: { _ in FakeRefiner() },
        inputInjector: FakeInput(),
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
}
