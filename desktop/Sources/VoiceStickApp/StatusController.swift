import AppKit

final class StatusController {
    private enum AppStatus {
        case needsPairing
        case listening
        case processing
        case ready
        case error

        init(text: String) {
            let normalized = text.lowercased()
            if normalized.contains("pair") {
                self = .needsPairing
            } else if normalized.contains("listen") {
                self = .listening
            } else if normalized.contains("error") || normalized.contains("failed") {
                self = .error
            } else if normalized.contains("process") || normalized.contains("final") || normalized.contains("transcrib") {
                self = .processing
            } else if normalized.contains("ready") ||
                        normalized.contains("connect") ||
                        normalized.contains("scan") ||
                        normalized.contains("match") ||
                        normalized.contains("pause") ||
                        normalized.contains("no speech") {
                self = .ready
            } else {
                self = .processing
            }
        }

        func symbolName(hasConnectedDevices: Bool) -> String {
            switch self {
            case .needsPairing:
                return "dot.radiowaves.left.and.right"
            case .listening:
                return "mic.fill"
            case .processing:
                return "waveform"
            case .ready:
                if !hasConnectedDevices {
                    return "dot.radiowaves.left.and.right"
                }
                return "link.circle.fill"
            case .error:
                return "exclamationmark.triangle"
            }
        }

        var accessibilityDescription: String {
            switch self {
            case .needsPairing:
                return "Pair VoiceStick"
            case .listening:
                return "Listening"
            case .processing:
                return "Processing"
            case .ready:
                return "Ready"
            case .error:
                return "Error"
            }
        }

        var visibleTitle: String? {
            switch self {
            case .needsPairing:
                return "Pair"
            case .processing:
                return "Processing"
            case .error:
                return "Error"
            case .listening, .ready:
                return nil
            }
        }
    }

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let overlay = OverlayController()

