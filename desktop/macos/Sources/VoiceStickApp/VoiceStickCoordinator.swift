import AppKit
import Foundation

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

        init(peripheralID: UUID, deviceID: String?, sessionID: UInt32, config: AppConfig) {
            self.peripheralID = peripheralID
            self.deviceID = deviceID
            self.sessionID = sessionID
            self.startedAt = Date()
            self.asr = VolcengineASRClient(config: config)
            self.debugAudioRecorder = DebugAudioRecorder(
                enabled: config.debugAudioCache,
                directory: config.debugAudioDirectory
            )
        }

        var duration: TimeInterval {
            Date().timeIntervalSince(startedAt)
        }
    }

    private var config: AppConfig
    private let statusController: StatusController
    private let ble: BleCentral
    private var asr: any ASRClient
    private var translator: LLMTranslationClient
    private let subtitleController = SubtitleController()
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    private let inputInjector = InputInjector()
    private let firmwareManifestClient = FirmwareManifestClient()
    private var debugAudioRecorder: DebugAudioRecorder
    private let minimumRecordingDuration: TimeInterval = 0.5
    private let firmwareManifestCacheDuration: TimeInterval = 24 * 60 * 60

    private var activeSessionID: UInt32?
    private var activePeripheralID: UUID?
    private var activeSessionStartedAt: Date?
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
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
    private var subtitleCycles: [UUID: SubtitleCycle] = [:]
    var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = VolcengineASRClient(config: config)
        self.translator = LLMTranslationClient(config: config)
        self.debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
    }

    func start() {
        ble.onConnectionChange = { [weak self] connectedDevices in
            guard let self else { return }
            self.statusController.setConnectedDevices(connectedDevices)
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            if !connectedDevices.isEmpty {
                self.statusController.setStatus("Ready")
                self.ble.sendInteractionMode(self.config.interactionMode)
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
        }

        ble.onStateEvent = { [weak self] peripheralID, event in
            self?.handleStateEvent(event, peripheralID: peripheralID)
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
        firmwareManifestRefreshTimer?.invalidate()
    }

    func updateConfig(_ config: AppConfig) {
        let wasRecognizing = asrStarted || activeSessionID != nil || isWaitingForFinalText || !subtitleCycles.isEmpty
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
            activeSessionID = nil
            activePeripheralID = nil
            activeSessionStartedAt = nil
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
            subtitleController.hideAll()
            sendUIStateForActiveDevice("ready")
            activePeripheralID = nil
            finishRecognitionCycle()
        }

        self.config = config
        ble.sendInteractionMode(config.interactionMode)
        debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
        asr = VolcengineASRClient(config: config)
        translator = LLMTranslationClient(config: config)
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
                self.sendUIStateForActiveDevice("thinking", text: text)
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

        asr.onUpgradeURL = { url in
            DispatchQueue.main.async {
                let alert = NSAlert()
                alert.messageText = "ASR quota reached"
                alert.informativeText = "Your VoiceStick Cloud quota has been used. Upgrade your account to continue."
                alert.addButton(withTitle: "Upgrade")
                alert.addButton(withTitle: "Cancel")
                if alert.runModal() == .alertFirstButtonReturn {
                    NSWorkspace.shared.open(url)
                }
            }
        }
    }

    private func configureSubtitleASRCallbacks(for cycle: SubtitleCycle) {
        let peripheralID = cycle.peripheralID
        cycle.asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
                guard let self, let cycle = self.subtitleCycles[peripheralID] else { return }
                self.statusController.showPartial(text, deviceID: cycle.deviceID)
                self.ble.sendUIState("thinking", text: text, to: peripheralID)
            }
        }
        cycle.asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
            }
        }
        cycle.asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithFinalText(peripheralID: peripheralID, text: text)
            }
        }
        cycle.asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithError(peripheralID: peripheralID, message: message)
            }
        }
        cycle.asr.onUpgradeURL = { url in
            DispatchQueue.main.async {
                NSWorkspace.shared.open(url)
            }
        }
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
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

    func checkFirmwareUpdatesNow() {
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: true)
    }

    func checkFirmwareAfterPairing(deviceID: String) {
        pendingFirmwareUpdatePromptDeviceIDs.insert(deviceID)
        checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
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
        case "button_down":
            handleButtonDown(event, peripheralID: peripheralID)
        case "button_up":
            handleButtonUp(event, peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleButtonDown(_ event: StateEvent, peripheralID: UUID) {
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
        switch event.button {
        case "primary":
            handlePrimaryButtonUp(peripheralID: peripheralID)
        case "secondary":
            if subtitleCycles[peripheralID] != nil {
                cancelSubtitleCycle(peripheralID: peripheralID, reason: "secondary_cancel")
                return
            }
            cancelPendingPaste(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        if config.defaultOutputProfile.target == .subtitle {
            handleSubtitlePrimaryButtonDown(sessionID: sessionID, peripheralID: peripheralID)
            return
        }
        if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
            return
        }
        if let activePeripheralID {
            if activePeripheralID != peripheralID {
                ble.sendUIState("ready", to: peripheralID)
            }
            NSLog("Ignoring primary button while another recording is active")
            return
        }
        if isWaitingForFinalText {
            ble.sendUIState("ready", to: peripheralID)
            NSLog("Ignoring primary button while previous recording is finalizing")
            return
        }
        guard let sessionID else {
            return
        }

        activeSessionID = sessionID
        activePeripheralID = peripheralID
        activeSessionStartedAt = Date()
        receivedAudioFrames = 0
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        pendingPasteState = .idle
        isShowingASRError = false
        oggMuxer.reset()
        debugAudioRecorder.start(deviceID: deviceID(for: peripheralID), sessionID: sessionID)
        statusController.showListening(deviceID: deviceID(for: peripheralID))
        sendUIStateForActiveDevice("recording")
    }

    private func handlePrimaryButtonUp(peripheralID: UUID) {
        if subtitleCycles[peripheralID] != nil {
            handleSubtitlePrimaryButtonUp(peripheralID: peripheralID)
            return
        }
        guard activeSessionID != nil, activePeripheralID == peripheralID else {
            return
        }
        let recordingDuration = currentRecordingDuration
        let isValidRecording = recordingDuration >= minimumRecordingDuration
        activeSessionID = nil
        activeSessionStartedAt = nil
        if !isValidRecording {
            cancelShortRecording()
        } else if receivedAudioFrames == 0 {
            finishWithASRError("No audio frames from device")
        } else {
            sendUIStateForActiveDevice("thinking")
            sendFinalOggChunkIfNeeded(recordingDuration: recordingDuration)
        }
    }

    private func handleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        if subtitleCycles[peripheralID] != nil {
            handleSubtitleAudioFrame(frame, peripheralID: peripheralID)
            return
        }
        guard frame.sessionID == activeSessionID, activePeripheralID == peripheralID else { return }

        if frame.isEnd && frame.payload.isEmpty {
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
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            activeSessionID = nil
            activeSessionStartedAt = nil
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

    private func handleSubtitlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        guard let sessionID else { return }
        if subtitleCycles[peripheralID] != nil {
            return
        }
        let deviceID = deviceID(for: peripheralID)
        let cycle = SubtitleCycle(
            peripheralID: peripheralID,
            deviceID: deviceID,
            sessionID: sessionID,
            config: config
        )
        configureSubtitleASRCallbacks(for: cycle)
        subtitleCycles[peripheralID] = cycle
        cycle.debugAudioRecorder.start(deviceID: deviceID, sessionID: sessionID)
        statusController.showListening(deviceID: deviceID)
        ble.sendUIState("recording", to: peripheralID)
    }

    private func handleSubtitlePrimaryButtonUp(peripheralID: UUID) {
        guard let cycle = subtitleCycles[peripheralID] else { return }
        if cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
        } else if cycle.receivedAudioFrames == 0 {
            finishSubtitleCycleWithError(peripheralID: peripheralID, message: "No audio frames from device")
        } else {
            ble.sendUIState("thinking", to: peripheralID)
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID)
        }
    }

    private func handleSubtitleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        guard let cycle = subtitleCycles[peripheralID], frame.sessionID == cycle.sessionID else { return }
        if frame.isEnd && frame.payload.isEmpty {
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID)
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
            peripheralID: peripheralID
        )
        if frame.isEnd {
            cycle.sentFinalAudioChunk = true
            if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
                cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            } else if cycle.asrStarted {
                cycle.debugAudioRecorder.finish()
                statusController.setStatus("Processing")
                ble.sendUIState("thinking", to: peripheralID)
            } else {
                cycle.debugAudioRecorder.finish()
                if !startSubtitleASRAndFlushBufferedChunks(peripheralID: peripheralID, lastChunkIsFinal: true) {
                    finishSubtitleCycleWithError(peripheralID: peripheralID, message: "Failed to start ASR")
                    return
                }
                statusController.setStatus("Processing")
                ble.sendUIState("thinking", to: peripheralID)
            }
        }
    }

    private func sendSubtitleFinalOggChunkIfNeeded(peripheralID: UUID) {
        guard let cycle = subtitleCycles[peripheralID], !cycle.sentFinalAudioChunk else { return }
        cycle.sentFinalAudioChunk = true
        if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            return
        }
        let finalChunk = cycle.oggMuxer.finish()
        cycle.debugAudioRecorder.append(finalChunk)
        cycle.debugAudioRecorder.finish()
        sendOrBufferSubtitleOggChunk(finalChunk, isLast: true, canStartASR: true, peripheralID: peripheralID)
        statusController.setStatus("Processing")
        ble.sendUIState("thinking", to: peripheralID)
    }

    private func sendOrBufferSubtitleOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool, peripheralID: UUID) {
        guard let cycle = subtitleCycles[peripheralID] else { return }
        if cycle.asrStarted {
            cycle.asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }
        cycle.bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startSubtitleASRAndFlushBufferedChunks(peripheralID: peripheralID, lastChunkIsFinal: isLast) {
            finishSubtitleCycleWithError(peripheralID: peripheralID, message: "Failed to start ASR")
        }
    }

    private func startSubtitleASRAndFlushBufferedChunks(peripheralID: UUID, lastChunkIsFinal: Bool) -> Bool {
        guard let cycle = subtitleCycles[peripheralID] else { return false }
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
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            activeSessionID = nil
            activeSessionStartedAt = nil
            cancelShortRecording()
            return
        }

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        activeSessionID = nil
        activeSessionStartedAt = nil
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
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
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asr.cancel()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        debugAudioRecorder.discard()
        statusController.setStatus("Ready")
        statusController.hideOverlay()
        sendUIStateForActiveDevice("ready")
        activePeripheralID = nil
    }

    private func finishWithFinalText(_ text: String) {
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
            activePeripheralID = nil
            return
        }
        if text.isEmpty {
            pastedFinalText = true
            pendingPasteState = .idle
            statusController.showFinal(text, deviceID: activeDeviceID) { [weak self] in
                self?.finishRecognitionCycle()
                self?.sendUIStateForActiveDevice("ready")
                self?.activePeripheralID = nil
            }
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
        guard segment.definite, let cycle = subtitleCycles[peripheralID] else { return }
        let profile = outputProfile(for: cycle.deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: cycle.deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: cycle.deviceID)
    }

    private func finishSubtitleCycleWithFinalText(peripheralID: UUID, text: String) {
        guard let cycle = subtitleCycles[peripheralID], !cycle.finishedFinalText else { return }
        cycle.finishedFinalText = true
        let profile = outputProfile(for: cycle.deviceID)
        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showSubtitleText(text, profile: profile, deviceID: cycle.deviceID)
        }
        finishSubtitleCycle(peripheralID: peripheralID, hideOverlay: true)
    }

    private func finishSubtitleCycleWithError(peripheralID: UUID, message: String) {
        guard let cycle = subtitleCycles[peripheralID] else { return }
        NSLog("ASR error VS-\(cycle.deviceID ?? "unknown"): \(message)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        statusController.showError(message, deviceID: cycle.deviceID) { [weak self] in
            self?.ble.sendUIState("ready", to: peripheralID)
        }
        subtitleCycles.removeValue(forKey: peripheralID)
    }

    private func cancelSubtitleCycle(peripheralID: UUID, reason: String) {
        guard let cycle = subtitleCycles[peripheralID] else { return }
        NSLog("Cancel subtitle cycle VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        statusController.hideOverlay(deviceID: cycle.deviceID)
        ble.sendUIState("ready", to: peripheralID)
        subtitleCycles.removeValue(forKey: peripheralID)
    }

    private func finishSubtitleCycle(peripheralID: UUID, hideOverlay: Bool) {
        guard let cycle = subtitleCycles[peripheralID] else { return }
        if hideOverlay {
            statusController.hideOverlay(deviceID: cycle.deviceID)
        }
        statusController.setStatus("Ready")
        ble.sendUIState("ready", to: peripheralID)
        subtitleCycles.removeValue(forKey: peripheralID)
    }

    private func showSubtitleText(_ text: String, profile: OutputProfile, deviceID: String?) {
        guard let deviceID else { return }
        transformText(text, profile: profile, deviceID: deviceID) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let outputText):
                self.subtitleController.show(
                    text: outputText,
                    deviceID: deviceID,
                    color: self.config.themeColor(for: deviceID)
                )
            case .failure(let error):
                self.statusController.showError(error.localizedDescription, deviceID: deviceID)
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
        asr.cancel()
        pendingPasteState = .idle
        activeSessionID = nil
        activeSessionStartedAt = nil
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        isShowingASRError = true
        errorRecoveryToken += 1
        let token = errorRecoveryToken
        sendUIStateForActiveDevice("error", text: message)
        statusController.showError(message, deviceID: activeDeviceID) { [weak self] in
            guard let self, self.errorRecoveryToken == token else { return }
            self.recoverFromASRError(hideOverlay: false)
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
            activePeripheralID = nil
        }
    }

    private func commitPendingPaste(text: String) {
        guard pendingPasteText == text else {
            return
        }

        completePendingPaste(text: text)
    }

    private func completePendingPaste(text: String) {
        pendingPasteState = .idle
        finishRecognitionCycle()
        inputInjector.paste(text: text, pressEnter: config.autoEnter)
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        activePeripheralID = nil
    }

    private var isWaitingForFinalText: Bool {
        activeSessionID == nil &&
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
            activeSessionID == nil,
            !isWaitingForFinalText,
            let text = lastRecoverableText,
            !text.isEmpty
        else {
            return false
        }

        activePeripheralID = peripheralID
        pendingPasteState = .paused(text: text)
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
        activePeripheralID = nil
    }

    private func cancelRecognitionInProgress() {
        activeSessionID = nil
        activeSessionStartedAt = nil
        asr.cancel()
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        activePeripheralID = nil
    }

    private func cancelActiveCycleIfDeviceDisconnected() {
        let disconnectedSubtitlePeripheralIDs = subtitleCycles.keys.filter { !ble.isConnected($0) }
        for peripheralID in disconnectedSubtitlePeripheralIDs {
            guard let cycle = subtitleCycles[peripheralID] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            statusController.hideOverlay(deviceID: cycle.deviceID)
            subtitleCycles.removeValue(forKey: peripheralID)
        }
        guard let activePeripheralID, !ble.isConnected(activePeripheralID) else { return }
        asr.cancel()
        pendingPasteState = .idle
        activeSessionID = nil
        self.activePeripheralID = nil
        activeSessionStartedAt = nil
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        statusController.hideOverlay()
        subtitleController.hideAll()
    }

    private func finishRecognitionCycle() {
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
}
