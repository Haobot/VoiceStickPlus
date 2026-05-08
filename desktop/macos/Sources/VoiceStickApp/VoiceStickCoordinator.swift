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

    private var config: AppConfig
    private let statusController: StatusController
    private let ble: BleCentral
    private var asr: any ASRClient
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
    var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = VolcengineASRClient(config: config)
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
        let wasRecognizing = asrStarted || activeSessionID != nil || isWaitingForFinalText
        if wasRecognizing {
            asr.onPartial = nil
            asr.onFinal = nil
            asr.onError = nil
            asr.onUpgradeURL = nil
            asr.cancel()
            activeSessionID = nil
            activePeripheralID = nil
            activeSessionStartedAt = nil
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
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
            cancelPendingPaste(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
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
        guard asr.start(options: ASRSessionOptions(hotwords: config.asrHotwords)) else {
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
        guard let activePeripheralID, !ble.isConnected(activePeripheralID) else { return }
        asr.cancel()
        pendingPasteState = .idle
        activeSessionID = nil
        self.activePeripheralID = nil
        activeSessionStartedAt = nil
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        statusController.hideOverlay()
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

    private func deviceID(for peripheralID: UUID) -> String? {
        ble.deviceID(for: peripheralID)
    }
}
