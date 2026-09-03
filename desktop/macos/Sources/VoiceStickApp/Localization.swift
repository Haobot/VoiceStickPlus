import Foundation

/// UI 界面语言（对齐 Windows UiLanguage；rawValue 即 config.toml `ui_language` 序列化值，
/// 跨端配置兼容，勿改字符串）。
enum UiLanguage: String, CaseIterable {
    case system
    case en = "en"
    case zhHans = "zh-Hans"

    /// 设置窗语言下拉显示名（按当前界面语言；对齐 Windows kSettingsLanguage* 三选项）。
    var displayName: String {
        switch self {
        case .system: return tr(.languageSystem)
        case .en: return tr(.languageEnglish)
        case .zhHans: return tr(.languageChineseSimplified)
        }
    }

    /// 生效语言（对齐 Windows EffectiveUiLanguage）：system 时读系统首选语言，
    /// zh 前缀（zh/zh-*/zh_*）→ 简体中文，否则 → 英文。
    var effective: UiLanguage {
        guard self == .system else { return self }
        return Self.fromLocaleName(Locale.preferredLanguages.first)
    }

    /// 对齐 Windows UiLanguageFromLocaleName。
    static func fromLocaleName(_ name: String?) -> UiLanguage {
        guard let name else { return .en }
        let locale = name.lowercased()
        if locale == "zh" || locale.hasPrefix("zh-") || locale.hasPrefix("zh_") {
            return .zhHans
        }
        return .en
    }

    /// 对齐 Windows UiLanguageFromName：zh_CN/zh-CN/zh 兼容，非法值回 system。
    init(configValue: String) {
        switch configValue {
        case "en":
            self = .en
        case "zh-Hans", "zh_CN", "zh-CN", "zh":
            self = .zhHans
        default:
            self = .system
        }
    }
}

/// 稳定语义 key（对齐 Windows StringId；不要用英文原文做 key，以免文案微调破坏索引）。
/// 只覆盖 macOS 现有 UI 点位；英文文案与 macOS 原有硬编码一致，中文文案逐条对齐
/// Windows localization.cc 中文表（含 win32_app.cc 的 Localized*Name 辅助函数）。
enum L10nKey: String, CaseIterable {
    // 通用
    case ok, cancel, save, close, restoreDefaults

    // 语言下拉
    case languageSystem, languageEnglish, languageChineseSimplified

    // 状态栏按钮
    case statusPairVoiceStick, statusListening, statusProcessing, statusReady, statusError
    case statusPair

    // 托盘主菜单
    case menuRestoreLastInput, menuPairDevice, menuSettings, menuWebsite
    case menuCheckAppUpdates, menuQuit, menuPressReturnAfterPaste, menuLaunchAtLogin
    case menuInteraction, menuHoldToTalk, menuClickToTalk
    case menuHotkey, menuEnableHotkey, menuCustomHotkey, menuCustomHotkeyNamed
    case menuOutput, outputFocusedApp, outputSubtitle

    // 设备子菜单
    case stateScanning, stateConnected
    case menuThemeColor, menuThemeSize, menuOverlayPosition
    case menuTranslation, textOriginal, menuTranslateTo
    case menuInteractionSettings, menuEncoderSettings, menuRemoteSettings, menuBatteryMonitor
    case menuUpdateFirmwareFromFile, menuForgetDevice
    case firmwareCheckingUpdates, firmwareUpdateCheckFailed, firmwareUpdateTo, menuFirmwareUpToDate

    // 主题/灵敏度/编码器枚举显示名
    case themeAuto, themeWhite, themeBlack, themePink, themeGreen, themeYellow, themeBlue, themePurple
    case sizeBig, sizeMedium, sizeSmall
    case positionCenter, positionBottomCenter, positionTopLeft, positionTopRight
    case positionBottomLeft, positionBottomRight
    case wakeLow, wakeMedium, wakeHigh
    case ledRed, ledGreen, ledBlue, ledYellow, ledPurple, ledCyan, ledWhite, ledOff
    case actionRecording, actionCustomKey

    // 设置窗口
    case settingsTitle, settingsLanguage
    case settingsSectionGeneral, settingsDeveloperMode
    case settingsSectionAsr, settingsProvider, settingsApiKey, settingsApplyTrial
    case settingsResourceId, settingsHotwords, settingsHotwordsHint
    case settingsSectionRefine, settingsLlmBaseUrl, settingsLlmModel
    case settingsRefineText, settingsRefinePrompt
    case settingsSectionOutput, settingsOutputTarget
    case settingsSectionSystem, settingsLaunchAtLogin, settingsShowImuDebug
    case settingsSectionAudioFiles, settingsDebugAudio, settingsDebugDir, settingsChooseDir
    case settingsOpenConfigFolder
    case settingsApplyingTrial, settingsTrialApplied, settingsTrialPageOpened
    case settingsTrialOpenFailedTitle, settingsTrialFailedTitle
    case settingsSaved, settingsSaveFailedTitle

