import AppKit
import Foundation
import VoiceStickCore

final class VoiceStickCoordinator {
    private enum PendingPasteState {
        case idle
        case waitingToPaste(text: String)
        case paused(text: String)

        var isIdle: Bool {
            if case .idle = self {
                return true
            }
            return false
        }
    }

    private enum MainInputState {
        case ready
        case recording(sessionID: UInt32, peripheralID: UUID, startedAt: Date)
        case finalizing(sessionID: UInt32, peripheralID: UUID, startedAt: Date?)
        case pendingConfirmation(peripheralID: UUID)
        case pausedConfirmation(peripheralID: UUID)
        case error(peripheralID: UUID?)

        var sessionID: UInt32? {
            switch self {
            case .recording(let sessionID, _, _), .finalizing(let sessionID, _, _):
                return sessionID
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var peripheralID: UUID? {
            switch self {
            case .recording(_, let peripheralID, _),
                 .finalizing(_, let peripheralID, _),
                 .pendingConfirmation(let peripheralID),
                 .pausedConfirmation(let peripheralID):
                return peripheralID
            case .error(let peripheralID):
                return peripheralID
            case .ready:
                return nil
            }
        }

        var startedAt: Date? {
            switch self {
            case .recording(_, _, let startedAt):
                return startedAt
            case .finalizing(_, _, let startedAt):
                return startedAt
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var isRecording: Bool {
            if case .recording = self {
                return true
            }
            return false
        }

        var isFinalizing: Bool {
            if case .finalizing = self {
                return true
            }
            return false
        }

        var isBusy: Bool {
            if case .ready = self {
                return false
            }
            return true
        }
    }

    private final class SubtitleCycle {
        let peripheralID: UUID
        let deviceID: String?
        let sessionID: UInt32
        let startedAt: Date
        let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
        let debugAudioRecorder: DebugAudioRecorder
        var asr: any ASRClient
        var receivedAudioFrames = 0
        var bufferedOggChunks: [Data] = []
        var asrStarted = false
        var sentFinalAudioChunk = false
        var finishedFinalText = false
        var waitingForAudioEnd = false
        var audioEndTimeoutTimer: Timer?

        init(peripheralID: UUID, deviceID: String?, sessionID: UInt32, config: AppConfig) {
            self.peripheralID = peripheralID
            self.deviceID = deviceID
            self.sessionID = sessionID
            self.startedAt = Date()
            self.asr = ASRClientFactory.makeClient(config: config)
            self.debugAudioRecorder = DebugAudioRecorder(
                enabled: config.debugAudioCache,
                directory: config.debugAudioDirectory
            )
        }

        var duration: TimeInterval {
            Date().timeIntervalSince(startedAt)
        }

        deinit {
            audioEndTimeoutTimer?.invalidate()
        }
    }

    private struct SubtitleCycleKey: Hashable {
        let peripheralID: UUID
        let sessionID: UInt32
    }

    private var config: AppConfig
    private let statusController: StatusController
    private let ble: BleCentral
    private var asr: any ASRClient
    private var translator: LLMTranslationClient
    private var refiner: LLMRefinementClient
    /// 前台应用追踪（三期按键映射按前台应用切换的注入点；AppDelegate 启动时装配，
    /// v1 协调器暂不消费，app 级覆盖落地后由按键映射 resolve 使用）。
    var frontmostAppProvider: FrontmostAppProvider?
    private let subtitleController = SubtitleController()
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    private let inputInjector = InputInjector()
    private let firmwareManifestClient = FirmwareManifestClient()
    private var debugAudioRecorder: DebugAudioRecorder
    private let minimumRecordingDuration: TimeInterval = 0.5
    private let audioEndTimeout: TimeInterval = 1.0
    private let firmwareManifestCacheDuration: TimeInterval = 24 * 60 * 60

    private var mainInputState = MainInputState.ready
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
    private var waitingForAudioEnd = false
    private var audioEndTimeoutTimer: Timer?
    private var pendingPasteState = PendingPasteState.idle
    private var lastRecoverableText: String?
    private var lastRecoverablePeripheralID: UUID?
    private var pairedDeviceIDs: [String]
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    private var latestFirmwareManifest: FirmwareManifest?
    private var lastFirmwareManifestCheckAt: Date?
    private var firmwareManifestCheckInFlight = false
    private var firmwareManifestRefreshTimer: Timer?
    private var pendingFirmwareUpdatePromptDeviceIDs: Set<String> = []
    private var errorRecoveryToken = 0
    private var isShowingASRError = false
    private var subtitleCycles: [SubtitleCycleKey: SubtitleCycle] = [:]
    private var activeSubtitleSessions: [UUID: UInt32] = [:]

    // MARK: 编码器/敲击交互状态（对齐 Windows 协调器同名字段组）

    /// tap 注入节流锚点（两次方向键注入最短间隔 500ms）。
    private var lastTapInjectAt: Date?
    /// 编码器旋转快慢分档测速估计器（EWMA，全局单例；换设备时重置冷启动）。
    private var encoderSpeedEstimator = EncoderRotateSpeedEstimator()
    private var lastEncoderRotateDeviceID: String?
    /// 快速甩动注入一次后进入停转锁定，直到静默 250ms 停稳才恢复识别。
    private var encoderRotateLockout = false
    private var lastEncoderRotateEventAt: Date?
    /// 慢速旋转延迟判定挂起段（decide_window 内累计，到期/换向/加速冲刷）。
    private var encoderPendingActive = false
    private var encoderPendingCCW = false
    private var encoderPendingSteps: UInt32 = 0
    private var encoderPendingStartedAt = Date.distantPast
    private var encoderPendingPeripheralID: UUID?
    /// decide_window 到期冲刷定时器（pending 激活时 30ms 周期运行）。
    private var encoderRotateTimer: Timer?
    var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?
    /// 注入路径发现无辅助功能权限（AppDelegate 接此回调弹引导窗；悬浮窗提示由协调器自带节流）。
    var onAccessibilityPermissionMissing: (() -> Void)?
    /// power_log 分片 / power_mgmt 事件（deviceID 寻址；AppDelegate 转发到电量监测窗口）。
    var onPowerLogFragment: ((String, PowerLogFragment) -> Void)?
    var onPowerMgmtEvent: ((String, PowerMgmtEvent) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = ASRClientFactory.makeClient(config: config)
        self.translator = LLMTranslationClient(config: config)
        self.refiner = LLMRefinementClient(config: config)
        self.debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
        // 小米遥控器接入：paired_device 条目表注入（实时读最新 config，配对/遗忘
        // 即时生效）；同款注入方式见 BleCentral.pairedDevicesProvider 注释。
        ble.pairedDevicesProvider = { [weak self] in self?.config.pairedDevices ?? [] }
        // 注入权限兜底：无辅助功能权限时 CGEvent 被静默丢弃，转悬浮窗提示 + 引导弹窗。
        inputInjector.onAccessibilityPermissionMissing = { [weak self] in
            self?.handleAccessibilityPermissionMissing()
        }
    }

    /// 透传：按 RC deviceID 解析 ATVV 会话参数（见 BleCentral.xiaomiOptionsResolver）。
    func setXiaomiOptionsResolver(_ resolver: @escaping (String) -> XiaomiAtvvSession.Options) {
        ble.xiaomiOptionsResolver = resolver
    }

    /// 透传：F5 抑制锚点（AppDelegate 事件钩子读取；写入在 BleCentral 回调线程）。
    var xiaomiMicOpenAnchor: XiaomiMicOpenAnchor {
        ble.micOpenAnchor
    }

    /// 透传：BLE 连接集变化（AppDelegate 的 F5 suppressor 门控挂在上面）。
    var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?

    func start() {
        ble.onConnectionChange = { [weak self] connectedDevices in
            guard let self else { return }
            self.statusController.setConnectedDevices(connectedDevices)
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            if !connectedDevices.isEmpty {
                self.statusController.setStatus("Ready")
                self.ble.sendInteractionMode(self.config.interactionMode)
                self.ble.sendShowIMUDebug(self.config.showIMUDebug)
                // 设备交互/编码器设置按设备覆盖：逐台取其有效配置单播（无覆盖设备收到
                // 全局默认值，与旧广播行为等价）。小米遥控器无 IMU/敲击/编码器硬件，跳过
                // （对齐 Windows 连接回调；BLE 层另有按类门控兜底）。
                for device in connectedDevices where device.deviceClass == .stickS3 {
                    self.sendDeviceInteractionSettings(deviceID: device.deviceID)
                }
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
            self.onConnectionChange?(connectedDevices)
        }

        ble.onStateEvent = { [weak self] peripheralID, event in
            self?.handleStateEvent(event, peripheralID: peripheralID)
        }

        // power_log 分片与 power_mgmt 事件透传（电量监测窗口经 AppDelegate 消费；
        // 窗口未开时丢弃，对齐 Windows）。
        ble.onPowerLogFragment = { [weak self] peripheralID, fragment in
            guard let self, let deviceID = self.deviceID(for: peripheralID) else { return }
            self.onPowerLogFragment?(deviceID, fragment)
        }
        ble.onPowerMgmtEvent = { [weak self] peripheralID, event in
            guard let self, let deviceID = self.deviceID(for: peripheralID) else { return }
            self.onPowerMgmtEvent?(deviceID, event)
        }

        ble.onAudioFrame = { [weak self] peripheralID, frame in
            self?.handleAudioFrame(frame, peripheralID: peripheralID)
        }

        configureASRCallbacks()
        ble.start()
        checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        startFirmwareManifestRefreshTimer()
    }

    deinit {
        audioEndTimeoutTimer?.invalidate()
        firmwareManifestRefreshTimer?.invalidate()
        encoderRotateTimer?.invalidate()
    }

    func updateConfig(_ config: AppConfig) {
        let wasRecognizing = asrStarted || mainInputState.isBusy || isWaitingForFinalText || !subtitleCycles.isEmpty
        if wasRecognizing {
            asr.onPartial = nil
            asr.onSegment = nil
            asr.onFinal = nil
            asr.onError = nil
            asr.onUpgradeURL = nil
            asr.cancel()
            for cycle in subtitleCycles.values {
                cycle.asr.cancel()
                cycle.debugAudioRecorder.discard()
            }
            subtitleCycles.removeAll()
            activeSubtitleSessions.removeAll()
            mainInputState = .ready
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
            subtitleController.hideAll()
            sendUIStateForActiveDevice("ready")
            finishRecognitionCycle()
        }

        self.config = config
        ble.sendInteractionMode(config.interactionMode)
        ble.sendShowIMUDebug(config.showIMUDebug)
        // 编码器/交互设置按设备覆盖：对已连接 StickS3 逐台取其有效配置单播
        // （对齐 Windows UpdateConfig；小米遥控器跳过）。
        for deviceID in ble.connectedStickDeviceIDs() {
            sendDeviceInteractionSettings(deviceID: deviceID)
        }
        debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
        asr = ASRClientFactory.makeClient(config: config)
        translator = LLMTranslationClient(config: config)
        refiner = LLMRefinementClient(config: config)
        configureASRCallbacks()

        if pairedDeviceIDs != config.pairedDeviceIDs {
            updatePairedDeviceIDs(config.pairedDeviceIDs)
        } else if isShowingASRError {
            recoverFromASRError()
        } else if wasRecognizing {
            statusController.setStatus("Ready")
        }
    }

    private func configureASRCallbacks() {
        asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
                guard let self else { return }
                self.statusController.showPartial(text, deviceID: self.activeDeviceID)
                if self.shouldSendPartialToDevice() {
                    self.sendUIStateForActiveDevice("thinking", text: text)
                }
            }
        }

        asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                self?.handleDefiniteSegment(segment)
            }
        }

        asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishWithFinalText(text)
            }
        }

        asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishWithASRError(message)
            }
        }

        asr.onUpgradeURL = { [weak self] url, message in
            DispatchQueue.main.async {
                self?.presentASRUpgradeAlert(url: url, message: message)
            }
        }
    }

    private func configureSubtitleASRCallbacks(for cycle: SubtitleCycle) {
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
                guard
                    let self,
                    let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                    self.canUpdateOverlayForSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
                else { return }
                self.statusController.showPartial(text, deviceID: cycle.deviceID)
                if self.shouldSendSubtitlePartialToDevice(cycle) {
                    self.ble.sendUIState("thinking", text: text, to: peripheralID)
                }
            }
        }
        cycle.asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                guard self?.isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) == true else {
                    return
                }
                self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
            }
        }
        cycle.asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithFinalText(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    text: text
                )
            }
        }
        cycle.asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithError(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    message: message
                )
            }
        }
        cycle.asr.onUpgradeURL = { url, _ in
            DispatchQueue.main.async {
                NSWorkspace.shared.open(url)
            }
        }
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        // 配对/遗忘由 AppDelegate 先落盘；此处从磁盘刷新配对条目，
        // 否则 ble.pairedDevicesProvider 读到的是启动时的旧快照，
        // RC 设备（靠 paired_device 条目反查 UUID 重连）在重启前永远连不上。
        config.pairedDevices = AppConfig.load().pairedDevices
        statusController.setPairedDeviceIDs(deviceIDs)
        statusController.setConnectedDevices([])
        statusController.setStatus(deviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
        ble.updatePairedDeviceIDs(deviceIDs)
    }

    func updateFirmware(from url: URL, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        do {
            let image = try Data(contentsOf: url)
            ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                DispatchQueue.main.async {
                    completion(result)
                }
            }
        } catch {
            completion(.failure(error))
        }
    }

    func cancelFirmwareUpdate() {
        ble.cancelFirmwareUpdate()
    }

    /// power_log 命令下发（电量监测窗口；对齐 Windows VoiceStickCoordinator::SendPowerLogCommand）。
    func sendPowerLogCommand(deviceID: String, payload: Data) {
        ble.sendPowerLogCommand(payload, to: deviceID)
    }

    func checkFirmwareUpdatesNow() {
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: true)
    }

    func checkFirmwareAfterPairing(deviceID: String) {
        pendingFirmwareUpdatePromptDeviceIDs.insert(deviceID)
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: false)
        refreshFirmwareAvailability()
    }

    func updateFirmwareFromLatest(for deviceID: String,
                                  progress: @escaping (FirmwareUpdateProgress) -> Void,
                                  completion: @escaping (Result<Void, Error>) -> Void) {
        guard let manifest = latestFirmwareManifest else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.invalidResponse))
            return
        }

        firmwareManifestClient.downloadOTA(from: manifest) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(let image):
                    self.ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                        DispatchQueue.main.async {
                            completion(result)
                        }
                    }
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        }
    }

    private func handleStateEvent(_ event: StateEvent, peripheralID: UUID) {
        switch event.event {
        case "device_info":
            if let hardware = event.hardware, let firmwareVersion = event.firmwareVersion {
                NSLog("Connected VoiceStick hardware=\(hardware) firmware=\(firmwareVersion)")
            }
            updateDeviceFirmwareInfo(event: event, peripheralID: peripheralID)
        case "encoder_status":
            if let present = event.encoderPresent, let deviceID = deviceID(for: peripheralID) {
                statusController.setDeviceEncoderPresent(deviceID, present: present)
            }
        case "battery_status":
            if let level = event.batteryLevel, let deviceID = deviceID(for: peripheralID) {
                statusController.setDeviceBattery(
                    deviceID,
                    level: level,
                    charging: event.batteryCharging ?? false,
                    usbPowered: event.batteryUsbPowered ?? false
                )
            }
        case "button_down":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonDown(event, peripheralID: peripheralID)
            } else {
                handleButtonDown(event, peripheralID: peripheralID)
            }
        case "button_up":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonUp(event, peripheralID: peripheralID)
            } else {
                handleButtonUp(event, peripheralID: peripheralID)
            }
        case "button_click":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonClick(event, peripheralID: peripheralID)
            } else {
                handleButtonClick(event, peripheralID: peripheralID)
            }
        case "button_double_click":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonDoubleClick(event, peripheralID: peripheralID)
            } else {
                handleButtonDoubleClick(event, peripheralID: peripheralID)
            }
        case "tap":
            handleTapEvent(event, peripheralID: peripheralID)
        case "encoder_rotate":
            handleEncoderRotate(event, peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleButtonDoubleClick(_ event: StateEvent, peripheralID: UUID) {
        guard event.button == "primary" else { return }
        NSLog("Double-click detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), sending Enter")

        cancelActiveSessionsForDoubleClick(peripheralID: peripheralID)

        injectDoubleClickAction(peripheralID: peripheralID)

        // 回到就绪状态。
        ble.sendUIState("ready", to: peripheralID)
        mainInputState = .ready
        statusController.setStatus("Ready")
    }

    /// 双击注入动作：StickS3 主键双击恒为 Enter（核心交互）；小米语音键双击（A 级）
    /// 按按键映射处置——原生=Enter / 禁用=不注入 / 按键=KeySpec 注入，解析失败回落
    /// Enter（对齐 HID 拦截层语义）。
    private func injectDoubleClickAction(peripheralID: UUID) {
        guard ble.deviceClass(for: peripheralID) == .xiaomiRemote2Pro,
              let deviceID = deviceID(for: peripheralID) else {
            inputInjector.sendEnter()
            return
        }
        let mapping = config.buttonsSettings(for: deviceID).mapping(for: .voiceDoubleClick)
        switch mapping.action {
        case .native:
            inputInjector.sendEnter()
        case .disabled:
            NSLog("xiaomi voice double-click disabled by mapping on \(deviceID)")
        case .key:
            guard let spec = KeySpec.parse(mapping.key) else {
                NSLog("xiaomi voice double-click key invalid: \"\(mapping.key)\" on \(deviceID), fallback to Enter")
                inputInjector.sendEnter()
                return
            }
            inputInjector.sendKeyCombo(spec)
        }
    }

    /// 双击取消结构（对齐 Windows CancelActiveSessionsForDoubleClick）：取消当前活跃
    /// 录音 + 该设备的字幕会话。物理主键双击与编码器双击（key 动作）共用。
    private func cancelActiveSessionsForDoubleClick(peripheralID: UUID) {
        // 取消当前活跃录音（如果有）。
        if case .recording(_, let recordingPeripheralID, _) = mainInputState,
           recordingPeripheralID == peripheralID {
            cancelRecognitionInProgress()
        }
        // 取消字幕会话。
        cancelSubtitleCycles(peripheralID: peripheralID, reason: "double_click")
    }

    private func handleButtonDown(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button down button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
        case "secondary":
            break
        default:
            break
        }
    }

    private func handleButtonUp(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button up button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonUp(peripheralID: peripheralID)
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleButtonClick(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button click button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
                return
            }
            guard config.interactionMode == .clickToTalk else {
                ble.sendUIState("ready", to: peripheralID)
                return
            }
            if case .recording(_, let recordingPeripheralID, _) = mainInputState,
               recordingPeripheralID == peripheralID {
                handlePrimaryButtonUp(peripheralID: peripheralID)
            } else if case .finalizing(_, let finalizingPeripheralID, _) = mainInputState,
                      finalizingPeripheralID == peripheralID {
                NSLog("Ignoring primary button click while recording is finalizing")
                ble.sendUIState("thinking", to: peripheralID)
            } else {
                handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
            }
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleSecondaryButtonClick(peripheralID: UUID) {
        if activeSubtitleSessions[peripheralID] != nil {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        if subtitleCycles.values.contains(where: { $0.peripheralID == peripheralID }) {
            cancelSubtitleCycles(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        cancelPendingPaste(peripheralID: peripheralID)
    }

    // MARK: - 设备交互/编码器设置下发（对齐 Windows 连接回调与 UpdateConfig 的逐台单播）

    /// 对单台 StickS3 下发交互 + 编码器设置（5 项）。调用方保证 deviceID 为 StickS3；
    /// BLE 层 sendStickControlPayload 另有按类门控兜底。
    private func sendDeviceInteractionSettings(deviceID: String) {
        let inter = config.interactionSettings(for: deviceID)
        ble.sendStickControlPayload(
            BleProtocol.tapEnabledPayload(enabled: inter.tapToArrow),
            label: "tap_enabled", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.tapSensitivityPayload(level: inter.tapSensitivity),
            label: "tap_sensitivity", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.imuWakeSensitivityPayload(thresholdLsb: inter.imuWakeSensitivity.thresholdLsb),
            label: "imu_wake_sensitivity", deviceID: deviceID
        )
        let enc = config.encoderSettings(for: deviceID)
        ble.sendStickControlPayload(
            BleProtocol.encoderLedColorPayload(color: enc.ledColor.rawValue),
            label: "encoder_led_color", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.encoderRecordingGatePayload(enabled: enc.pressAction == .recording),
            label: "encoder_recording_gate", deviceID: deviceID
        )
    }

    // MARK: - 编码器按键事件（对齐 Windows HandleEncoderButton{Down,Up,Click,DoubleClick}）

    private func handleEncoderButtonDown(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonDown(event, peripheralID: peripheralID)
            return
        }
        // key 动作：down/up 不注入（在 click 成对确认时注入一次），仅记日志。
        NSLog("encoder button down on VS-\(deviceID(for: peripheralID) ?? "unknown") (press_action=key, ignored)")
    }

    private func handleEncoderButtonUp(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonUp(event, peripheralID: peripheralID)
            return
        }
        NSLog("encoder button up on VS-\(deviceID(for: peripheralID) ?? "unknown") (press_action=key, ignored)")
    }

    private func handleEncoderButtonClick(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonClick(event, peripheralID: peripheralID)
            return
        }
        guard let spec = KeySpec.parse(enc.pressKey) else {
            NSLog("encoder press key invalid: \"\(enc.pressKey)\" on VS-\(deviceID(for: peripheralID) ?? "unknown"), click ignored")
            return
        }
        // 编码器单击在录音/识别中仍注入是有意设计：该键是用户显式配置的快捷键（如撤销），
        // 与会话状态正交；不同于 rotate 的录音中抑制。
        NSLog("encoder click on VS-\(deviceID(for: peripheralID) ?? "unknown"), injecting \(spec.displayText)")
        inputInjector.sendKeyCombo(spec)
    }

    private func handleEncoderButtonDoubleClick(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        let deviceID = deviceID(for: peripheralID) ?? ""
        if enc.doubleClickAction == .recording {
            // 切换录音起停：复用固件 remote_button 通道（固件侧等价一次远程按下/松开，
            // 音频链路真实完整，等同 click_to_talk 点按起停）。remote_button 走
            // APP_INPUT_SOURCE_REMOTE，不受 encoder_recording_gate 门控约束——
            // press_action=key（门控关）时双击起停录音仍可用，属有意设计。
            let hasActive = activeSessionID != nil || activeSubtitleSessions[peripheralID] != nil
            NSLog("encoder double-click on VS-\(deviceID)\(hasActive ? ", remote stop recording" : ", remote start recording")")
            let requestID = nextHotkeyRequestID
            nextHotkeyRequestID &+= 1
            ble.sendRemoteButton(
                action: hasActive ? "up" : "down",
                deviceID: deviceID,
                requestID: requestID
            )
            return
        }
        // key 动作：沿用物理主键双击的取消结构，注入配置的按键（默认 enter=现行为）。
        cancelActiveSessionsForDoubleClick(peripheralID: peripheralID)
        if let spec = KeySpec.parse(enc.doubleClickKey) {
            NSLog("encoder double-click on VS-\(deviceID), injecting \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
        } else {
            NSLog("encoder double-click key invalid: \"\(enc.doubleClickKey)\" on VS-\(deviceID), fallback to Enter")
            inputInjector.sendEnter()
        }
        ble.sendUIState("ready", to: peripheralID)
        mainInputState = .ready
        statusController.setStatus("Ready")
    }

    // MARK: - 敲击事件（对齐 Windows HandleTapEvent）

    private func handleTapEvent(_ event: StateEvent, peripheralID: UUID) {
        // 总开关关闭则忽略（按设备取有效配置）。
        guard config.interactionSettings(for: deviceID(for: peripheralID)).tapToArrow else { return }
        // 录音中或识别中忽略敲击，避免震动干扰当前语音周期（macOS 无体感鼠标，
        // 无 IsAirMouseActive 分支）。与双击主键不同：tap 不取消录音/识别，仅在不冲突时注入方向键。
        if mainInputState.isRecording || mainInputState.isFinalizing {
            return
        }
        // 节流：两次方向键注入最短间隔 500ms，防止快速连击导致光标连续下移。
        let now = Date()
        if let last = lastTapInjectAt, now.timeIntervalSince(last) < 0.5 {
            NSLog("tap detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), throttled (<500ms since last)")
            return
        }
        lastTapInjectAt = now
        NSLog("tap detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), sending ArrowDown")
        inputInjector.sendArrowDown()
        ble.sendUIState("ready", to: peripheralID)
    }

    // MARK: - 编码器旋转事件（对齐 Windows HandleEncoderRotate/InjectEncoderRotateSteps/
    // FlushEncoderRotatePending/EncoderRotateTick）

    /// steps 上限钳制：固件侧已截断到 uint8（255），桌面端再钳到物理合理值，
    /// 防伪造/异常 BLE 帧让注入循环放大挂死线程。真实 10ms 窗口内旋转 1~3 步。
    private static let maxEncoderRotateSteps: UInt32 = 64
    /// 停转窗口：连续旋转时事件间隔 <=10ms（加 BLE 抖动亦远小于此值），静默超过
    /// 250ms 无旋转事件即可靠判定停稳。
    private static let encoderRotateStopGap: TimeInterval = 0.25

    private func handleEncoderRotate(_ event: StateEvent, peripheralID: UUID) {
        let deviceID = deviceID(for: peripheralID) ?? ""
        let enc = config.encoderSettings(for: deviceID)
        // 总开关关闭则忽略。
        guard enc.toArrow else { return }
        // 录音中或识别中忽略旋转，避免干扰当前语音周期（门控与 tap 一致；
        // macOS 无体感鼠标，无 IsAirMouseActive 分支）。
        if mainInputState.isRecording || mainInputState.isFinalizing {
            return
        }
        let rawSteps = event.steps ?? 0
        guard rawSteps > 0 else { return }
        let steps = min(rawSteps, Self.maxEncoderRotateSteps)
        // 多设备交替旋转：测速估计器是全局单例，换设备时重置冷启动，避免跨设备手势
        // 互相污染 EWMA 估计（阈值/方向可能按设备不同）。
        if deviceID != lastEncoderRotateDeviceID {
            encoderSpeedEstimator.reset()
            lastEncoderRotateDeviceID = deviceID
        }
        // 方向映射：默认 cw→rotate_cw_key / ccw→rotate_ccw_key；
        // rotation_invert=true 时翻转。direction 非 "ccw"（含空串/未知值）按 cw 处理，
        // 与固件只发 cw|ccw 的约定一致。
        let effectiveCCW = (event.direction == "ccw") != enc.rotationInvert
        let now = Date()
        if encoderRotateLockout {
            let stopped = lastEncoderRotateEventAt
                .map { now.timeIntervalSince($0) > Self.encoderRotateStopGap } ?? true
            if !stopped {
                // 锁定中：屏蔽一切旋转输出（含快甩减速段的慢速事件与换向事件）。
                // 不喂测速估计器：减速段样本与新手势无关，静默 250ms 后估计器自动冷启动。
                lastEncoderRotateEventAt = now
                NSLog("encoder rotate suppressed (lockout) on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps)")
                return
            }
            // 已停稳：退出锁定，本事件走正常识别。
            encoderRotateLockout = false
        }
        // 快慢分档：EWMA 平滑估计测速（见 EncoderRotateSpeedEstimator）；估计值
        // >= rotate_fast_threshold 走快速档按键，否则走普通按键。
        let speedSps = encoderSpeedEstimator.addSample(now: now, steps: steps)
        let fast = EncoderRotateSpeedEstimator.isFast(
            smoothedSpeedSps: speedSps, thresholdSps: enc.rotateFastThreshold
        )
        let normalKey = effectiveCCW ? enc.rotateCcwKey : enc.rotateCwKey
        let fastKey = effectiveCCW ? enc.rotateCcwFastKey : enc.rotateCwFastKey
        if fast {
            // 快速甩动视为一次手势：注入一次快速键后进入停转锁定，直到停稳才恢复识别。
            encoderRotateLockout = true
            lastEncoderRotateEventAt = now
            if encoderPendingActive {
                // 加速段识别：挂起的慢速事件是本次快甩的起步，整段丢弃不注入。
                encoderPendingActive = false
                encoderPendingSteps = 0
                encoderPendingPeripheralID = nil
                stopEncoderRotateTimer()
                NSLog("encoder rotate pending discarded (acceleration) on VS-\(deviceID)")
            }
            // 选键：快速档 → 普通档 → 方向键兜底；一次手势只注入一次。
            var fastSpec = KeySpec.parse(fastKey)
            if fastSpec == nil {
                // 快速档按键非法（绕过加载校验直改内存/未来新键名）回退普通按键。
                NSLog("encoder rotate fast key invalid: \"\(fastKey)\" on VS-\(deviceID), fallback to normal key")
                fastSpec = KeySpec.parse(normalKey)
            }
            guard let spec = fastSpec else {
                // 非法配置回退方向键，保持可用。
                NSLog("encoder rotate key invalid: \"\(normalKey)\" on VS-\(deviceID), fallback to arrows")
                if effectiveCCW {
                    inputInjector.sendArrowUp()
                } else {
                    inputInjector.sendArrowDown()
                }
                return
            }
            // 与 tap 不同，旋转不回写 ui_state（无屏幕状态变化）。
            NSLog("encoder rotate on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps)\(rawSteps > steps ? " (clamped from \(rawSteps))" : "") speed=\(Int(speedSps))sps [fast] -> \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
            return
        }
        // 慢速路径：延迟判定——先挂起累计，判定窗内判快则整段丢弃（见 fast 分支），
        // 到期由 encoderRotateTick 或此处新事件检查冲刷。window<=0 时立即注入（旧行为）。
        if enc.rotateDecideWindowMs <= 0 {
            injectEncoderRotateSteps(ccw: effectiveCCW, steps: steps, keyText: normalKey, deviceID: deviceID)
            return
        }
        if encoderPendingActive {
            let expired = now.timeIntervalSince(encoderPendingStartedAt) >=
                Double(enc.rotateDecideWindowMs) / 1000.0
            if expired || encoderPendingCCW != effectiveCCW {
                // 旧 pending 到期或换向：不是本次快甩的加速段，立即冲刷。
                flushEncoderRotatePending()
            }
        }
        if !encoderPendingActive {
            encoderPendingActive = true
            encoderPendingCCW = effectiveCCW
            encoderPendingSteps = 0
            encoderPendingStartedAt = now
            encoderPendingPeripheralID = peripheralID
            startEncoderRotateTimer()
        }
        encoderPendingSteps = min(encoderPendingSteps + steps, Self.maxEncoderRotateSteps)
        NSLog("encoder rotate pending on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps) total=\(encoderPendingSteps)")
    }

    private func injectEncoderRotateSteps(ccw: Bool, steps: UInt32, keyText: String, deviceID: String) {
        guard let spec = KeySpec.parse(keyText) else {
            // 非法配置（绕过加载校验直改内存/未来新键名）回退方向键，保持可用。
            NSLog("encoder rotate key invalid: \"\(keyText)\" on VS-\(deviceID), fallback to arrows")
            for _ in 0..<steps {
                if ccw {
                    inputInjector.sendArrowUp()
                } else {
                    inputInjector.sendArrowDown()
                }
            }
            return
        }
        // 与 tap 不同，旋转不回写 ui_state（无屏幕状态变化）。
        NSLog("encoder rotate inject on VS-\(deviceID) steps=\(steps) -> \(spec.displayText)")
        for _ in 0..<steps {
            inputInjector.sendKeyCombo(spec)
        }
    }

    private func flushEncoderRotatePending() {
        guard encoderPendingActive else { return }
        let ccw = encoderPendingCCW
        let steps = encoderPendingSteps
        let peripheralID = encoderPendingPeripheralID
        encoderPendingActive = false
        encoderPendingSteps = 0
        encoderPendingPeripheralID = nil
        stopEncoderRotateTimer()
        // 按挂起 pending 来源设备取覆盖配置；设备中途断开则回全局默认（可接受）。
        let deviceID = peripheralID.flatMap { self.deviceID(for: $0) } ?? ""
        let enc = config.encoderSettings(for: deviceID)
        let key = ccw ? enc.rotateCcwKey : enc.rotateCwKey
        injectEncoderRotateSteps(ccw: ccw, steps: steps, keyText: key, deviceID: deviceID)
    }

    private func encoderRotateTick() {
        guard encoderPendingActive, let peripheralID = encoderPendingPeripheralID else { return }
        // 到期判定用挂起 pending 来源设备的覆盖配置（decide_window_ms 可按设备不同）。
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        guard enc.rotateDecideWindowMs > 0 else { return }
        if Date().timeIntervalSince(encoderPendingStartedAt) >=
            Double(enc.rotateDecideWindowMs) / 1000.0 {
            flushEncoderRotatePending()
        }
    }

    private func startEncoderRotateTimer() {
        guard encoderRotateTimer == nil else { return }
        // 30ms 周期检查 pending 到期（对齐 Windows UI 层 30ms 定时器）。
        let timer = Timer(timeInterval: 0.03, repeats: true) { [weak self] _ in
            self?.encoderRotateTick()
        }
        RunLoop.main.add(timer, forMode: .common)
        encoderRotateTimer = timer
    }

    private func stopEncoderRotateTimer() {
        encoderRotateTimer?.invalidate()
        encoderRotateTimer = nil
    }

    private func handlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        if config.defaultOutputProfile.target == .subtitle {
            handleSubtitlePrimaryButtonDown(sessionID: sessionID, peripheralID: peripheralID)
            return
        }
        if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
            return
        }
        if mainInputState.isBusy {
            if activePeripheralID != peripheralID {
                ble.sendUIState("ready", to: peripheralID)
            } else if mainInputState.isFinalizing || isWaitingForFinalText {
                ble.sendUIState("thinking", to: peripheralID)
            }
            NSLog("Ignoring primary button while main input is busy")
            return
        }
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring primary button down with missing/zero session; sending ready")
            ble.sendUIState("ready", to: peripheralID)
            return
        }

        mainInputState = .recording(sessionID: sessionID, peripheralID: peripheralID, startedAt: Date())
        receivedAudioFrames = 0
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        pendingPasteState = .idle
        isShowingASRError = false
        oggMuxer.reset()
        debugAudioRecorder.start(deviceID: deviceID(for: peripheralID), sessionID: sessionID,
                                 devicePrefix: deviceNamePrefix(for: peripheralID))
        statusController.showListening(deviceID: deviceID(for: peripheralID))
        sendUIStateForActiveDevice("recording")
    }

    private func handlePrimaryButtonUp(peripheralID: UUID) {
        if activeSubtitleSessions[peripheralID] != nil {
            handleSubtitlePrimaryButtonUp(peripheralID: peripheralID)
            return
        }
        guard case .recording(_, let recordingPeripheralID, _) = mainInputState,
              recordingPeripheralID == peripheralID
        else {
            return
        }
        let recordingDuration = currentRecordingDuration
        let isValidRecording = recordingDuration >= minimumRecordingDuration
        if !isValidRecording {
            cancelShortRecording()
        } else {
            beginWaitingForAudioEnd(reason: "button_up")
        }
    }

    private func handleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        if subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) != nil {
            handleSubtitleAudioFrame(frame, peripheralID: peripheralID)
            return
        }
        guard frame.sessionID == activeSessionID, activePeripheralID == peripheralID else { return }
        guard !sentFinalAudioChunk || (frame.isEnd && frame.payload.isEmpty) else { return }

        if frame.isEnd && frame.payload.isEmpty {
            cancelAudioEndTimeout()
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }

        guard !frame.payload.isEmpty else { return }
        receivedAudioFrames += 1
        let oggChunk = oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        debugAudioRecorder.append(oggChunk)
        sendOrBufferOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: currentRecordingDuration >= minimumRecordingDuration
        )
        if frame.isEnd {
            cancelAudioEndTimeout()
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            enterFinalizingState(reason: "audio_end")
            if !asrStarted && recordingDuration < minimumRecordingDuration {
                cancelShortRecording()
            } else if asrStarted {
                debugAudioRecorder.finish()
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
            } else {
                debugAudioRecorder.finish()
                if !startASRAndFlushBufferedChunks(lastChunkIsFinal: true) {
                    finishWithASRError("Failed to start ASR")
                    return
                }
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
            }
        }
    }

    private func beginWaitingForAudioEnd(reason: String) {
        guard !waitingForAudioEnd else { return }
        waitingForAudioEnd = true
        NSLog("Waiting for audio END frame reason=\(reason)")
        enterFinalizingState(reason: reason)
        scheduleAudioEndTimeout()
    }

    private func enterFinalizingState(reason: String) {
        guard case .recording(let sessionID, let peripheralID, let startedAt) = mainInputState else {
            return
        }
        NSLog("Main input finalizing reason=\(reason)")
        mainInputState = .finalizing(sessionID: sessionID, peripheralID: peripheralID, startedAt: startedAt)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
    }

    private func scheduleAudioEndTimeout() {
        audioEndTimeoutTimer?.invalidate()
        let sessionID = activeSessionID
        let peripheralID = activePeripheralID
        audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self else { return }
            guard self.waitingForAudioEnd,
                  self.activeSessionID == sessionID,
                  self.activePeripheralID == peripheralID
            else { return }
            NSLog("Audio END timeout; finalizing buffered audio")
            self.sendFinalOggChunkIfNeeded(recordingDuration: self.currentRecordingDuration)
        }
    }

    private func cancelAudioEndTimeout() {
        waitingForAudioEnd = false
        audioEndTimeoutTimer?.invalidate()
        audioEndTimeoutTimer = nil
    }

    private func handleSubtitlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring subtitle button_down with missing/zero session dev=VS-\(deviceID(for: peripheralID) ?? "unknown"); sending ready")
            ble.sendUIState("ready", to: peripheralID)
            return
        }
        let deviceID = deviceID(for: peripheralID)
        NSLog("Subtitle button_down dev=VS-\(deviceID ?? "unknown") session=\(sessionID) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil") existing=\(subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil)")
        if activeSubtitleSessions[peripheralID] == sessionID ||
            subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil {
            NSLog("Subtitle button_down ignored dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
            return
        }
        if let previousSessionID = activeSubtitleSessions[peripheralID] {
            NSLog("Subtitle button_down preempt active dev=VS-\(deviceID ?? "unknown") previous=\(previousSessionID) next=\(sessionID)")
            clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: previousSessionID)
        }
        let cycle = SubtitleCycle(
            peripheralID: peripheralID,
            deviceID: deviceID,
            sessionID: sessionID,
            config: config
        )
        configureSubtitleASRCallbacks(for: cycle)
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)] = cycle
        activeSubtitleSessions[peripheralID] = sessionID
        cycle.debugAudioRecorder.start(deviceID: deviceID, sessionID: sessionID,
                                       devicePrefix: deviceNamePrefix(for: peripheralID))
        NSLog("Subtitle cycle start dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
        statusController.showListening(deviceID: deviceID)
        ble.sendUIState("recording", to: peripheralID)
    }

    private func handleSubtitlePrimaryButtonUp(peripheralID: UUID) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else {
            NSLog("Subtitle button_up ignored no active cycle dev=VS-\(deviceID(for: peripheralID) ?? "unknown")")
            return
        }
        let sessionID = cycle.sessionID
        NSLog("Subtitle button_up dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) duration=\(String(format: "%.3f", cycle.duration))")
        if cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
        } else if cycle.receivedAudioFrames == 0 {
            finishSubtitleCycleWithError(
                peripheralID: peripheralID,
                sessionID: sessionID,
                message: "No audio frames from device"
            )
        } else {
            beginWaitingForSubtitleAudioEnd(cycle, reason: "button_up")
            finishSubtitleAudioInput(cycle)
        }
    }

    private func handleSubtitleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) else { return }
        if frame.isEnd && frame.payload.isEmpty {
            cancelSubtitleAudioEndTimeout(cycle)
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: frame.sessionID)
            return
        }
        guard !frame.payload.isEmpty else { return }
        cycle.receivedAudioFrames += 1
        let oggChunk = cycle.oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        cycle.debugAudioRecorder.append(oggChunk)
        sendOrBufferSubtitleOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: cycle.duration >= minimumRecordingDuration,
            cycle: cycle
        )
        if frame.isEnd {
            cancelSubtitleAudioEndTimeout(cycle)
            cycle.sentFinalAudioChunk = true
            if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
                cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            } else if cycle.asrStarted {
                cycle.debugAudioRecorder.finish()
                finishSubtitleAudioInput(cycle)
            } else {
                cycle.debugAudioRecorder.finish()
                if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: true) {
                    finishSubtitleCycleWithError(
                        peripheralID: peripheralID,
                        sessionID: cycle.sessionID,
                        message: "Failed to start ASR"
                    )
                    return
                }
                finishSubtitleAudioInput(cycle)
            }
        }
    }

    private func beginWaitingForSubtitleAudioEnd(_ cycle: SubtitleCycle, reason: String) {
        guard !cycle.waitingForAudioEnd else { return }
        cycle.waitingForAudioEnd = true
        NSLog("Waiting for subtitle audio END frame VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        if config.interactionMode != .holdToTalk {
            statusController.setStatus("Processing")
            ble.sendUIState("thinking", to: cycle.peripheralID)
        }
        scheduleSubtitleAudioEndTimeout(cycle)
    }

    private func scheduleSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.audioEndTimeoutTimer?.invalidate()
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self,
                  let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                  cycle.waitingForAudioEnd
            else { return }
            NSLog("Subtitle audio END timeout VS-\(cycle.deviceID ?? "unknown"); finalizing buffered audio")
            self.sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: sessionID)
        }
    }

    private func cancelSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.waitingForAudioEnd = false
        cycle.audioEndTimeoutTimer?.invalidate()
        cycle.audioEndTimeoutTimer = nil
    }

    private func sendSubtitleFinalOggChunkIfNeeded(peripheralID: UUID, sessionID: UInt32) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.sentFinalAudioChunk
        else { return }
        cycle.sentFinalAudioChunk = true
        NSLog("Subtitle audio final dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) asr_started=\(cycle.asrStarted)")
        cancelSubtitleAudioEndTimeout(cycle)
        if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            return
        }
        let finalChunk = cycle.oggMuxer.finish()
        cycle.debugAudioRecorder.append(finalChunk)
        cycle.debugAudioRecorder.finish()
        sendOrBufferSubtitleOggChunk(finalChunk, isLast: true, canStartASR: true, cycle: cycle)
    }

    private func finishSubtitleAudioInput(_ cycle: SubtitleCycle) {
        if config.interactionMode == .holdToTalk {
            NSLog("Subtitle audio input finished dev=VS-\(cycle.deviceID ?? "unknown") session=\(cycle.sessionID) -> device ready")
            clearActiveSubtitleSession(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID)
            ble.sendUIState("ready", to: cycle.peripheralID)
        } else {
            statusController.setStatus("Processing")
            ble.sendUIState("thinking", to: cycle.peripheralID)
        }
    }

    private func sendOrBufferSubtitleOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool, cycle: SubtitleCycle) {
        if cycle.asrStarted {
            cycle.asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }
        cycle.bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: isLast) {
            finishSubtitleCycleWithError(
                peripheralID: cycle.peripheralID,
                sessionID: cycle.sessionID,
                message: "Failed to start ASR"
            )
        }
    }

    private func startSubtitleASRAndFlushBufferedChunks(_ cycle: SubtitleCycle, lastChunkIsFinal: Bool) -> Bool {
        guard !cycle.asrStarted else { return true }
        let useDefiniteSegments = shouldUseDefiniteSegments(for: outputProfile(for: cycle.deviceID))
        guard cycle.asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments
        )) else {
            cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }
        cycle.asrStarted = true
        for bufferedChunk in cycle.bufferedOggChunks.dropLast() {
            cycle.asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = cycle.bufferedOggChunks.last {
            cycle.asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func sendFinalOggChunkIfNeeded(recordingDuration: TimeInterval) {
        guard !sentFinalAudioChunk else { return }
        sentFinalAudioChunk = true
        cancelAudioEndTimeout()
        enterFinalizingState(reason: "final_audio_sent")
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            cancelShortRecording()
            return
        }
        if receivedAudioFrames == 0 {
            finishWithASRError("No audio frames from device")
            return
        }

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
    }

    private func shouldSendPartialToDevice() -> Bool {
        sentFinalAudioChunk
    }

    private var currentRecordingDuration: TimeInterval {
        guard let activeSessionStartedAt else { return 0 }
        return Date().timeIntervalSince(activeSessionStartedAt)
    }

    private func sendOrBufferOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool) {
        if asrStarted {
            asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }

        bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startASRAndFlushBufferedChunks(lastChunkIsFinal: isLast) {
            finishWithASRError("Failed to start ASR")
        }
    }

    private func startASRAndFlushBufferedChunks(lastChunkIsFinal: Bool) -> Bool {
        guard !asrStarted else { return true }
        let profile = outputProfile(for: activeDeviceID)
        let useDefiniteSegments = shouldUseDefiniteSegments(for: profile)
        guard asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments
        )) else {
            bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }

        asrStarted = true
        for bufferedChunk in bufferedOggChunks.dropLast() {
            asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = bufferedOggChunks.last {
            asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func cancelShortRecording() {
        NSLog("Ignoring short recording under \(minimumRecordingDuration)s")
        cancelAudioEndTimeout()
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asr.cancel()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        debugAudioRecorder.discard()
        statusController.setStatus("Ready")
        statusController.hideOverlay()
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func finishWithFinalText(_ text: String) {
        guard !pastedFinalText else { return }
        // LLM 精修（对齐 Windows refine_enabled 语义）：final 原文先经 LLM 改写，
        // overlay 进 refining 态（三点跳动 + 原文）；失败/热词丢失回退原文。
        if config.refineEnabled, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            statusController.showRefining(text, deviceID: activeDeviceID)
            refiner.refine(text, hotwords: config.asrHotwords) { [weak self] refined in
                DispatchQueue.main.async {
                    guard let self, !self.pastedFinalText else { return }
                    self.finishWithRefinedText(refined ?? text)
                }
            }
            return
        }
        finishWithRefinedText(text)
    }

    private func finishWithRefinedText(_ text: String) {
        guard !pastedFinalText else { return }
        let profile = outputProfile(for: activeDeviceID)
        if profile.target == .subtitle {
            pastedFinalText = true
            pendingPasteState = .idle
            let deviceID = activeDeviceID
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                showSubtitleText(text, profile: profile, deviceID: deviceID)
            }
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }
        if text.isEmpty {
            pastedFinalText = true
            pendingPasteState = .idle
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }

        if profile.transform == .translate {
            pastedFinalText = true
            statusController.setStatus("Translating")
            transformText(text, profile: profile, deviceID: activeDeviceID) { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let translatedText):
                    self.enterPendingConfirmation(text: translatedText)
                case .failure(let error):
                    self.finishWithASRError(error.localizedDescription)
                }
            }
            return
        }

        enterPendingConfirmation(text: text)
    }

    private func enterPendingConfirmation(text: String) {
        pastedFinalText = true
        lastRecoverableText = text
        lastRecoverablePeripheralID = activePeripheralID
        statusController.setHasRecoverableInput(true)
        pendingPasteState = .waitingToPaste(text: text)
        if let peripheralID = activePeripheralID {
            mainInputState = .pendingConfirmation(peripheralID: peripheralID)
        }
        statusController.showFinal(text, deviceID: activeDeviceID) { [weak self] in
            self?.commitPendingPaste(text: text)
        }
        sendUIStateForActiveDevice("pending_confirmation", text: text)
    }

    private func handleDefiniteSegment(_ segment: ASRSegment) {
        guard segment.definite, let deviceID = activeDeviceID else { return }
        let profile = outputProfile(for: deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: deviceID)
    }

    private func handleSubtitleDefiniteSegment(_ segment: ASRSegment, peripheralID: UUID) {
        guard segment.definite, let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        let profile = outputProfile(for: cycle.deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: cycle.deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: cycle.deviceID)
    }

    private func finishSubtitleCycleWithFinalText(peripheralID: UUID, sessionID: UInt32, text: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.finishedFinalText
        else { return }
        cycle.finishedFinalText = true
        NSLog("Subtitle final text dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) text_len=\(text.count)")
        let profile = outputProfile(for: cycle.deviceID)
        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showSubtitleText(text, profile: profile, deviceID: cycle.deviceID) { [weak self] didShowSubtitle in
                guard let self else { return }
                self.finishSubtitleCycle(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    hideOverlay: didShowSubtitle &&
                        self.shouldHideOverlayForFinishedSubtitleCycle(
                            peripheralID: peripheralID,
                            sessionID: sessionID
                        )
                )
            }
            return
        }
        finishSubtitleCycle(
            peripheralID: peripheralID,
            sessionID: sessionID,
            hideOverlay: shouldHideOverlayForFinishedSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
        )
    }

    private func finishSubtitleCycleWithError(peripheralID: UUID, sessionID: UInt32, message: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("ASR error VS-\(cycle.deviceID ?? "unknown"): \(message)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.showError(message, deviceID: cycle.deviceID) { [weak self] in
                self?.ble.sendUIState("ready", to: peripheralID)
            }
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func cancelSubtitleCycle(peripheralID: UUID, reason: String) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        NSLog("Cancel subtitle cycle VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        statusController.hideOverlay(deviceID: cycle.deviceID)
        ble.sendUIState("ready", to: peripheralID)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: cycle.sessionID)
        subtitleCycles.removeValue(
            forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: cycle.sessionID)
        )
    }

    private func finishSubtitleCycle(peripheralID: UUID, sessionID: UInt32, hideOverlay: Bool) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("Subtitle cycle finish dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) hide_overlay=\(hideOverlay) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil")")
        if hideOverlay {
            statusController.hideOverlay(deviceID: cycle.deviceID)
        }
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.setStatus("Ready")
            ble.sendUIState("ready", to: peripheralID)
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func shouldHideOverlayForFinishedSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) ||
            !hasActiveSubtitleSession(peripheralID: peripheralID)
    }

    private func canUpdateOverlayForSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID].map { $0 == sessionID } ?? true
    }

    private func shouldSendSubtitlePartialToDevice(_ cycle: SubtitleCycle) -> Bool {
        guard isActiveSubtitleCycle(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID) else {
            return false
        }
        return cycle.sentFinalAudioChunk
    }

    private func subtitleCycle(peripheralID: UUID, sessionID: UInt32) -> SubtitleCycle? {
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)]
    }

    private func activeSubtitleCycle(peripheralID: UUID) -> SubtitleCycle? {
        guard let sessionID = activeSubtitleSessions[peripheralID] else { return nil }
        return subtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
    }

    private func isActiveSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID] == sessionID
    }

    private func hasActiveSubtitleSession(peripheralID: UUID) -> Bool {
        activeSubtitleSessions[peripheralID] != nil
    }

    private func clearActiveSubtitleSession(peripheralID: UUID, sessionID: UInt32) {
        if activeSubtitleSessions[peripheralID] == sessionID {
            activeSubtitleSessions.removeValue(forKey: peripheralID)
        }
    }

    private func cancelSubtitleCycles(peripheralID: UUID, reason: String) {
        NSLog("Cancel subtitle cycles \(peripheralID) reason=\(reason)")
        activeSubtitleSessions.removeValue(forKey: peripheralID)
        let keys = subtitleCycles.keys.filter { $0.peripheralID == peripheralID }
        for key in keys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            cancelSubtitleAudioEndTimeout(cycle)
            subtitleCycles.removeValue(forKey: key)
        }
        statusController.hideOverlay(deviceID: deviceID(for: peripheralID))
        ble.sendUIState("ready", to: peripheralID)
    }

    private func showSubtitleText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        completion: ((Bool) -> Void)? = nil
    ) {
        guard let deviceID else {
            completion?(false)
            return
        }
        transformText(text, profile: profile, deviceID: deviceID) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let outputText):
                NSLog("Subtitle show text dev=VS-\(deviceID) text_len=\(outputText.count)")
                self.subtitleController.show(
                    text: outputText,
                    deviceID: deviceID,
                    color: self.config.themeColor(for: deviceID)
                )
                completion?(true)
            case .failure(let error):
                self.statusController.showError(error.localizedDescription, deviceID: deviceID)
                completion?(false)
            }
        }
    }

    private func transformText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        guard profile.transform == .translate else {
            completion(.success(text))
            return
        }
        translator.translate(
            text,
            targetLanguage: profile.translationTarget,
            hotwords: config.asrHotwords
        ) { [weak self] result in
            DispatchQueue.main.async {
                guard self != nil else { return }
                completion(result)
            }
        }
    }

    private func finishWithASRError(_ message: String) {
        NSLog("ASR error: \(message)")
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        debugAudioRecorder.discard()
        isShowingASRError = true
        errorRecoveryToken += 1
        let token = errorRecoveryToken
        let errorPeripheralID = activePeripheralID
        mainInputState = .error(peripheralID: errorPeripheralID)
        finishRecognitionCycle()
        sendUIStateForActiveDevice("error", text: message)
        statusController.showError(message, deviceID: activeDeviceID) { [weak self] in
            guard let self, self.errorRecoveryToken == token else { return }
            self.recoverFromASRError(hideOverlay: false)
        }
    }

    private func presentASRUpgradeAlert(url: URL, message: String) {
        statusController.hideOverlay { [weak self] in
            guard let self else { return }
            self.recoverFromASRError(hideOverlay: false)
            NSApp.activate(ignoringOtherApps: true)

            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "VoiceStick Cloud needs attention"
            alert.informativeText = message
            alert.addButton(withTitle: "Open")
            alert.addButton(withTitle: "Cancel")
            if alert.runModal() == .alertFirstButtonReturn {
                NSWorkspace.shared.open(url)
            }
        }
    }

    private func recoverFromASRError(hideOverlay: Bool = true) {
        guard isShowingASRError else { return }
        errorRecoveryToken += 1
        isShowingASRError = false
        if hideOverlay {
            statusController.hideOverlay()
        }
        if pairedDeviceIDs.isEmpty {
            statusController.setStatus("Pair a VoiceStick")
        } else {
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
        }
    }

    private func commitPendingPaste(text: String) {
        guard pendingPasteText == text else {
            return
        }

        completePendingPaste(text: text)
    }

    /// 无辅助功能权限时的注入兜底：悬浮窗提示（10s 节流，防编码器旋转等高频路径
    /// 刷屏）+ 转发 AppDelegate 弹每次启动一次的设置引导窗。
    private var lastAccessibilityWarningAt: Date?

    private func handleAccessibilityPermissionMissing() {
        let now = Date()
        if lastAccessibilityWarningAt == nil
            || now.timeIntervalSince(lastAccessibilityWarningAt!) > 10 {
            lastAccessibilityWarningAt = now
            statusController.showTimedMessage(
                tr(.accessibilityPasteBlocked), duration: 2.5, deviceID: activeDeviceID
            )
        }
        onAccessibilityPermissionMissing?()
    }

    private func completePendingPaste(text: String) {
        let shouldPressEnter = config.autoEnter
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
        inputInjector.paste(text: text, pressEnter: shouldPressEnter)
    }

    private var isWaitingForFinalText: Bool {
        mainInputState.isFinalizing &&
            sentFinalAudioChunk &&
            asrStarted &&
            pendingPasteState.isIdle &&
            !pastedFinalText
    }

    private var pendingPasteText: String? {
        switch pendingPasteState {
        case .idle:
            return nil
        case .waitingToPaste(let text), .paused(let text):
            return text
        }
    }

    func restoreLastInputConfirmation() -> Bool {
        restoreLastInputConfirmation(peripheralID: lastRecoverablePeripheralID)
    }

    private func restoreLastInputConfirmation(peripheralID: UUID?) -> Bool {
        guard
            pendingPasteState.isIdle,
            !mainInputState.isBusy,
            !isWaitingForFinalText,
            let text = lastRecoverableText,
            !text.isEmpty
        else {
            return false
        }

        pendingPasteState = .paused(text: text)
        if let peripheralID {
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
        }
        statusController.showPausedFinal(text, deviceID: activeDeviceID)
        sendUIStateForActiveDevice("pending_confirmation", text: text)
        return true
    }

    private func handleFrontButtonDuringPendingPaste(peripheralID: UUID) -> Bool {
        switch pendingPasteState {
        case .idle:
            return false
        case .waitingToPaste(let text):
            guard activePeripheralID == peripheralID else { return true }
            pendingPasteState = .paused(text: text)
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
            statusController.showPausedFinal(text, deviceID: deviceID(for: peripheralID))
            sendUIStateForActiveDevice("pending_confirmation", text: text)
            return true
        case .paused(let text):
            guard activePeripheralID == peripheralID else { return true }
            statusController.hideOverlay { [weak self] in
                self?.commitPendingPaste(text: text)
            }
            return true
        }
    }

    private func cancelPendingPaste(peripheralID: UUID) {
        if activeSessionID != nil {
            if activePeripheralID == peripheralID {
                cancelRecognitionInProgress()
            }
            return
        }
        if isWaitingForFinalText {
            guard activePeripheralID == peripheralID else { return }
            cancelRecognitionInProgress()
            return
        }
        if pendingPasteText == nil {
            _ = restoreLastInputConfirmation(peripheralID: peripheralID)
            return
        }
        guard activePeripheralID == peripheralID else { return }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func cancelRecognitionInProgress() {
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func cancelActiveCycleIfDeviceDisconnected() {
        let disconnectedSubtitleKeys = subtitleCycles.keys.filter { !ble.isConnected($0.peripheralID) }
        for key in disconnectedSubtitleKeys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            statusController.hideOverlay(deviceID: cycle.deviceID)
            clearActiveSubtitleSession(peripheralID: key.peripheralID, sessionID: key.sessionID)
            subtitleCycles.removeValue(forKey: key)
        }
        guard let activePeripheralID, !ble.isConnected(activePeripheralID) else { return }
        if waitingForAudioEnd {
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }
        asr.cancel()
        pendingPasteState = .idle
        mainInputState = .ready
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        statusController.hideOverlay()
        subtitleController.hideAll()
    }

    private func finishRecognitionCycle() {
        cancelAudioEndTimeout()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        bufferedOggChunks.removeAll(keepingCapacity: true)
    }

    private func updateDeviceFirmwareInfo(event: StateEvent, peripheralID: UUID) {
        guard let deviceID = ble.deviceID(for: peripheralID) else { return }
        var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
        if let hardware = event.hardware {
            info.hardware = hardware
        }
        if let firmwareVersion = event.firmwareVersion {
            info.currentVersion = firmwareVersion
        }
        info.errorMessage = nil
        firmwareInfoByDeviceID[deviceID] = info
        refreshFirmwareAvailability()
    }

    private func startFirmwareManifestRefreshTimer() {
        firmwareManifestRefreshTimer?.invalidate()
        firmwareManifestRefreshTimer = Timer.scheduledTimer(withTimeInterval: 60 * 60, repeats: true) { [weak self] _ in
            self?.checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        }
    }

    private func checkFirmwareUpdatesIfNeeded(force: Bool, showErrors: Bool) {
        if firmwareManifestCheckInFlight {
            return
        }
        if !force,
           let lastFirmwareManifestCheckAt,
           Date().timeIntervalSince(lastFirmwareManifestCheckAt) < firmwareManifestCacheDuration {
            refreshFirmwareAvailability()
            return
        }

        firmwareManifestCheckInFlight = true
        setFirmwareChecking(true)
        firmwareManifestClient.fetchManifest { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.firmwareManifestCheckInFlight = false
                self.setFirmwareChecking(false)
                switch result {
                case .success(let manifest):
                    NSLog("Firmware manifest version=\(manifest.version) hardware=\(manifest.hardware)")
                    self.lastFirmwareManifestCheckAt = Date()
                    self.latestFirmwareManifest = manifest
                    self.clearFirmwareErrors()
                    self.refreshFirmwareAvailability()
                case .failure(let error):
                    NSLog("Firmware manifest check failed: \(error.localizedDescription)")
                    if showErrors {
                        self.setFirmwareError(error.localizedDescription)
                    } else {
                        self.refreshFirmwareAvailability()
                    }
                }
            }
        }
    }

    private func refreshFirmwareAvailability() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.latestVersion = nil
            info.updateAvailable = false
            guard let manifest = latestFirmwareManifest else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let hardware = info.hardware else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let currentVersion = info.currentVersion else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard hardware == manifest.hardware else {
                NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=false reason=hardware_mismatch manifest_hardware=\(manifest.hardware)")
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            info.latestVersion = manifest.version
            info.updateAvailable = FirmwareVersion.isVersion(currentVersion, olderThan: manifest.version)
            firmwareInfoByDeviceID[deviceID] = info
            NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=\(info.updateAvailable)")
            maybeShowFirmwareUpdatePromptAfterPairing(deviceID: deviceID, info: info)
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func maybeShowFirmwareUpdatePromptAfterPairing(deviceID: String, info: DeviceFirmwareInfo) {
        guard
            pendingFirmwareUpdatePromptDeviceIDs.contains(deviceID),
            let currentVersion = info.currentVersion,
            let latestVersion = info.latestVersion
        else {
            return
        }
        guard info.updateAvailable else {
            pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
            return
        }
        pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
        let isBelowMinimum = FirmwareVersion.isVersion(
            currentVersion,
            olderThan: AppConfig.minimumCompatibleFirmwareVersion
        )
        DispatchQueue.main.async { [onFirmwareUpdatePrompt] in
            onFirmwareUpdatePrompt?(deviceID, currentVersion, latestVersion, isBelowMinimum)
        }
    }

    private func setFirmwareChecking(_ isChecking: Bool) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.isChecking = isChecking
            if isChecking {
                info.errorMessage = nil
            }
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func clearFirmwareErrors() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.errorMessage = nil
            firmwareInfoByDeviceID[deviceID] = info
        }
    }

    private func setFirmwareError(_ message: String) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.errorMessage = message
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private var activeSessionID: UInt32? {
        mainInputState.sessionID
    }

    private var activePeripheralID: UUID? {
        mainInputState.peripheralID
    }

    private var activeSessionStartedAt: Date? {
        mainInputState.startedAt
    }

    private func sendUIStateForActiveDevice(_ state: String, text: String = "") {
        ble.sendUIState(state, text: text, to: activePeripheralID)
    }

    private var activeDeviceID: String? {
        activePeripheralID.flatMap { ble.deviceID(for: $0) }
    }

    private func outputProfile(for deviceID: String?) -> OutputProfile {
        config.outputProfile(for: deviceID)
    }

    private func shouldUseDefiniteSegments(for profile: OutputProfile) -> Bool {
        profile.target == .subtitle && config.interactionMode == .clickToTalk
    }

    private func deviceID(for peripheralID: UUID) -> String? {
        ble.deviceID(for: peripheralID)
    }

    /// 调试录音文件名前缀（RC- / VS-，按设备类）。
    private func deviceNamePrefix(for peripheralID: UUID) -> String {
        ble.deviceClass(for: peripheralID) == .xiaomiRemote2Pro ? "RC-" : "VS-"
    }

    // MARK: - 全局热键（对齐 Windows HandleGlobalHotkeyPressed/Released）

    private var hotkeyIsDown = false
    private var hotkeyActiveDeviceID: String?
    private var nextHotkeyRequestID: UInt32 = 1

    /// 热键按下：目标 = 活跃设备（已连接）否则首台已连接 StickS3；
    /// 按住说话模式记录按下态等松开，点击说话模式固件侧按 down 翻转起停。
    func handleGlobalHotkeyPressed() {
        guard !hotkeyIsDown else { return }
        let connected = ble.connectedStickDeviceIDs()
        let target: String?
        if let active = activeDeviceID, connected.contains(active) {
            target = active
        } else {
            target = connected.first
        }
        guard let targetDevice = target else {
            statusController.setStatus(config.pairedDeviceIDs.isEmpty
                ? "Hotkey: pair a VoiceStick first"
                : "Hotkey: VoiceStick not connected; press the main button to wake it")
            return
        }
        let requestID = nextHotkeyRequestID
        nextHotkeyRequestID &+= 1
        if config.interactionMode == .holdToTalk {
            hotkeyIsDown = true
            hotkeyActiveDeviceID = targetDevice
        }
        ble.sendRemoteButton(action: "down", deviceID: targetDevice, requestID: requestID)
        statusController.setStatus("Recording (hotkey) on VS-\(targetDevice)")
    }

    /// 热键松开：仅按住说话模式补发 remote_button_up。
    func handleGlobalHotkeyReleased() {
        guard config.interactionMode == .holdToTalk else { return }
        guard hotkeyIsDown else { return }
        let target = hotkeyActiveDeviceID
        hotkeyIsDown = false
        hotkeyActiveDeviceID = nil
        if let target, ble.isConnected(deviceID: target) {
            let requestID = nextHotkeyRequestID
            nextHotkeyRequestID &+= 1
            ble.sendRemoteButton(action: "up", deviceID: target, requestID: requestID)
        }
    }
}
