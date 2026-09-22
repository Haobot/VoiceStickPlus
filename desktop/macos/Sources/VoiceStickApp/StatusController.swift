import AppKit
import VoiceStickCore

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
                return tr(.statusPairVoiceStick)
            case .listening:
                return tr(.statusListening)
            case .processing:
                return tr(.statusProcessing)
            case .ready:
                return tr(.statusReady)
            case .error:
                return tr(.statusError)
            }
        }

        var visibleTitle: String? {
            switch self {
            case .needsPairing:
                return tr(.statusPair)
            case .processing:
                return tr(.statusProcessing)
            case .error:
                return tr(.statusError)
            case .listening, .ready:
                return nil
            }
        }
    }

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private var overlays: [String: OverlayController] = [:]
    private var visibleOverlayKeys: Set<String> = []

    var onQuit: (() -> Void)?
    var onOpenSettings: (() -> Void)?
    var onPairDevice: (() -> Void)?
    var onForgetDevice: ((String) -> Void)?
    var onUpdateFirmwareDevice: ((String) -> Void)?
    var onOpenInteractionSettings: ((String) -> Void)?
    var onOpenEncoderSettings: ((String) -> Void)?
    var onOpenRemoteSettings: ((String) -> Void)?
    var onOpenButtonMapping: ((String) -> Void)?
    var onOpenBatteryMonitor: ((String) -> Void)?
    var onUpdateFirmwareFromFile: ((String) -> Void)?
    var onSetDeviceThemeColor: ((String, OverlayThemeColor) -> Void)?
    var onSetDeviceOverlayPosition: ((String, OverlayPosition) -> Void)?
    var onSetDeviceThemeSize: ((String, OverlayThemeSize) -> Void)?
    var onRestoreLastInput: (() -> Bool)?
    var onSetInteractionMode: ((InteractionMode) -> Void)?
    var onSetAutoEnter: ((Bool) -> Void)?
    var onSetGlobalHotkeyEnabled: ((Bool) -> Void)?
    var onSetGlobalHotkey: ((String) -> Void)?
    var onCustomizeHotkey: (() -> Void)?
    var onSetLaunchAtLogin: ((Bool) -> Void)?
    var onSetDefaultOutputProfile: ((OutputProfile) -> Void)?
    var onSetDeviceOutputProfile: ((String, OutputProfile) -> Void)?
    var onCheckForUpdates: (() -> Void)? {
        didSet { rebuildMenu() }
    }
    /// 未连接配对条目的 hardware 查询（AppDelegate 注入，实时读 config）：
    /// 推导设备类别——RC 设备隐藏固件菜单、fallback 名用 RC- 前缀。
    var hardwareProvider: ((String) -> String?)? {
        didSet { rebuildMenu() }
    }
    private var needsPairing: Bool
    private var hasRecoverableInput = false
    private var pairedDeviceIDs: [String]
    private var deviceThemeColors: [String: OverlayThemeColor]
    private var deviceOverlayPositions: [String: OverlayPosition]
    private var deviceThemeSizes: [String: OverlayThemeSize]
    private var connectedDevices: [ConnectedVoiceStickDevice] = []
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    /// 编码器在位状态（固件 encoder_status 事件驱动；未上报过的设备默认 true，
    /// 对齐 Windows UiState::SetDeviceEncoderPresent 的默认语义，供菜单 gating）。
    private(set) var encoderPresentByDeviceID: [String: Bool] = [:]
    /// 电量状态（固件 battery_status 事件 / 小米 0x2A19 合成事件驱动），供菜单标题后缀。
    private(set) var batteryByDeviceID: [String: (level: Int, charging: Bool, usbPowered: Bool)] = [:]
    private var interactionMode: InteractionMode
    private var autoEnter: Bool
    private var defaultOutputProfile: OutputProfile
    private var deviceOutputProfiles: [String: OutputProfile]
    private var globalHotkeyEnabled: Bool
    private var globalHotkey: String
    private var launchAtLogin: Bool
    /// 最近一次状态栏状态（语言切换时按它重建按钮文案）。
    private var currentStatus: AppStatus = .ready

    init(pairedDeviceIDs: [String] = [],
         deviceThemeColors: [String: OverlayThemeColor] = [:],
         deviceOverlayPositions: [String: OverlayPosition] = [:],
         deviceThemeSizes: [String: OverlayThemeSize] = [:],
         interactionMode: InteractionMode = .holdToTalk,
         autoEnter: Bool = true,
         defaultOutputProfile: OutputProfile = .default,
         deviceOutputProfiles: [String: OutputProfile] = [:],
         globalHotkeyEnabled: Bool = true,
         globalHotkey: String = "Alt+X",
         launchAtLogin: Bool = true) {
        self.pairedDeviceIDs = pairedDeviceIDs
        self.deviceThemeColors = deviceThemeColors
        self.deviceOverlayPositions = deviceOverlayPositions
        self.deviceThemeSizes = deviceThemeSizes
        self.interactionMode = interactionMode
        self.autoEnter = autoEnter
        self.defaultOutputProfile = defaultOutputProfile
        self.deviceOutputProfiles = deviceOutputProfiles
        self.globalHotkeyEnabled = globalHotkeyEnabled
        self.globalHotkey = globalHotkey
        self.launchAtLogin = launchAtLogin
        self.needsPairing = pairedDeviceIDs.isEmpty
        updateStatusButton(.ready)
        rebuildMenu()
    }

    func setPairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        deviceThemeColors = deviceThemeColors.filter { deviceIDs.contains($0.key) }
        deviceOverlayPositions = deviceOverlayPositions.filter { deviceIDs.contains($0.key) }
        deviceThemeSizes = deviceThemeSizes.filter { deviceIDs.contains($0.key) }
        deviceOutputProfiles = deviceOutputProfiles.filter { deviceIDs.contains($0.key) }
        needsPairing = deviceIDs.isEmpty
        rebuildMenu()
    }

    func setDeviceThemeColors(_ colors: [String: OverlayThemeColor]) {
        deviceThemeColors = colors.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    func setDeviceOverlayPositions(_ positions: [String: OverlayPosition]) {
        deviceOverlayPositions = positions.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    func setDeviceThemeSizes(_ sizes: [String: OverlayThemeSize]) {
        deviceThemeSizes = sizes.filter { pairedDeviceIDs.contains($0.key) }
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

    /// 编码器在位上报（对齐 Windows UiState::SetDeviceEncoderPresent）。
    func setDeviceEncoderPresent(_ deviceID: String, present: Bool) {
        guard encoderPresentByDeviceID[deviceID] != present else { return }
        encoderPresentByDeviceID[deviceID] = present
        rebuildMenu()
    }

    /// 电量上报（对齐 Windows UiState::SetDeviceBattery）。
    func setDeviceBattery(_ deviceID: String, level: Int, charging: Bool, usbPowered: Bool) {
        let value = (level: level, charging: charging, usbPowered: usbPowered)
        let old = batteryByDeviceID[deviceID]
        guard old?.level != value.level || old?.charging != value.charging ||
                old?.usbPowered != value.usbPowered else { return }
        batteryByDeviceID[deviceID] = value
        rebuildMenu()
    }

    func setHasRecoverableInput(_ hasRecoverableInput: Bool) {
        guard self.hasRecoverableInput != hasRecoverableInput else { return }
        self.hasRecoverableInput = hasRecoverableInput
        rebuildMenu()
    }

    func setInputOptions(interactionMode: InteractionMode, autoEnter: Bool) {
        guard self.interactionMode != interactionMode || self.autoEnter != autoEnter else { return }
        self.interactionMode = interactionMode
        self.autoEnter = autoEnter
        rebuildMenu()
    }

    func setDefaultOutputProfile(_ profile: OutputProfile) {
        guard defaultOutputProfile != profile else { return }
        defaultOutputProfile = profile
        rebuildMenu()
    }

    func setDeviceOutputProfiles(_ profiles: [String: OutputProfile]) {
        deviceOutputProfiles = profiles.filter { pairedDeviceIDs.contains($0.key) }
        rebuildMenu()
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if hasRecoverableInput {
            menu.addItem(makeMenuItem(
                title: tr(.menuRestoreLastInput),
                symbolName: "arrow.uturn.backward",
                action: #selector(restoreLastInput)
            ))
            menu.addItem(NSMenuItem.separator())
        }

        addDeviceItems()

        addInputItems()

        menu.addItem(makeMenuItem(
            title: tr(.menuPairDevice),
            symbolName: "dot.radiowaves.left.and.right",
            action: #selector(pairDevice)
        ))

        menu.addItem(makeMenuItem(
            title: tr(.menuSettings),
            symbolName: "gearshape",
            action: #selector(openSettings),
            keyEquivalent: ","
        ))

        menu.addItem(NSMenuItem.separator())

        menu.addItem(makeMenuItem(
            title: tr(.menuWebsite),
            symbolName: "safari",
            action: #selector(openWebsite)
        ))

        if onCheckForUpdates != nil {
            menu.addItem(makeMenuItem(
                title: tr(.menuCheckAppUpdates),
                symbolName: "arrow.triangle.2.circlepath",
                action: #selector(checkForUpdates)
            ))
        }

        menu.addItem(makeMenuItem(
            title: tr(.menuQuit),
            symbolName: "power",
            action: #selector(quitApp),
            keyEquivalent: "q"
        ))

        statusItem.menu = menu
    }

    private func addInputItems() {
        addOutputItems()

        let afterPasteItem = makeMenuItem(
            title: tr(.menuPressReturnAfterPaste),
            symbolName: "return",
            action: #selector(toggleAutoEnter)
        )
        afterPasteItem.state = autoEnter ? .on : .off
        menu.addItem(afterPasteItem)

        // 开机自启动（SMAppService 需 macOS 13+；12 隐藏该开关）。
        if #available(macOS 13.0, *) {
            let launchItem = makeMenuItem(
                title: tr(.menuLaunchAtLogin),
                symbolName: "sunrise",
                action: #selector(toggleLaunchAtLogin)
            )
            launchItem.state = launchAtLogin ? .on : .off
            menu.addItem(launchItem)
        }

        addHotkeyItems()

        let interactionItem = makeMenuItem(
            title: tr(.menuInteraction),
            symbolName: "hand.tap",
            action: nil
        )
        let interactionSubmenu = NSMenu()
        let holdItem = makeMenuItem(
            title: tr(.menuHoldToTalk),
            symbolName: "hand.tap",
            action: #selector(selectInteractionMode)
        )
        holdItem.representedObject = InteractionMode.holdToTalk.rawValue
        holdItem.state = interactionMode == .holdToTalk ? .on : .off
        interactionSubmenu.addItem(holdItem)

        let clickItem = makeMenuItem(
            title: tr(.menuClickToTalk),
            symbolName: "cursorarrow.click",
            action: #selector(selectInteractionMode)
        )
        clickItem.representedObject = InteractionMode.clickToTalk.rawValue
        clickItem.state = interactionMode == .clickToTalk ? .on : .off
        interactionSubmenu.addItem(clickItem)

        interactionItem.submenu = interactionSubmenu
        menu.addItem(interactionItem)
        menu.addItem(NSMenuItem.separator())
    }

    private func addHotkeyItems() {
        // 热键子菜单（对齐 Windows 托盘：启用勾选 + 预设 radio + 自定义捕获）。
        let hotkeyItem = makeMenuItem(title: tr(.menuHotkey), symbolName: "keyboard", action: nil)
        let hotkeySubmenu = NSMenu()
        let enableItem = NSMenuItem(
            title: tr(.menuEnableHotkey),
            action: #selector(toggleGlobalHotkey),
            keyEquivalent: ""
        )
        enableItem.target = self
        enableItem.state = globalHotkeyEnabled ? .on : .off
        hotkeySubmenu.addItem(enableItem)
        hotkeySubmenu.addItem(NSMenuItem.separator())
        for preset in GlobalHotkeyManager.presets {
            let item = NSMenuItem(
                title: GlobalHotkeyManager.displayName(for: preset),
                action: #selector(selectGlobalHotkeyPreset),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = preset
            item.state = globalHotkeyEnabled && globalHotkey == preset ? .on : .off
            hotkeySubmenu.addItem(item)
        }
        let isCustom = !GlobalHotkeyManager.presets.contains(globalHotkey)
        let customTitle = isCustom
            ? tr(.menuCustomHotkeyNamed, GlobalHotkeyManager.displayName(for: globalHotkey))
            : tr(.menuCustomHotkey)
        let customItem = NSMenuItem(
            title: customTitle,
            action: #selector(customizeGlobalHotkey),
            keyEquivalent: ""
        )
        customItem.target = self
        customItem.state = globalHotkeyEnabled && isCustom ? .on : .off
        hotkeySubmenu.addItem(customItem)
        hotkeyItem.submenu = hotkeySubmenu
        menu.addItem(hotkeyItem)
    }

    private func addOutputItems() {
        let outputItem = makeMenuItem(
            title: tr(.menuOutput),
            symbolName: "text.bubble",
            action: nil
        )
        let outputSubmenu = NSMenu()
        for target in OutputTarget.allCases {
            let item = NSMenuItem(
                title: target.displayName,
                action: #selector(selectOutputTarget),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = target.rawValue
            item.state = defaultOutputProfile.target == target ? .on : .off
            outputSubmenu.addItem(item)
        }
        outputItem.submenu = outputSubmenu
        menu.addItem(outputItem)
    }

    private func addDeviceItems() {
        guard !pairedDeviceIDs.isEmpty else { return }

        let connectedByID = Dictionary(uniqueKeysWithValues: connectedDevices.map { ($0.deviceID, $0) })
        for deviceID in pairedDeviceIDs.sorted() {
            let connectedDevice = connectedByID[deviceID]
            // 设备类别：已连接用连接态类别；未连接的配对条目按 config hardware 推导。
            let isXiaomi = connectedDevice.map { $0.deviceClass == .xiaomiRemote2Pro }
                ?? (hardwareProvider?(deviceID) == PairedDeviceEntry.hardwareXiaomiRemote2Pro)
            var title = connectedDevice?.name ?? (isXiaomi ? "RC-\(deviceID)" : "VS-\(deviceID)")
            // 电量后缀（对齐 Windows DeviceTitleWithBattery/BatteryStatusText）：
            // " (NN%)" + 充电（优先）/外接电源后缀。仅在有上报时追加。
            if let battery = batteryByDeviceID[deviceID] {
                let batteryText = Localization.batteryStatusText(
                    level: battery.level, charging: battery.charging, usbPowered: battery.usbPowered)
                title += " (\(batteryText))"
            }
            let deviceItem = makeMenuItem(
                title: title,
                symbolName: connectedDevice == nil ? "link.circle" : "link.circle.fill",
                action: nil
            )
            let submenu = NSMenu()
            let stateItem = NSMenuItem(
                title: connectedDevice == nil ? tr(.stateScanning) : tr(.stateConnected),
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

            addThemeColorItems(to: submenu, deviceID: deviceID)
            addThemeSizeItems(to: submenu, deviceID: deviceID)
            addOverlayPositionItems(to: submenu, deviceID: deviceID)
            addDeviceTextItems(to: submenu, deviceID: deviceID)
            submenu.addItem(NSMenuItem.separator())

            if isXiaomi {
                // 遥控器设置（仅小米；对齐 Windows kMenuRemoteSettings）。
                let remoteItem = makeMenuItem(
                    title: tr(.menuRemoteSettings),
                    symbolName: "gearshape",
                    action: #selector(openRemoteSettings)
                )
                remoteItem.representedObject = deviceID
                submenu.addItem(remoteItem)

                // 按键映射（仅小米；对齐 Windows kMenuXiaomiKeymap）。
                let buttonMappingItem = makeMenuItem(
                    title: tr(.menuButtonMapping),
                    symbolName: "keyboard",
                    action: #selector(openButtonMapping)
                )
                buttonMappingItem.representedObject = deviceID
                submenu.addItem(buttonMappingItem)
            } else {
                // 设备交互设置（仅 StickS3；对齐 Windows kMenuInteractionSettings）。
                let interactionItem = makeMenuItem(
                    title: tr(.menuInteractionSettings),
                    symbolName: "hand.tap",
                    action: #selector(openInteractionSettings)
                )
                interactionItem.representedObject = deviceID
                submenu.addItem(interactionItem)

                // 编码器设置（encoderPresent 未上报过默认 true，对齐 Windows gating）。
                if encoderPresentByDeviceID[deviceID] ?? true {
                    let encoderItem = makeMenuItem(
                        title: tr(.menuEncoderSettings),
                        symbolName: "dial.medium",
                        action: #selector(openEncoderSettings)
                    )
                    encoderItem.representedObject = deviceID
                    submenu.addItem(encoderItem)
                }

                // 电池电压监测（仅已连接；对齐 Windows kMenuBatteryMonitor）。
                if connectedDevice != nil {
                    let batteryItem = makeMenuItem(
                        title: tr(.menuBatteryMonitor),
                        symbolName: "battery.100.bolt",
                        action: #selector(openBatteryMonitor)
                    )
                    batteryItem.representedObject = deviceID
                    submenu.addItem(batteryItem)
                }
            }

            // 小米遥控器没有 VoiceStick 固件概念：隐藏固件/更新区块，其余菜单照常。
            if !isXiaomi {
                addFirmwareItems(to: submenu, deviceID: deviceID, isConnected: connectedDevice != nil)
                // 本地文件固件更新（仅已连接；对齐 Windows kMenuUpdateFirmwareFromFile）。
                if connectedDevice != nil {
                    let fromFileItem = makeMenuItem(
                        title: tr(.menuUpdateFirmwareFromFile),
                        symbolName: "doc.zipper",
                        action: #selector(updateFirmwareFromFile)
                    )
                    fromFileItem.representedObject = deviceID
                    submenu.addItem(fromFileItem)
                }
            }

            let forgetItem = makeMenuItem(
                title: tr(.menuForgetDevice),
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

    private func addThemeColorItems(to submenu: NSMenu, deviceID: String) {
        let currentColor = deviceThemeColors[deviceID] ?? .auto
        let themeItem = makeMenuItem(
            title: tr(.menuThemeColor),
            symbolName: "paintpalette",
            action: nil
        )
        let themeSubmenu = NSMenu()
        for color in OverlayThemeColor.allCases {
            let colorItem = NSMenuItem(
                title: color.displayName,
                action: #selector(selectDeviceThemeColor),
                keyEquivalent: ""
            )
            colorItem.target = self
            colorItem.representedObject = "\(deviceID):\(color.rawValue)"
            colorItem.state = currentColor == color ? .on : .off
            themeSubmenu.addItem(colorItem)
        }
        themeItem.submenu = themeSubmenu
        submenu.addItem(themeItem)
    }

    private func addThemeSizeItems(to submenu: NSMenu, deviceID: String) {
        let currentSize = deviceThemeSizes[deviceID] ?? .big
        let sizeItem = makeMenuItem(
            title: tr(.menuThemeSize),
            symbolName: "textformat.size",
            action: nil
        )
        let sizeSubmenu = NSMenu()
        for size in OverlayThemeSize.allCases {
            let item = NSMenuItem(
                title: size.displayName,
                action: #selector(selectDeviceThemeSize),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = "\(deviceID):\(size.rawValue)"
            item.state = currentSize == size ? .on : .off
            sizeSubmenu.addItem(item)
        }
        sizeItem.submenu = sizeSubmenu
        submenu.addItem(sizeItem)
    }

    private func addOverlayPositionItems(to submenu: NSMenu, deviceID: String) {
        let currentPosition = deviceOverlayPositions[deviceID] ?? .bottomCenter
        let positionItem = makeMenuItem(
            title: tr(.menuOverlayPosition),
            symbolName: "rectangle.inset.filled",
            action: nil
        )
        let positionSubmenu = NSMenu()
        for position in OverlayPosition.allCases {
            let item = NSMenuItem(
                title: position.displayName,
                action: #selector(selectDeviceOverlayPosition),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = "\(deviceID):\(position.rawValue)"
            item.state = currentPosition == position ? .on : .off
            positionSubmenu.addItem(item)
        }
        positionItem.submenu = positionSubmenu
        submenu.addItem(positionItem)
    }

    private func addDeviceTextItems(to submenu: NSMenu, deviceID: String) {
        let profile = outputProfile(for: deviceID)
        let textItem = makeMenuItem(
            title: tr(.menuTranslation),
            symbolName: "textformat",
            action: nil
        )
        let textSubmenu = NSMenu()
        let originalItem = NSMenuItem(
            title: tr(.textOriginal),
            action: #selector(selectDeviceTextMode),
            keyEquivalent: ""
        )
        originalItem.target = self
        originalItem.representedObject = "\(deviceID):original:"
        originalItem.state = profile.transform == .original ? .on : .off
        textSubmenu.addItem(originalItem)
        textSubmenu.addItem(NSMenuItem.separator())

        for language in Self.translationTargets {
            let item = NSMenuItem(
                title: tr(.menuTranslateTo, Localization.translationTargetName(
                    code: language.code, englishName: language.name)),
                action: #selector(selectDeviceTextMode),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = "\(deviceID):translate:\(language.code)"
            item.state = profile.transform == .translate && profile.translationTarget == language.code ? .on : .off
            textSubmenu.addItem(item)
        }
        textItem.submenu = textSubmenu
        submenu.addItem(textItem)
    }

    private func addFirmwareItems(to submenu: NSMenu, deviceID: String, isConnected: Bool) {
        let info = firmwareInfoByDeviceID[deviceID]
        // 当前固件行保持英文（对齐 Windows FirmwareIdentityText：该信息行不本地化）。
        let currentTitle = info?.currentVersion.map { "Firmware \($0)" } ?? "Firmware Unknown"
        let currentItem = NSMenuItem(title: currentTitle, action: nil, keyEquivalent: "")
        currentItem.isEnabled = false
        currentItem.image = Self.symbolImage(named: "info.circle", accessibilityDescription: currentTitle)
        submenu.addItem(currentItem)

        if info?.isChecking == true {
            let checkingItem = NSMenuItem(title: tr(.firmwareCheckingUpdates), action: nil, keyEquivalent: "")
            checkingItem.isEnabled = false
            checkingItem.image = Self.symbolImage(named: "arrow.triangle.2.circlepath", accessibilityDescription: checkingItem.title)
            submenu.addItem(checkingItem)
            return
        }

        if let errorMessage = info?.errorMessage {
            let errorItem = NSMenuItem(title: tr(.firmwareUpdateCheckFailed), action: nil, keyEquivalent: "")
            errorItem.toolTip = errorMessage
            errorItem.isEnabled = false
            errorItem.image = Self.symbolImage(named: "exclamationmark.triangle", accessibilityDescription: errorItem.title)
            submenu.addItem(errorItem)
            return
        }

        if info?.updateAvailable == true, let latestVersion = info?.latestVersion {
            let updateItem = makeMenuItem(
                title: tr(.firmwareUpdateTo, latestVersion),
                symbolName: "square.and.arrow.down",
                action: #selector(updateFirmwareForDevice)
            )
            updateItem.representedObject = deviceID
            updateItem.isEnabled = isConnected
            submenu.addItem(updateItem)
        } else if info?.latestVersion != nil && info?.currentVersion != nil {
            let upToDateItem = NSMenuItem(title: tr(.menuFirmwareUpToDate), action: nil, keyEquivalent: "")
            upToDateItem.isEnabled = false
            upToDateItem.image = Self.symbolImage(named: "checkmark.circle", accessibilityDescription: upToDateItem.title)
            submenu.addItem(upToDateItem)
        }
    }

    func setStatus(_ text: String) {
        DispatchQueue.main.async {
            self.updateStatusButton(AppStatus(text: text))
        }
    }

    func showListening(deviceID: String? = nil) {
        setStatus("Listening")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showListening(text: "")
    }

    func showPartial(_ text: String, deviceID: String? = nil) {
        setStatus(text.isEmpty ? "Listening" : text)
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showPartial(text)
    }

    /// 进入精修态（三点跳动指示器 + ASR 原文）。
    func showRefining(_ text: String, deviceID: String? = nil) {
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showRefining(text)
    }

    /// 流式精修追加（无滚动过渡动画）。
    func appendPartial(_ text: String, deviceID: String? = nil) {
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.appendPartial(text)
    }

    /// 中性信息（圆点指示器），自定义时长自动隐藏。
    func showTimedMessage(_ text: String, duration: TimeInterval, deviceID: String? = nil,
                          onHidden: (() -> Void)? = nil) {
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showTimedMessage(text, duration: duration, onHidden: { [weak self] in
            self?.markOverlayHidden(for: deviceID)
            onHidden?()
        })
    }

    func showFinal(_ text: String, deviceID: String? = nil, onHidden: (() -> Void)? = nil) {
        setStatus(text.isEmpty ? "No speech" : "Ready")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showFinal(text: text, onHidden: { [weak self] in
            self?.markOverlayHidden(for: deviceID)
            onHidden?()
        })
    }

    func showPausedFinal(_ text: String, deviceID: String? = nil) {
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showPausedFinal(text: text)
    }

    func showError(_ text: String, deviceID: String? = nil, onHidden: (() -> Void)? = nil) {
        setStatus("ASR error: \(text)")
        let overlay = overlay(for: deviceID)
        markOverlayVisible(for: deviceID)
        applyOverlayStyle(for: deviceID, overlay: overlay)
        overlay.showError(text, onHidden: { [weak self] in
            self?.markOverlayHidden(for: deviceID)
            onHidden?()
        })
    }

    func hideOverlay(onHidden: (() -> Void)? = nil) {
        let overlayList = overlays.values
        visibleOverlayKeys.removeAll()
        var remaining = overlayList.count
        guard remaining > 0 else {
            onHidden?()
            return
        }
        for overlay in overlayList {
            overlay.hide {
                remaining -= 1
                if remaining == 0 {
                    onHidden?()
                }
            }
        }
    }

    func hideOverlay(deviceID: String?, onHidden: (() -> Void)? = nil) {
        let key = overlayKey(for: deviceID)
        visibleOverlayKeys.remove(key)
        updateOverlayStackIndices()
        guard let overlay = overlays[key] else {
            onHidden?()
            return
        }
        overlay.hide(onHidden: onHidden)
    }

    private func updateStatusButton(_ status: AppStatus) {
        currentStatus = status
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

    /// 界面语言切换后重建菜单与状态栏按钮文案（tr 均在调用时读当前语言，不缓存）。
    func refreshLocalization() {
        updateStatusButton(currentStatus)
        rebuildMenu()
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

    private static let translationTargets: [(code: String, name: String)] = [
        ("en", "English"),
        ("zh-Hans", "Chinese (Simplified)"),
        ("zh-Hant", "Chinese (Traditional)"),
        ("ja", "Japanese"),
        ("ko", "Korean"),
        ("ru", "Russian"),
        ("fr", "French"),
        ("de", "German"),
        ("es", "Spanish"),
        ("it", "Italian"),
        ("pt", "Portuguese"),
        ("nl", "Dutch"),
        ("sv", "Swedish"),
        ("pl", "Polish"),
        ("tr", "Turkish"),
        ("ar", "Arabic"),
        ("hi", "Hindi"),
        ("id", "Indonesian"),
        ("vi", "Vietnamese"),
        ("th", "Thai")
    ]

    private func themeColor(for deviceID: String?) -> OverlayThemeColor {
        guard let deviceID else { return .auto }
        return deviceThemeColors[AppConfig.normalizedDeviceID(deviceID)] ?? .auto
    }

    private func overlayPosition(for deviceID: String?) -> OverlayPosition {
        guard let deviceID else { return .bottomCenter }
        return deviceOverlayPositions[AppConfig.normalizedDeviceID(deviceID)] ?? .bottomCenter
    }

    private func themeSize(for deviceID: String?) -> OverlayThemeSize {
        guard let deviceID else { return .big }
        return deviceThemeSizes[AppConfig.normalizedDeviceID(deviceID)] ?? .big
    }

    private func overlayKey(for deviceID: String?) -> String {
        guard let deviceID else { return "__default__" }
        return AppConfig.normalizedDeviceID(deviceID)
    }

    private func overlay(for deviceID: String?) -> OverlayController {
        let key = overlayKey(for: deviceID)
        if let overlay = overlays[key] {
            return overlay
        }
        let overlay = OverlayController()
        overlays[key] = overlay
        return overlay
    }

    private func markOverlayVisible(for deviceID: String?) {
        visibleOverlayKeys.insert(overlayKey(for: deviceID))
        updateOverlayStackIndices()
    }

    private func markOverlayHidden(for deviceID: String?) {
        visibleOverlayKeys.remove(overlayKey(for: deviceID))
        updateOverlayStackIndices()
    }

    private func updateOverlayStackIndices() {
        let groupedKeys = Dictionary(grouping: visibleOverlayKeys) { key in
            overlayPosition(for: deviceID(forOverlayKey: key))
        }
        for keys in groupedKeys.values {
            for (index, key) in keys.sorted().enumerated() {
                overlays[key]?.setStackIndex(index)
            }
        }
    }

    private func deviceID(forOverlayKey key: String) -> String? {
        key == "__default__" ? nil : key
    }

    private func outputProfile(for deviceID: String) -> OutputProfile {
        guard let deviceProfile = deviceOutputProfiles[AppConfig.normalizedDeviceID(deviceID)] else {
            return defaultOutputProfile
        }
        return OutputProfile(
            target: defaultOutputProfile.target,
            transform: deviceProfile.transform,
            translationTarget: deviceProfile.translationTarget
        )
    }

    private func applyOverlayStyle(for deviceID: String?, overlay: OverlayController) {
        overlay.setThemeColor(themeColor(for: deviceID))
        overlay.setThemeSize(themeSize(for: deviceID))
        overlay.setPosition(overlayPosition(for: deviceID))
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

    @objc private func openInteractionSettings(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onOpenInteractionSettings?(deviceID)
    }

    @objc private func openEncoderSettings(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onOpenEncoderSettings?(deviceID)
    }

    @objc private func openRemoteSettings(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onOpenRemoteSettings?(deviceID)
    }

    @objc private func openButtonMapping(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onOpenButtonMapping?(deviceID)
    }

    @objc private func openBatteryMonitor(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onOpenBatteryMonitor?(deviceID)
    }

    @objc private func updateFirmwareFromFile(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onUpdateFirmwareFromFile?(deviceID)
    }

    @objc private func selectDeviceThemeColor(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 1).map(String.init)
        guard parts.count == 2,
              let color = OverlayThemeColor(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        if color == .auto {
            deviceThemeColors.removeValue(forKey: deviceID)
        } else {
            deviceThemeColors[deviceID] = color
        }
        rebuildMenu()
        onSetDeviceThemeColor?(deviceID, color)
    }

    @objc private func selectDeviceOverlayPosition(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 1).map(String.init)
        guard parts.count == 2,
              let position = OverlayPosition(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        if position == .bottomCenter {
            deviceOverlayPositions.removeValue(forKey: deviceID)
        } else {
            deviceOverlayPositions[deviceID] = position
        }
        rebuildMenu()
        onSetDeviceOverlayPosition?(deviceID, position)
    }

    @objc private func selectDeviceThemeSize(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 1).map(String.init)
        guard parts.count == 2,
              let size = OverlayThemeSize(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        if size == .big {
            deviceThemeSizes.removeValue(forKey: deviceID)
        } else {
            deviceThemeSizes[deviceID] = size
        }
        rebuildMenu()
        onSetDeviceThemeSize?(deviceID, size)
    }

    @objc private func selectDeviceTextMode(_ sender: NSMenuItem) {
        guard let selection = sender.representedObject as? String else { return }
        let parts = selection.split(separator: ":", maxSplits: 2, omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 2,
              let transform = TextTransform(rawValue: parts[1]) else { return }
        let deviceID = AppConfig.normalizedDeviceID(parts[0])
        var profile = outputProfile(for: deviceID)
        profile.transform = transform
        if transform == .translate, parts.count == 3, !parts[2].isEmpty {
            profile.translationTarget = parts[2]
        }
        setDeviceOutputProfile(deviceID: deviceID, profile: profile)
    }

    private func setDeviceOutputProfile(deviceID: String, profile: OutputProfile) {
        let storedProfile = OutputProfile(
            target: defaultOutputProfile.target,
            transform: profile.transform,
            translationTarget: profile.translationTarget
        )
        if storedProfile.transform == defaultOutputProfile.transform &&
            storedProfile.translationTarget == defaultOutputProfile.translationTarget {
            deviceOutputProfiles.removeValue(forKey: deviceID)
        } else {
            deviceOutputProfiles[deviceID] = storedProfile
        }
        rebuildMenu()
        onSetDeviceOutputProfile?(deviceID, storedProfile)
    }

    @objc private func restoreLastInput() {
        _ = onRestoreLastInput?()
    }

    @objc private func selectInteractionMode(_ sender: NSMenuItem) {
        guard
            let rawValue = sender.representedObject as? String,
            let mode = InteractionMode(rawValue: rawValue)
        else { return }
        interactionMode = mode
        rebuildMenu()
        onSetInteractionMode?(mode)
    }

    @objc private func toggleAutoEnter() {
        autoEnter.toggle()
        rebuildMenu()
        onSetAutoEnter?(autoEnter)
    }

    @objc private func toggleGlobalHotkey() {
        globalHotkeyEnabled.toggle()
        rebuildMenu()
        onSetGlobalHotkeyEnabled?(globalHotkeyEnabled)
    }

    @objc private func selectGlobalHotkeyPreset(_ sender: NSMenuItem) {
        guard let preset = sender.representedObject as? String else { return }
        globalHotkey = preset
        // 对齐 Windows：选定预设即视为启用。
        globalHotkeyEnabled = true
        rebuildMenu()
        onSetGlobalHotkey?(preset)
    }

    @objc private func customizeGlobalHotkey() {
        onCustomizeHotkey?()
    }

    @objc private func toggleLaunchAtLogin() {
        launchAtLogin.toggle()
        rebuildMenu()
        onSetLaunchAtLogin?(launchAtLogin)
    }

    /// 自定义热键捕获完成后由 AppDelegate 回写。
    func applyCustomHotkey(_ spec: String) {
        globalHotkey = spec
        globalHotkeyEnabled = true
        rebuildMenu()
        onSetGlobalHotkey?(spec)
    }

    @objc private func selectOutputTarget(_ sender: NSMenuItem) {
        guard
            let rawValue = sender.representedObject as? String,
            let target = OutputTarget(rawValue: rawValue)
        else { return }
        defaultOutputProfile.target = target
        rebuildMenu()
        onSetDefaultOutputProfile?(defaultOutputProfile)
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