    // 引导窗口
    case onboardingSetupTitle
    case onboardingStepPairDevice, onboardingStepAsrKey, onboardingStepAccessibility, onboardingStepReady
    case onboardingBack, onboardingContinue, onboardingFinish
    case onboardingPairTitle, onboardingPairDetail
    case onboardingProviderTitle, onboardingProviderDetail
    case onboardingAccessibilityTitle, onboardingAccessibilityDetail
    case onboardingReadyTitle, onboardingReadyDetail
    case onboardingOpenAccessibilitySettings
    case columnDevice, columnId, columnRssi
    case bluetoothUnavailable, foundCount, selectDevice
    case onboardingSelectedDevice, valueNotPaired, valueAllowed, valueNotAllowedYet
    case applyTrialFailed, saveFailedWithError
    case openedSystemSettings, openAccessibilityManually
    case accessibilityAllowed, accessibilityNotAllowed
    case accessibilityPasteBlocked, accessibilityAlertTitle, accessibilityAlertBody
    case selectDeviceFirst, enterApiKeyFor, enterValidCloudURL, allowAccessibilityFirst

    // 配对窗口
    case pairTitle, pairColumnType, pairButton, pairNamePaired
    case deviceTypeVoiceStick, deviceTypeXiaomiRemote
    case pairInProgress, pairAlreadyPaired, pairIdConflict, pairDeviceLost, pairPairingDevice
    case pairTimeoutManual, pairConnectFailedManual, pairDisconnectedManual
    case pairAtvvServiceMissingManual, pairAtvvCharacteristicsMissingManual, pairSubscribeFailedManual

    // 固件更新窗口与提示
    case firmwareWindowTitle, firmwareUpdatingTitle, firmwarePreparing
    case firmwareSpeed, firmwareEstimatingTime, firmwareTimeRemaining, firmwareFinishingOnDevice
    case firmwareUpdatedTitle, firmwareUpdatedDetail, firmwareDone
    case firmwareUpdateFailedTitle, firmwareKeptCurrent
    case firmwareCancellingTitle, firmwareCancellingDetail, firmwareCancelling
    case firmwareLocalFile
    case firmwarePromptTitleRequired, firmwarePromptTitleAvailable, firmwarePromptBody
    case firmwarePromptUpdateButton, firmwarePromptLaterButton

    // 热键捕获窗口
    case hotkeyCaptureTitle, hotkeyCaptureHint, hotkeyCaptureMissingModifier, hotkeyCaptureApplyHint

    // 设备交互设置窗口
    case interactionSettingsTitle
    case settingsWakeSensitivity, settingsTapToArrow, settingsTapSensitivity
    case settingsAirMouseX, settingsAirMouseY

    // 编码器设置窗口
    case encoderSettingsTitle, settingsSectionEncoder
    case encoderToArrow, encoderRotationInvert
    case encoderRotateCwKey, encoderRotateCcwKey
    case encoderRotateFastThreshold, encoderRotateCwFastKey, encoderRotateCcwFastKey
    case encoderDecideWindow, encoderLedColor
    case encoderPressAction, encoderPressKey, encoderDoubleClickAction, encoderDoubleClickKey
    case encoderInvalidKey

    // 遥控器设置窗口
    case remoteSettingsTitle, remoteSettingsSection
    case remoteGainDb, remoteDoubleClickMs, remoteEffectiveNextConnect

    // 电池电压监测窗口
    case batteryMonitorTitle, batteryUsbAutoOff, batteryWarnUsb
    case batteryStart, batteryStop, batteryExportCsv, batteryExportPng
    case batteryStatusIdle, batteryStatusAnchoring, batteryStatusProbing
    case batteryStatusMonitoring, batteryStatusFinished, batteryStatusError
    case batteryErrProbeTimeout, batteryErrDumpTimeout, batteryErrRestart
    case batteryErrDisconnected, batteryErrLogCleared
    case batterySavedTo, batterySaveFailed, batteryNotEnoughSamples
    case batteryAxisTime, batteryAxisVoltage, batteryChartTitle
}

/// 界面文案查表。当前语言由 AppDelegate 在启动与配置变更时写入 `Localization.current`
///（存配置值；查表时取 `effective`，调用时读取、不缓存）。
enum Localization {
    static var current: UiLanguage = .system

    static func tr(_ key: L10nKey) -> String {
        let table = current.effective == .zhHans ? chinese : english
        return table[key] ?? english[key] ?? key.rawValue
    }

    static func tr(_ key: L10nKey, _ args: CVarArg...) -> String {
        String(format: tr(key), arguments: args)
    }

    /// 电量后缀（对齐 Windows BatteryStatusText）："NN%" + 「充电中」（优先）/「外接电源」。
    static func batteryStatusText(level: Int, charging: Bool, usbPowered: Bool) -> String {
        var text = "\(level)%"
        if charging {
            text += current.effective == .zhHans ? "，充电中" : ", charging"
        } else if usbPowered {
            text += current.effective == .zhHans ? "，外接电源" : ", plugged in"
        }
        return text
    }