    var onQuit: (() -> Void)?
    var onOpenSettings: (() -> Void)?
    var onPairDevice: (() -> Void)?
    var onForgetDevice: ((String) -> Void)?
    var onUpdateFirmwareDevice: ((String) -> Void)?
    var onCheckFirmwareUpdates: (() -> Void)?
    var onRestoreLastInput: (() -> Bool)?
    var onCheckForUpdates: (() -> Void)? {
        didSet { rebuildMenu() }
    }
    private var needsPairing: Bool
    private var hasRecoverableInput = false
    private var pairedDeviceIDs: [String]
    private var connectedDevices: [ConnectedVoiceStickDevice] = []
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]

    init(pairedDeviceIDs: [String] = []) {
        self.pairedDeviceIDs = pairedDeviceIDs
        self.needsPairing = pairedDeviceIDs.isEmpty
        updateStatusButton(.ready)
        rebuildMenu()
    }

    func setPairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        needsPairing = deviceIDs.isEmpty
        rebuildMenu()
    }

    func setConnectedDevices(_ devices: [ConnectedVoiceStickDevice]) {
        let sortedDevices = devices.sorted { $0.deviceID < $1.deviceID }
        guard connectedDevices.map(\.deviceID) != sortedDevices.map(\.deviceID) ||
                connectedDevices.map(\.name) != sortedDevices.map(\.name) else { return }
        connectedDevices = sortedDevices
        rebuildMenu()
    }

    func setFirmwareInfo(_ infoByDeviceID: [String: DeviceFirmwareInfo]) {
        firmwareInfoByDeviceID = infoByDeviceID
        rebuildMenu()
    }

    func setHasRecoverableInput(_ hasRecoverableInput: Bool) {
        guard self.hasRecoverableInput != hasRecoverableInput else { return }
        self.hasRecoverableInput = hasRecoverableInput
        rebuildMenu()
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if hasRecoverableInput {
            menu.addItem(makeMenuItem(
                title: "Restore Last Input",
                symbolName: "arrow.uturn.backward",
                action: #selector(restoreLastInput)
            ))
            menu.addItem(NSMenuItem.separator())
        }

        addDeviceItems()

        menu.addItem(makeMenuItem(
            title: "Pair Device...",
            symbolName: "dot.radiowaves.left.and.right",
            action: #selector(pairDevice)
        ))

        menu.addItem(makeMenuItem(
            title: "Settings...",
            symbolName: "gearshape",
            action: #selector(openSettings),
            keyEquivalent: ","
        ))

        menu.addItem(NSMenuItem.separator())

        menu.addItem(makeMenuItem(
            title: "Website",
            symbolName: "safari",
            action: #selector(openWebsite)
        ))

        menu.addItem(makeMenuItem(
            title: "Check for Firmware Updates",
            symbolName: "arrow.down.circle",
            action: #selector(checkFirmwareUpdates)
        ))

        if onCheckForUpdates != nil {
            menu.addItem(makeMenuItem(
                title: "Check for App Updates...",
                symbolName: "arrow.triangle.2.circlepath",
                action: #selector(checkForUpdates)
            ))
        }

        menu.addItem(makeMenuItem(
            title: "Quit",
            symbolName: "power",
            action: #selector(quitApp),
            keyEquivalent: "q"
        ))

        statusItem.menu = menu
    }

    private func addDeviceItems() {
        guard !pairedDeviceIDs.isEmpty else { return }

        let connectedByID = Dictionary(uniqueKeysWithValues: connectedDevices.map { ($0.deviceID, $0) })
        for deviceID in pairedDeviceIDs.sorted() {
            let connectedDevice = connectedByID[deviceID]
            let title = connectedDevice?.name ?? "VS-\(deviceID)"
            let deviceItem = makeMenuItem(
                title: title,
                symbolName: connectedDevice == nil ? "link.circle" : "link.circle.fill",
                action: nil
            )
            let submenu = NSMenu()
            let stateItem = NSMenuItem(
                title: connectedDevice == nil ? "Scanning" : "Connected",
                action: nil,
                keyEquivalent: ""
            )
            stateItem.isEnabled = false
            stateItem.image = Self.symbolImage(
                named: connectedDevice == nil ? "antenna.radiowaves.left.and.right" : "checkmark.circle",
                accessibilityDescription: stateItem.title
            )
            submenu.addItem(stateItem)
            submenu.addItem(NSMenuItem.separator())

            addFirmwareItems(to: submenu, deviceID: deviceID, isConnected: connectedDevice != nil)

            let forgetItem = makeMenuItem(
                title: "Forget This Device",
                symbolName: "xmark.circle",
                action: #selector(forgetConnectedDevice)
            )
            forgetItem.representedObject = deviceID
            submenu.addItem(forgetItem)
            deviceItem.submenu = submenu
            menu.addItem(deviceItem)
        }

        menu.addItem(NSMenuItem.separator())
    }

    private func addFirmwareItems(to submenu: NSMenu, deviceID: String, isConnected: Bool) {
        let info = firmwareInfoByDeviceID[deviceID]
        let currentTitle = info?.currentVersion.map { "Firmware \($0)" } ?? "Firmware Unknown"
        let currentItem = NSMenuItem(title: currentTitle, action: nil, keyEquivalent: "")
        currentItem.isEnabled = false
        currentItem.image = Self.symbolImage(named: "info.circle", accessibilityDescription: currentTitle)
        submenu.addItem(currentItem)

        if info?.isChecking == true {
            let checkingItem = NSMenuItem(title: "Checking for Updates", action: nil, keyEquivalent: "")
            checkingItem.isEnabled = false
            checkingItem.image = Self.symbolImage(named: "arrow.triangle.2.circlepath", accessibilityDescription: "Checking")
            submenu.addItem(checkingItem)
            return
        }

        if let errorMessage = info?.errorMessage {
            let errorItem = NSMenuItem(title: "Update Check Failed", action: nil, keyEquivalent: "")
            errorItem.toolTip = errorMessage
            errorItem.isEnabled = false
            errorItem.image = Self.symbolImage(named: "exclamationmark.triangle", accessibilityDescription: "Update Check Failed")
            submenu.addItem(errorItem)
            return
        }

        if info?.updateAvailable == true, let latestVersion = info?.latestVersion {
            let updateItem = makeMenuItem(
                title: "Update to \(latestVersion)...",
                symbolName: "square.and.arrow.down",
                action: #selector(updateFirmwareForDevice)
            )
            updateItem.representedObject = deviceID
            updateItem.isEnabled = isConnected
            submenu.addItem(updateItem)
        } else if info?.latestVersion != nil && info?.currentVersion != nil {
            let upToDateItem = NSMenuItem(title: "Firmware Up to Date", action: nil, keyEquivalent: "")
            upToDateItem.isEnabled = false
            upToDateItem.image = Self.symbolImage(named: "checkmark.circle", accessibilityDescription: "Firmware Up to Date")
            submenu.addItem(upToDateItem)
        }
    }

    func setStatus(_ text: String) {
        DispatchQueue.main.async {
            self.updateStatusButton(AppStatus(text: text))
        }
    }

    func showListening() {
        setStatus("Listening")
        overlay.showListening(text: "")
    }

    func showPartial(_ text: String) {
        setStatus(text.isEmpty ? "Listening" : text)
        overlay.showListening(text: text)
    }

    func showFinal(_ text: String, onHidden: (() -> Void)? = nil) {
        setStatus(text.isEmpty ? "No speech" : "Ready")
        overlay.showFinal(text: text, onHidden: onHidden)
    }

    func showPausedFinal(_ text: String) {
        overlay.showPaused(text: text)
    }

    func showError(_ text: String, onHidden: (() -> Void)? = nil) {
        setStatus("ASR error: \(text)")
        overlay.showError(text, onHidden: onHidden)
    }

    func hideOverlay(onHidden: (() -> Void)? = nil) {
        overlay.hide(onHidden: onHidden)
    }

    private func updateStatusButton(_ status: AppStatus) {
        guard let button = statusItem.button else { return }
        button.image = Self.symbolImage(
            named: status.symbolName(hasConnectedDevices: !connectedDevices.isEmpty),
            accessibilityDescription: status.accessibilityDescription
        )
        button.title = status.visibleTitle ?? ""
        button.imagePosition = status.visibleTitle == nil ? .imageOnly : .imageLeading
        button.toolTip = "VoiceStick: \(status.accessibilityDescription)"
        button.setAccessibilityLabel("VoiceStick: \(status.accessibilityDescription)")
    }

    private func makeMenuItem(
        title: String,
        symbolName: String,
        action: Selector?,
        keyEquivalent: String = ""
    ) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: keyEquivalent)
        item.target = self
        item.image = Self.symbolImage(named: symbolName, accessibilityDescription: title)
        return item
    }

    private static func symbolImage(named name: String, accessibilityDescription: String) -> NSImage? {
        let configuration = NSImage.SymbolConfiguration(pointSize: 14, weight: .regular)
        let image = NSImage(systemSymbolName: name, accessibilityDescription: accessibilityDescription)?
            .withSymbolConfiguration(configuration)
        image?.isTemplate = true
        image?.size = NSSize(width: 16, height: 16)
        return image
    }

    @objc private func openSettings() {
        onOpenSettings?()
    }

    @objc private func pairDevice() {
        onPairDevice?()
    }

    @objc private func forgetConnectedDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onForgetDevice?(deviceID)
    }

    @objc private func updateFirmwareForDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onUpdateFirmwareDevice?(deviceID)
    }

    @objc private func checkFirmwareUpdates() {
        onCheckFirmwareUpdates?()
    }

    @objc private func restoreLastInput() {
        _ = onRestoreLastInput?()
    }

    @objc private func checkForUpdates() {
        onCheckForUpdates?()
    }

    @objc private func openWebsite() {
        NSWorkspace.shared.open(AppConfig.websiteURL)
    }

    @objc private func quitApp() {
        onQuit?()
    }
}