    /// 翻译目标语言显示名（对齐 Windows LocalizedTranslationTargetName：
    /// 中文界面用中文语言名，其余语言界面保持英文名）。
    static func translationTargetName(code: String, englishName: String) -> String {
        guard current.effective == .zhHans else { return englishName }
        return chineseLanguageNames[code] ?? englishName
    }

    /// 自检：英文表和中文表对所有 key 都有条目（对齐 Windows LocalizationTablesAreComplete）。
    static func tablesAreComplete() -> Bool {
        L10nKey.allCases.allSatisfy { english[$0] != nil && chinese[$0] != nil }
    }

    private static let chineseLanguageNames: [String: String] = [
        "en": "英文",
        "zh-Hans": "简体中文",
        "zh-Hant": "繁体中文",
        "ja": "日文",
        "ko": "韩文",
        "ru": "俄文",
        "fr": "法文",
        "de": "德文",
        "es": "西班牙文",
        "it": "意大利文",
        "pt": "葡萄牙文",
        "nl": "荷兰文",
        "sv": "瑞典文",
        "pl": "波兰文",
        "tr": "土耳其文",
        "ar": "阿拉伯文",
        "hi": "印地文",
        "id": "印尼文",
        "vi": "越南文",
        "th": "泰文",
    ]

    private static let english: [L10nKey: String] = [
        .ok: "OK",
        .cancel: "Cancel",
        .save: "Save",
        .close: "Close",
        .restoreDefaults: "Restore defaults",

        .languageSystem: "Follow System",
        .languageEnglish: "English",
        .languageChineseSimplified: "Simplified Chinese",

        .statusPairVoiceStick: "Pair VoiceStick",
        .statusListening: "Listening",
        .statusProcessing: "Processing",
        .statusReady: "Ready",
        .statusError: "Error",
        .statusPair: "Pair",

        .menuRestoreLastInput: "Restore Last Input",
        .menuPairDevice: "Pair Device...",
        .menuSettings: "Settings...",
        .menuWebsite: "Website",
        .menuCheckAppUpdates: "Check for App Updates...",
        .menuQuit: "Quit",
        .menuPressReturnAfterPaste: "Press Return After Paste",
        .menuLaunchAtLogin: "Launch at Login",
        .menuInteraction: "Interaction",
        .menuHoldToTalk: "Hold to Talk",
        .menuClickToTalk: "Click to Talk",
        .menuHotkey: "Hotkey",
        .menuEnableHotkey: "Enable Hotkey",
        .menuCustomHotkey: "Custom Hotkey...",
        .menuCustomHotkeyNamed: "Custom Hotkey (%@)...",
        .menuOutput: "Output",
        .outputFocusedApp: "Focused App",
        .outputSubtitle: "Subtitle",

        .stateScanning: "Scanning",
        .stateConnected: "Connected",
        .menuThemeColor: "Theme Color",
        .menuThemeSize: "Theme Size",
        .menuOverlayPosition: "Overlay Position",
        .menuTranslation: "Translation",
        .textOriginal: "Original",
        .menuTranslateTo: "Translate to %@",
        .menuInteractionSettings: "Device interaction settings...",
        .menuEncoderSettings: "Encoder settings...",
        .menuRemoteSettings: "Remote settings...",
        .menuBatteryMonitor: "Battery voltage monitor...",
        .menuUpdateFirmwareFromFile: "Update Firmware from File...",
        .menuForgetDevice: "Forget This Device",
        .firmwareCheckingUpdates: "Checking for Updates",
        .firmwareUpdateCheckFailed: "Update Check Failed",
        .firmwareUpdateTo: "Update to %@...",
        .menuFirmwareUpToDate: "Firmware Up to Date",

        .themeAuto: "Auto",
        .themeWhite: "White",
        .themeBlack: "Black",
        .themePink: "Pink",
        .themeGreen: "Green",
        .themeYellow: "Yellow",
        .themeBlue: "Blue",
        .themePurple: "Purple",
        .sizeBig: "Big",
        .sizeMedium: "Medium",
        .sizeSmall: "Small",
        .positionCenter: "Center",
        .positionBottomCenter: "Bottom Center",
        .positionTopLeft: "Top Left",
        .positionTopRight: "Top Right",
        .positionBottomLeft: "Bottom Left",
        .positionBottomRight: "Bottom Right",
        .wakeLow: "Low",
        .wakeMedium: "Medium",
        .wakeHigh: "High",
        .ledRed: "Red",
        .ledGreen: "Green",
        .ledBlue: "Blue",
        .ledYellow: "Yellow",
        .ledPurple: "Purple",
        .ledCyan: "Cyan",
        .ledWhite: "White",
        .ledOff: "Off",
        .actionRecording: "Recording",
        .actionCustomKey: "Custom key",

        .settingsTitle: "VoiceStick Settings",
        .settingsLanguage: "Language",
        .settingsSectionGeneral: "General",
        .settingsDeveloperMode: "Developer Mode",
        .settingsSectionAsr: "Speech Recognition",
        .settingsProvider: "Provider",
        .settingsApiKey: "API Key",
        .settingsApplyTrial: "Apply Trial",
        .settingsResourceId: "Resource ID",
        .settingsHotwords: "Hotwords",
        .settingsHotwordsHint: "Separate hotwords with commas or new lines.",
        .settingsSectionRefine: "Text Refinement",
        .settingsLlmBaseUrl: "Base URL",
        .settingsLlmModel: "Model",
        .settingsRefineText: "Refine Recognized Text",
        .settingsRefinePrompt: "Prompt",
        .settingsSectionOutput: "Output",
        .settingsOutputTarget: "Target",
        .settingsSectionSystem: "System",
        .settingsLaunchAtLogin: "Launch at Login",
        .settingsShowImuDebug: "Show IMU Debug on Device",
        .settingsSectionAudioFiles: "Audio Files",
        .settingsDebugAudio: "Save debug audio files",
        .settingsDebugDir: "Audio Folder",
        .settingsChooseDir: "Choose...",
        .settingsOpenConfigFolder: "Open Config Folder",
        .settingsApplyingTrial: "Applying trial API key...",
        .settingsTrialApplied: "Trial API key applied.",
        .settingsTrialPageOpened: "Opened trial application page.",
        .settingsTrialOpenFailedTitle: "Could Not Open Trial Page",
        .settingsTrialFailedTitle: "Could Not Apply Trial API Key",
        .settingsSaved: "Saved.",
        .settingsSaveFailedTitle: "Could Not Save Settings",

        .onboardingSetupTitle: "Set Up VoiceStick",
        .onboardingStepPairDevice: "Pair Device",
        .onboardingStepAsrKey: "ASR Key",
        .onboardingStepAccessibility: "Accessibility",
        .onboardingStepReady: "Ready",
        .onboardingBack: "Back",
        .onboardingContinue: "Continue",
        .onboardingFinish: "Finish",
        .onboardingPairTitle: "Pair your VoiceStick",
        .onboardingPairDetail: "Choose a nearby VS-XXXX device. VoiceStick needs a paired device before the app can listen.",
        .onboardingProviderTitle: "Choose your speech provider",
        .onboardingProviderDetail: "Pick the ASR provider and enter the key or endpoint settings it needs.",
        .onboardingAccessibilityTitle: "Allow text insertion",
        .onboardingAccessibilityDetail: "VoiceStick pastes recognized text at your cursor, so macOS Accessibility permission is required.",
        .onboardingReadyTitle: "VoiceStick is ready",
        .onboardingReadyDetail: "The device and ASR settings are configured. Finish setup to start scanning and connecting.",
        .onboardingOpenAccessibilitySettings: "Open Accessibility Settings",
        .columnDevice: "Device",
        .columnId: "ID",
        .columnRssi: "RSSI",
        .bluetoothUnavailable: "Bluetooth unavailable",
        .foundCount: "%d found",
        .selectDevice: "Select a device",
        .onboardingSelectedDevice: "Selected VS-%@",
        .valueNotPaired: "Not paired",
        .valueAllowed: "Allowed",
        .valueNotAllowedYet: "Not allowed yet",
        .applyTrialFailed: "Apply failed: %@",
        .saveFailedWithError: "Save failed: %@",
        .openedSystemSettings: "Opened System Settings.",
        .openAccessibilityManually: "Open System Settings, then go to Privacy & Security > Accessibility.",
        .accessibilityAllowed: "Accessibility permission is allowed.",
        .accessibilityNotAllowed: "Accessibility permission is not allowed yet.",
        .accessibilityPasteBlocked: "Accessibility permission missing — text not pasted",
        .accessibilityAlertTitle: "Accessibility Permission Required",
        .accessibilityAlertBody: "VoiceStick inserts recognized text at the cursor by simulating Cmd+V, which macOS blocks without Accessibility permission. Grant it in System Settings > Privacy & Security > Accessibility, then quit and relaunch VoiceStick — permission changes only take effect after relaunch.",
        .selectDeviceFirst: "Select a VoiceStick device first.",
        .enterApiKeyFor: "Enter the API key for %@.",
        .enterValidCloudURL: "Enter a valid Cloud URL.",
        .allowAccessibilityFirst: "Allow Accessibility permission before continuing.",

        .pairTitle: "Pair VoiceStick",
        .pairColumnType: "Type",
        .pairButton: "Pair",
        .pairNamePaired: "%@ (paired)",
        .deviceTypeVoiceStick: "Voice Stick",
        .deviceTypeXiaomiRemote: "Xiaomi Remote",
        .pairInProgress: "Pairing in progress",
        .pairAlreadyPaired: "Already paired",
        .pairIdConflict: "ID %@ conflicts with another paired device",
        .pairDeviceLost: "Device lost; keep scanning",
        .pairPairingDevice: "Pairing %@...",
        .pairTimeoutManual: "Pairing timed out. Pair manually in System Settings > Bluetooth.",
        .pairConnectFailedManual: "Connect failed. Pair manually in System Settings > Bluetooth.",
        .pairDisconnectedManual: "Disconnected during pairing. Pair manually in System Settings > Bluetooth.",
        .pairAtvvServiceMissingManual: "ATVV service not found. Pair manually in System Settings > Bluetooth.",
        .pairAtvvCharacteristicsMissingManual: "ATVV characteristics missing. Pair manually in System Settings > Bluetooth.",
        .pairSubscribeFailedManual: "Subscribe failed. Pair manually in System Settings > Bluetooth.",

        .firmwareWindowTitle: "Firmware Update",
        .firmwareUpdatingTitle: "Updating Firmware",
        .firmwarePreparing: "Preparing update...",
        .firmwareSpeed: "Speed %@",
        .firmwareEstimatingTime: "Estimating time remaining",
        .firmwareTimeRemaining: "%@ remaining",
        .firmwareFinishingOnDevice: "Finishing on device",
        .firmwareUpdatedTitle: "Firmware Updated",
        .firmwareUpdatedDetail: "The device is rebooting into the new firmware.",
        .firmwareDone: "Done",
        .firmwareUpdateFailedTitle: "Update Failed",
        .firmwareKeptCurrent: "The device kept its current firmware.",
        .firmwareCancellingTitle: "Cancelling Firmware Update",
        .firmwareCancellingDetail: "Stopping transfer and asking the device to abort.",
        .firmwareCancelling: "Cancelling",
        .firmwareLocalFile: "local file",
        .firmwarePromptTitleRequired: "Firmware update recommended",
        .firmwarePromptTitleAvailable: "Firmware update available",
        .firmwarePromptBody: "VS-%@ is running firmware %@. The latest firmware is %@.",
        .firmwarePromptUpdateButton: "Update Firmware",
        .firmwarePromptLaterButton: "Later",

        .hotkeyCaptureTitle: "Custom Hotkey",
        .hotkeyCaptureHint: "Press the new hotkey (modifier + key). Esc cancels.",
        .hotkeyCaptureMissingModifier: "Hotkey needs at least one modifier (Control/Option/Shift/Command).",
        .hotkeyCaptureApplyHint: "Press OK to apply, or press another combination.",

        .interactionSettingsTitle: "Device interaction - VS-%@",
        .settingsWakeSensitivity: "Wake Sensitivity",
        .settingsTapToArrow: "Double-tap device to press Down arrow",
        .settingsTapSensitivity: "Tap Sensitivity",
        .settingsAirMouseX: "Left/Right Sensitivity",
        .settingsAirMouseY: "Up/Down Sensitivity",

        .encoderSettingsTitle: "Encoder settings - VS-%@",
        .settingsSectionEncoder: "Encoder",
        .encoderToArrow: "Inject keys on rotate",
        .encoderRotationInvert: "Invert rotation direction",
        .encoderRotateCwKey: "Clockwise key",
        .encoderRotateCcwKey: "Counter-clockwise key",
        .encoderRotateFastThreshold: "Fast threshold (detents/s)",
        .encoderRotateCwFastKey: "Fast clockwise key",
        .encoderRotateCcwFastKey: "Fast counter-clockwise key",
        .encoderDecideWindow: "Decide window (ms)",
        .encoderLedColor: "Recording LED color",
        .encoderPressAction: "Press action",
        .encoderPressKey: "Press key",
        .encoderDoubleClickAction: "Double-click action",
        .encoderDoubleClickKey: "Double-click key",
        .encoderInvalidKey: "Invalid encoder key syntax (e.g. \"down\", \"ctrl+z\"); the field was not saved.",

        .remoteSettingsTitle: "Remote settings - RC-%@",
        .remoteSettingsSection: "Remote settings",
        .remoteGainDb: "Gain (dB, -24 to 24)",
        .remoteDoubleClickMs: "Double-click window (ms, 200 to 600)",
        .remoteEffectiveNextConnect: "Changes take effect on the next connection.",

        .batteryMonitorTitle: "Battery Voltage Monitor - VS-%@",
        .batteryUsbAutoOff: "Auto power-off on USB (10 min)",
        .batteryWarnUsb: "Tip: keep the device on USB power while monitoring; on battery it powers off after about 10 minutes idle.",
        .batteryStart: "Start",
        .batteryStop: "Stop",
        .batteryExportCsv: "Export CSV...",
        .batteryExportPng: "Export PNG...",
        .batteryStatusIdle: "Idle. Click Start to run a 60-minute monitoring session (one sample per minute).",
        .batteryStatusAnchoring: "Synchronizing time anchor...",
        .batteryStatusProbing: "Probing log baseline...",
        .batteryStatusMonitoring: "Monitoring: cycle %d/%d, %d points, next sample in %@",
        .batteryStatusFinished: "Finished: %d points collected.",
        .batteryStatusError: "Aborted: %@",
        .batteryErrProbeTimeout: "log probe timed out (device not responding)",
        .batteryErrDumpTimeout: "voltage log export timed out repeatedly",
        .batteryErrRestart: "device restarted (uptime went backwards)",
        .batteryErrDisconnected: "device disconnected",
        .batteryErrLogCleared: "device log was cleared (total shrank)",
        .batterySavedTo: "Saved: %@",
        .batterySaveFailed: "save failed: %@",
        .batteryNotEnoughSamples: "save failed: not enough valid samples",
        .batteryAxisTime: "Time (min)",
        .batteryAxisVoltage: "Voltage (mV)",
        .batteryChartTitle: "VS-%@ battery voltage (%d points)",
    ]

    private static let chinese: [L10nKey: String] = [
        .ok: "确定",
        .cancel: "取消",
        .save: "保存",
        .close: "关闭",
        .restoreDefaults: "恢复默认",

        .languageSystem: "跟随系统",
        .languageEnglish: "英文",
        .languageChineseSimplified: "简体中文",

        .statusPairVoiceStick: "配对 VoiceStick",
        .statusListening: "正在聆听",
        .statusProcessing: "处理中",
        .statusReady: "就绪",
        .statusError: "错误",
        .statusPair: "配对",

        .menuRestoreLastInput: "恢复上次输入",
        .menuPairDevice: "配对设备...",
        .menuSettings: "设置...",
        .menuWebsite: "官网",
        .menuCheckAppUpdates: "检查应用更新...",
        .menuQuit: "退出",
        .menuPressReturnAfterPaste: "粘贴后按回车",
        .menuLaunchAtLogin: "开机自启动",
        .menuInteraction: "交互方式",
        .menuHoldToTalk: "按住说话",
        .menuClickToTalk: "点击说话",
        .menuHotkey: "热键",
        .menuEnableHotkey: "启用热键",
        .menuCustomHotkey: "自定义热键...",
        .menuCustomHotkeyNamed: "自定义热键（%@）...",
        .menuOutput: "输出",
        .outputFocusedApp: "当前应用",
        .outputSubtitle: "字幕",

        .stateScanning: "正在扫描...",
        .stateConnected: "已连接",
        .menuThemeColor: "主题颜色",
        .menuThemeSize: "主题大小",
        .menuOverlayPosition: "悬浮窗位置",
        .menuTranslation: "翻译",
        .textOriginal: "原文",
        .menuTranslateTo: "翻译为 %@",
        .menuInteractionSettings: "设备交互设置…",
        .menuEncoderSettings: "编码器设置…",
        .menuRemoteSettings: "遥控器设置…",
        .menuBatteryMonitor: "电池电压监测…",
        .menuUpdateFirmwareFromFile: "从本地文件更新固件...",
        .menuForgetDevice: "忘记设备",
        .firmwareCheckingUpdates: "正在检查固件更新...",
        .firmwareUpdateCheckFailed: "固件检查失败",
        .firmwareUpdateTo: "更新到 %@...",
        .menuFirmwareUpToDate: "固件已是最新",

        .themeAuto: "自动",
        .themeWhite: "白色",
        .themeBlack: "黑色",
        .themePink: "粉色",
        .themeGreen: "绿色",
        .themeYellow: "黄色",
        .themeBlue: "蓝色",
        .themePurple: "紫色",
        .sizeBig: "大",
        .sizeMedium: "中",
        .sizeSmall: "小",
        .positionCenter: "居中",
        .positionBottomCenter: "底部居中",
        .positionTopLeft: "左上",
        .positionTopRight: "右上",
        .positionBottomLeft: "左下",
        .positionBottomRight: "右下",
        .wakeLow: "低",
        .wakeMedium: "中",
        .wakeHigh: "高",
        .ledRed: "红",
        .ledGreen: "绿",
        .ledBlue: "蓝",
        .ledYellow: "黄",
        .ledPurple: "紫",
        .ledCyan: "青",
        .ledWhite: "白",
        .ledOff: "关",
        .actionRecording: "录音",
        .actionCustomKey: "自定义按键",

        .settingsTitle: "VoiceStick 设置",
        .settingsLanguage: "界面语言",
        .settingsSectionGeneral: "通用",
        .settingsDeveloperMode: "开发者模式（显示全部高级设置）",
        .settingsSectionAsr: "语音识别",
        .settingsProvider: "服务提供方",
        .settingsApiKey: "API Key",
        .settingsApplyTrial: "申请试用",
        .settingsResourceId: "资源 ID",
        .settingsHotwords: "热词",
        .settingsHotwordsHint: "使用逗号或换行分隔热词。",
        .settingsSectionRefine: "文本精修",
        .settingsLlmBaseUrl: "Base URL",
        .settingsLlmModel: "模型",
        .settingsRefineText: "精修文本（去除停顿空格、修正标点、清理口头语）",
        .settingsRefinePrompt: "精修提示词",
        .settingsSectionOutput: "输出",
        .settingsOutputTarget: "输出目标",
        .settingsSectionSystem: "系统",
        .settingsLaunchAtLogin: "登录时自动运行 VoiceStick",
        .settingsShowImuDebug: "显示加速度调试数值",
        .settingsSectionAudioFiles: "音频文件",
        .settingsDebugAudio: "保存调试音频文件",
        .settingsDebugDir: "音频文件夹",
        .settingsChooseDir: "选择...",
        .settingsOpenConfigFolder: "打开配置文件夹",
        .settingsApplyingTrial: "正在申请试用 API Key...",
        .settingsTrialApplied: "试用 API Key 已应用。",
        .settingsTrialPageOpened: "已打开试用申请页面。",
        .settingsTrialOpenFailedTitle: "无法打开试用申请页面",
        .settingsTrialFailedTitle: "无法申请试用 API Key",
        .settingsSaved: "设置已保存。",
        .settingsSaveFailedTitle: "无法保存设置。",

        .onboardingSetupTitle: "设置 VoiceStick",
        .onboardingStepPairDevice: "配对设备",
        .onboardingStepAsrKey: "语音识别",
        .onboardingStepAccessibility: "辅助功能",
        .onboardingStepReady: "就绪",
        .onboardingBack: "上一步",
        .onboardingContinue: "下一步",
        .onboardingFinish: "完成",
        .onboardingPairTitle: "配对你的 VoiceStick 设备。",
        .onboardingPairDetail: "选择附近的 VS-XXXX 设备。VoiceStick 需要配对设备后才能开始语音输入。",
        .onboardingProviderTitle: "选择语音识别服务。",
        .onboardingProviderDetail: "选择语音识别服务方，并填写所需的 Key 或接入点设置。",
        .onboardingAccessibilityTitle: "允许文本插入",
        .onboardingAccessibilityDetail: "VoiceStick 会在光标处粘贴识别文本，因此需要 macOS 辅助功能权限。",
        .onboardingReadyTitle: "VoiceStick 已就绪。",
        .onboardingReadyDetail: "设备与语音识别设置已完成。点击完成开始扫描并连接设备。",
        .onboardingOpenAccessibilitySettings: "打开辅助功能设置",
        .columnDevice: "设备",
        .columnId: "ID",
        .columnRssi: "信号",
        .bluetoothUnavailable: "蓝牙不可用",
        .foundCount: "找到 %d 个设备",
        .selectDevice: "请选择设备",
        .onboardingSelectedDevice: "已选择 VS-%@",
        .valueNotPaired: "未配对",
        .valueAllowed: "已允许",
        .valueNotAllowedYet: "尚未允许",
        .applyTrialFailed: "申请试用失败：%@",
        .saveFailedWithError: "保存失败：%@",
        .openedSystemSettings: "已打开系统设置。",
        .openAccessibilityManually: "请打开系统设置，进入「隐私与安全性 > 辅助功能」。",
        .accessibilityAllowed: "已获得辅助功能权限。",
        .accessibilityNotAllowed: "尚未获得辅助功能权限。",
        .accessibilityPasteBlocked: "缺少辅助功能权限，文本未粘贴",
        .accessibilityAlertTitle: "需要辅助功能权限",
        .accessibilityAlertBody: "VoiceStick 通过模拟 Cmd+V 在光标处插入识别文本，macOS 需要辅助功能权限才会放行。请在「系统设置 > 隐私与安全性 > 辅助功能」中授权，然后退出并重新打开 VoiceStick——权限变更要在重启应用后才生效。",
        .selectDeviceFirst: "请先配对 VoiceStick 设备。",
        .enterApiKeyFor: "请输入 %@ 的 API Key。",
        .enterValidCloudURL: "请输入有效的 Cloud URL。",
        .allowAccessibilityFirst: "请先允许辅助功能权限再继续。",

        .pairTitle: "配对 VoiceStick",
        .pairColumnType: "类型",
        .pairButton: "配对",
        .pairNamePaired: "%@（已配对）",
        .deviceTypeVoiceStick: "语音棒",
        .deviceTypeXiaomiRemote: "小米遥控器",
        .pairInProgress: "正在配对...",
        .pairAlreadyPaired: "该设备已配对",
        .pairIdConflict: "ID %@ 与另一台已配对设备冲突",
        .pairDeviceLost: "设备已丢失，继续扫描",
        .pairPairingDevice: "正在配对 %@...",
        .pairTimeoutManual: "配对超时。请在「系统设置 > 蓝牙」中手动配对。",
        .pairConnectFailedManual: "连接失败。请在「系统设置 > 蓝牙」中手动配对。",
        .pairDisconnectedManual: "配对过程中连接断开。请在「系统设置 > 蓝牙」中手动配对。",
        .pairAtvvServiceMissingManual: "未找到 ATVV 服务。请在「系统设置 > 蓝牙」中手动配对。",
        .pairAtvvCharacteristicsMissingManual: "缺少 ATVV 特征。请在「系统设置 > 蓝牙」中手动配对。",
        .pairSubscribeFailedManual: "订阅失败。请在「系统设置 > 蓝牙」中手动配对。",

        .firmwareWindowTitle: "固件更新",
        .firmwareUpdatingTitle: "正在更新固件...",
        .firmwarePreparing: "正在准备更新...",
        .firmwareSpeed: "速度 %@",
        .firmwareEstimatingTime: "正在估算剩余时间",
        .firmwareTimeRemaining: "剩余 %@",
        .firmwareFinishingOnDevice: "正在设备上完成",
        .firmwareUpdatedTitle: "固件已更新",
        .firmwareUpdatedDetail: "设备正在重启到新固件。",
        .firmwareDone: "完成",
        .firmwareUpdateFailedTitle: "固件更新失败。",
        .firmwareKeptCurrent: "设备仍运行当前固件。",
        .firmwareCancellingTitle: "正在取消固件更新",
        .firmwareCancellingDetail: "正在停止传输并通知设备中止。",
        .firmwareCancelling: "正在取消",
        .firmwareLocalFile: "本地文件",
        .firmwarePromptTitleRequired: "建议更新固件",
        .firmwarePromptTitleAvailable: "有可用固件更新",
        .firmwarePromptBody: "VS-%@ 当前运行固件 %@。\n\n最新固件为 %@。",
        .firmwarePromptUpdateButton: "更新固件",
        .firmwarePromptLaterButton: "稍后",

        .hotkeyCaptureTitle: "自定义热键",
        .hotkeyCaptureHint: "请按下新的快捷键（修饰键 + 主键），Esc 取消。",
        .hotkeyCaptureMissingModifier: "错误：至少需要 1 个修饰键（Control/Option/Shift/Command）",
        .hotkeyCaptureApplyHint: "按「确定」应用，或按下其他组合。",

        .interactionSettingsTitle: "设备交互 - VS-%@",
        .settingsWakeSensitivity: "拿起灵敏度",
        .settingsTapToArrow: "双击设备按下方向键↓",
        .settingsTapSensitivity: "敲击灵敏度",
        .settingsAirMouseX: "左右灵敏度",
        .settingsAirMouseY: "上下灵敏度",

        .encoderSettingsTitle: "编码器设置 - VS-%@",
        .settingsSectionEncoder: "编码器",
        .encoderToArrow: "旋转时注入按键",
        .encoderRotationInvert: "旋转方向翻转",
        .encoderRotateCwKey: "顺时针按键",
        .encoderRotateCcwKey: "逆时针按键",
        .encoderRotateFastThreshold: "快慢阈值（格/秒）",
        .encoderRotateCwFastKey: "快速顺时针按键",
        .encoderRotateCcwFastKey: "快速逆时针按键",
        .encoderDecideWindow: "判定窗口（毫秒）",
        .encoderLedColor: "录音灯颜色",
        .encoderPressAction: "单击动作",
        .encoderPressKey: "单击按键",
        .encoderDoubleClickAction: "双击动作",
        .encoderDoubleClickKey: "双击按键",
        .encoderInvalidKey: "编码器按键语法无效（示例：down、ctrl+z），该字段未保存。",

        .remoteSettingsTitle: "遥控器设置 - RC-%@",
        .remoteSettingsSection: "遥控器设置",
        .remoteGainDb: "增益（dB，-24~24）",
        .remoteDoubleClickMs: "双击窗口（ms，200~600）",
        .remoteEffectiveNextConnect: "设置将在下次连接时生效。",

        .batteryMonitorTitle: "电池电压监测 - VS-%@",
        .batteryUsbAutoOff: "供电时10分钟自动关机",
        .batteryWarnUsb: "提示：建议监测期间保持设备 USB 供电；电池供电下设备空闲约 10 分钟会自动关机。",
        .batteryStart: "开始监测",
        .batteryStop: "停止",
        .batteryExportCsv: "导出 CSV…",
        .batteryExportPng: "导出 PNG…",
        .batteryStatusIdle: "空闲。点击「开始监测」启动 60 分钟监测（每分钟 1 个采样点）。",
        .batteryStatusAnchoring: "正在同步时间锚点…",
        .batteryStatusProbing: "正在探测日志基线…",
        .batteryStatusMonitoring: "监测中：第 %d/%d 周期，%d 个数据点，下次采集 %@",
        .batteryStatusFinished: "监测完成：共 %d 个数据点。",
        .batteryStatusError: "已中止：%@",
        .batteryErrProbeTimeout: "日志基线探测超时（设备无响应）",
        .batteryErrDumpTimeout: "电压日志导出反复超时",
        .batteryErrRestart: "设备已重启（uptime 回退）",
        .batteryErrDisconnected: "设备连接已断开",
        .batteryErrLogCleared: "设备端日志被清空（总长度回退）",
        .batterySavedTo: "已保存：%@",
        .batterySaveFailed: "保存失败：%@",
        .batteryNotEnoughSamples: "保存失败：有效采样点不足",
        .batteryAxisTime: "时间（分钟）",
        .batteryAxisVoltage: "电压（mV）",
        .batteryChartTitle: "VS-%@ 电池电压监测（%d 点）",
    ]
}

/// 查表便捷入口（当前语言见 Localization.current）。
func tr(_ key: L10nKey) -> String {
    Localization.tr(key)
}

func tr(_ key: L10nKey, _ args: CVarArg...) -> String {
    String(format: Localization.tr(key), arguments: args)
}
