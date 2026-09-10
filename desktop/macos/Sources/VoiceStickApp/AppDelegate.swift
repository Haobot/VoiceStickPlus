import AppKit
import ServiceManagement
import Sparkle
import VoiceStickCore

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusController: StatusController?
    private var coordinator: VoiceStickCoordinator?
    private var settingsWindowController: SettingsWindowController?
    private var pairDeviceWindowController: PairDeviceWindowController?
    private var onboardingWindowController: OnboardingWindowController?
    private var firmwareUpdateWindowController: FirmwareUpdateWindowController?
    private var hotkeyCaptureWindowController: HotkeyCaptureWindowController?
    private var interactionSettingsWindowController: InteractionSettingsWindowController?
    private var encoderSettingsWindowController: EncoderSettingsWindowController?
    private var remoteSettingsWindowController: RemoteSettingsWindowController?
    private var buttonMappingWindowController: ButtonMappingWindowController?
    /// 电量监测窗口（单实例：同设备重入置前，换设备销毁重建，对齐 Windows）。
    private var batteryMonitorWindowController: BatteryMonitorWindowController?
    /// usb_auto_off 状态缓存（固件连接时推送 / set 回推；开窗时立即同步一次）。
    private var usbAutoOffByDeviceID: [String: Bool] = [:]
    private var updaterController: SPUStandardUpdaterController?
    private var dockIconWindowIDs = Set<ObjectIdentifier>()
    private var config = AppConfig.defaults
    private let f5Suppressor = XiaomiF5Suppressor()
    private let buttonInterceptManager = XiaomiButtonInterceptManager()
    private let globalHotkeyManager = GlobalHotkeyManager()
    /// 前台应用追踪（三期）：按键映射按前台应用切换，app 切换 → 重新 resolve + syncButtonsIntercept。
    private let frontmostAppProvider = FrontmostAppProvider()
    private var hasConnectedXiaomiDevice = false
    /// 当前已连接的小米遥控器 deviceID（HID 拦截层据其映射配置启停）。
    private var connectedXiaomiDeviceIDs: [String] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        assert(Localization.tablesAreComplete())
        configureMainMenu()
        configureApplicationIcon()
        if AppConfig.configExists {
            startApp(config: AppConfig.load())
        } else {
            showOnboarding()
        }
    }

    private func configureMainMenu() {
        let mainMenu = NSMenu()
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)

        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit VoiceStick", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)

        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(NSMenuItem.separator())
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = editMenu

        NSApp.mainMenu = mainMenu
    }

    private func startApp(config: AppConfig) {
        self.config = config
        Localization.current = config.uiLanguage
        let statusController = StatusController(
            pairedDeviceIDs: config.pairedDeviceIDs,
            deviceThemeColors: config.deviceThemeColors,
            deviceOverlayPositions: config.deviceOverlayPositions,
            deviceThemeSizes: config.deviceThemeSizes,
            interactionMode: config.interactionMode,
            autoEnter: config.autoEnter,
            defaultOutputProfile: config.defaultOutputProfile,
            deviceOutputProfiles: config.deviceOutputProfiles,
            globalHotkeyEnabled: config.globalHotkeyEnabled,
            globalHotkey: config.globalHotkey,
            launchAtLogin: config.launchAtLogin
        )
        let coordinator = VoiceStickCoordinator(config: config, statusController: statusController)

        self.statusController = statusController
        self.coordinator = coordinator

        // 三期：注入前台应用追踪（双击动作按前台应用取 app 覆盖）；
        // 前台应用切换 → 重新 resolve + 刷新 intercept 层。
        coordinator.frontmostAppProvider = frontmostAppProvider
        frontmostAppProvider.onActiveAppChange = { [weak self] _ in
            self?.syncButtonsIntercept()
        }

        // 菜单设备类别推导（未连接配对条目按 config hardware）与 F5 门控的连接集 hook。
        statusController.hardwareProvider = { [weak self] deviceID in
            self?.config.hardware(forID: deviceID)
        }
        coordinator.onConnectionChange = { [weak self] devices in
            self?.hasConnectedXiaomiDevice = devices.contains { $0.deviceClass == .xiaomiRemote2Pro }
            self?.connectedXiaomiDeviceIDs =
                devices.filter { $0.deviceClass == .xiaomiRemote2Pro }.map(\.deviceID)
            self?.syncF5Suppressor()
            self?.syncButtonsIntercept()
            // 电量监测窗口设备断连通知（监测三态中断连 → error，对齐 Windows）。
            if let monitor = self?.batteryMonitorWindowController,
               !devices.contains(where: { $0.deviceID == monitor.deviceID }) {
                monitor.notifyDeviceDisconnected()
            }
        }
        coordinator.onPowerLogFragment = { [weak self] deviceID, fragment in
            // 窗口未开或设备不匹配时丢弃（对齐 Windows）。
            guard let monitor = self?.batteryMonitorWindowController,
                  monitor.deviceID == deviceID else { return }
            monitor.handlePowerLogFragment(fragment)
        }
        coordinator.onPowerMgmtEvent = { [weak self] deviceID, event in
            self?.usbAutoOffByDeviceID[deviceID] = event.usbAutoOff
            guard let monitor = self?.batteryMonitorWindowController,
                  monitor.deviceID == deviceID else { return }
            monitor.handlePowerMgmtEvent(event)
        }

        statusController.onQuit = { NSApp.terminate(nil) }
        statusController.onOpenSettings = { [weak self] in
            let controller = self?.settingsWindowController ?? SettingsWindowController()
            self?.settingsWindowController = controller
            controller.onConfigChanged = { [weak self] config in
                self?.config = config
                // 界面语言热更：重建托盘菜单；设置窗文案在下次打开时重建
                //（丢弃缓存控制器，等价 Windows 保存后全量刷新）。
                Localization.current = config.uiLanguage
                self?.settingsWindowController = nil
                self?.statusController?.refreshLocalization()
                self?.statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
                self?.statusController?.setDeviceThemeColors(config.deviceThemeColors)
                self?.statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
                self?.statusController?.setDeviceThemeSizes(config.deviceThemeSizes)
                self?.statusController?.setInputOptions(
                    interactionMode: config.interactionMode,
                    autoEnter: config.autoEnter
                )
                self?.statusController?.setDefaultOutputProfile(config.defaultOutputProfile)
                self?.statusController?.setDeviceOutputProfiles(config.deviceOutputProfiles)
                self?.coordinator?.updateConfig(config)
                // 配置热更：小米会话参数重新装配（新值对新建会话生效）+ F5 门控刷新。
                self?.assembleXiaomiOptionsResolver()
                self?.syncF5Suppressor()
                self?.syncButtonsIntercept()
            }
            self?.showDockIconWhileWindowVisible(controller)
            controller.show()
        }
        statusController.onPairDevice = { [weak self] in
            self?.showPairDeviceWindow()
        }
        statusController.onForgetDevice = { [weak self] deviceID in
            self?.forgetDevice(deviceID)
        }
        statusController.onUpdateFirmwareDevice = { [weak self] deviceID in
            self?.updateFirmwareFromLatest(for: deviceID)
        }
        statusController.onUpdateFirmwareFromFile = { [weak self] deviceID in
            self?.updateFirmwareFromFile(for: deviceID)
        }
        statusController.onOpenInteractionSettings = { [weak self] deviceID in
            self?.showInteractionSettings(for: deviceID)
        }
        statusController.onOpenEncoderSettings = { [weak self] deviceID in
            self?.showEncoderSettings(for: deviceID)
        }
        statusController.onOpenRemoteSettings = { [weak self] deviceID in
            self?.showRemoteSettings(for: deviceID)
        }
        statusController.onOpenButtonMapping = { [weak self] deviceID in
            self?.showButtonMapping(for: deviceID)
        }
        statusController.onOpenBatteryMonitor = { [weak self] deviceID in
            self?.showBatteryMonitor(for: deviceID)
        }
        coordinator.onFirmwareUpdatePrompt = { [weak self] deviceID, currentVersion, latestVersion, isBelowMinimum in
            self?.showFirmwareUpdatePrompt(
                deviceID: deviceID,
                currentVersion: currentVersion,
                latestVersion: latestVersion,
                isBelowMinimum: isBelowMinimum
            )
        }
        coordinator.onAccessibilityPermissionMissing = { [weak self] in
            self?.showAccessibilityPermissionAlertOnce()
        }
        statusController.onRestoreLastInput = { [weak self] in
            self?.coordinator?.restoreLastInputConfirmation() ?? false
        }
        statusController.onSetInteractionMode = { [weak self] mode in
            self?.updateInputOptions(interactionMode: mode, autoEnter: nil)
        }
        statusController.onSetAutoEnter = { [weak self] autoEnter in
            self?.updateInputOptions(interactionMode: nil, autoEnter: autoEnter)
        }
        statusController.onSetDefaultOutputProfile = { [weak self] profile in
            self?.updateDefaultOutputProfile(profile)
        }
        statusController.onSetDeviceOutputProfile = { [weak self] deviceID, profile in
            self?.updateDeviceOutputProfile(deviceID: deviceID, profile: profile)
        }
        statusController.onSetDeviceThemeColor = { [weak self] deviceID, color in
            self?.updateDeviceThemeColor(deviceID: deviceID, color: color)
        }
        statusController.onSetDeviceOverlayPosition = { [weak self] deviceID, position in
            self?.updateDeviceOverlayPosition(deviceID: deviceID, position: position)
        }
        statusController.onSetDeviceThemeSize = { [weak self] deviceID, size in
            self?.updateDeviceThemeSize(deviceID: deviceID, size: size)
        }
        statusController.onSetGlobalHotkeyEnabled = { [weak self] enabled in
            self?.updateGlobalHotkey(enabled: enabled, spec: nil)
        }
        statusController.onSetGlobalHotkey = { [weak self] spec in
            self?.updateGlobalHotkey(enabled: true, spec: spec)
        }
        statusController.onCustomizeHotkey = { [weak self] in
            self?.showHotkeyCaptureWindow()
        }
        statusController.onSetLaunchAtLogin = { [weak self] enabled in
            self?.updateLaunchAtLogin(enabled)
        }
        if Self.hasSparklePublicKey {
            let updaterController = SPUStandardUpdaterController(
                startingUpdater: true,
                updaterDelegate: nil,
                userDriverDelegate: nil
            )
            self.updaterController = updaterController
            statusController.onCheckForUpdates = {
                updaterController.updater.checkForUpdates()
            }
        }
        statusController.setStatus(config.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
        assembleXiaomiOptionsResolver()
        coordinator.start()
        syncF5Suppressor()
        syncButtonsIntercept()
        syncGlobalHotkey()
        syncLaunchAtLogin()
    }

    /// 全局热键注册（对齐 Windows：开关开才注册；注册失败多为冲突，状态栏提示）。
    private func syncGlobalHotkey() {
        globalHotkeyManager.onPressed = { [weak self] in
            self?.coordinator?.handleGlobalHotkeyPressed()
        }
        globalHotkeyManager.onReleased = { [weak self] in
            self?.coordinator?.handleGlobalHotkeyReleased()
        }
        guard config.globalHotkeyEnabled else {
            globalHotkeyManager.unregister()
            return
        }
        if globalHotkeyManager.register(spec: config.globalHotkey) {
            NSLog("Global hotkey registered: \(config.globalHotkey)")
        } else {
            statusController?.setStatus(
                "Hotkey registration failed: \(config.globalHotkey) (conflict or invalid)")
        }
    }

    /// 开机自启（SMAppService，macOS 13+；裸可执行文件运行时注册会失败，仅记日志）。
    private func syncLaunchAtLogin() {
        guard #available(macOS 13.0, *) else { return }
        do {
            if config.launchAtLogin {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            NSLog("Launch at login sync failed: \(error.localizedDescription)")
        }
    }

    private func updateGlobalHotkey(enabled: Bool, spec: String?) {
        var config = self.config
        config.globalHotkeyEnabled = enabled
        if let spec, !spec.isEmpty {
            config.globalHotkey = spec
        }
        do {
            try config.save()
            self.config = config
            syncGlobalHotkey()
        } catch {
            statusController?.setStatus("Hotkey save failed")
        }
    }

    private func showHotkeyCaptureWindow() {
        let controller = HotkeyCaptureWindowController()
        controller.onCapture = { [weak self] spec in
            self?.statusController?.applyCustomHotkey(spec)
            self?.hotkeyCaptureWindowController = nil
        }
        hotkeyCaptureWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func updateLaunchAtLogin(_ enabled: Bool) {
        var config = self.config
        config.launchAtLogin = enabled
        do {
            try config.save()
            self.config = config
            syncLaunchAtLogin()
        } catch {
            statusController?.setStatus("Launch at login save failed")
        }
    }

    /// 装配小米 ATVV 会话参数解析（interactionMode/gainDb/doubleClickMs 实时读
    /// 最新 config；新值对新建会话生效）。
    private func assembleXiaomiOptionsResolver() {
        coordinator?.setXiaomiOptionsResolver { [weak self] deviceID in
            guard let self else { return XiaomiAtvvSession.Options() }
            let settings = self.config.xiaomiSettings(for: deviceID)
            return XiaomiAtvvSession.Options(
                interactionMode: self.config.interactionMode,
                gainDb: settings.gainDb,
                doubleClickWindowMs: settings.doubleClickMs
            )
        }
    }

    /// F5 suppressor 按需装载门控（对齐 Windows SyncF5Suppressor）：开关开且
    /// （任一配对条目为小米遥控器，或当前已连接设备任一为 RC 类）才挂事件 tap；
    /// 从未配对小米的用户不承受常驻 tap 开销。刷新时机：启动、配对完成、
    /// Forget、配置热更、BLE 连接集变化（coordinator.onConnectionChange）。
    private func syncF5Suppressor() {
        let hasPairedXiaomi = config.pairedDevices.contains {
            $0.hardware == PairedDeviceEntry.hardwareXiaomiRemote2Pro
        }
        if config.xiaomiSuppressF5, hasPairedXiaomi || hasConnectedXiaomiDevice,
           let coordinator {
            f5Suppressor.start(anchor: coordinator.xiaomiMicOpenAnchor)
        } else {
            f5Suppressor.stop()
        }
    }

    /// HID 拦截层按需门控（二期）：拦截层可用（输入监控 + 辅助功能授权）且任一已连接
    /// 小米遥控器的按键映射开启 intercept 时才启动；否则停止。刷新时机：启动、
    /// 配置热更、按键映射窗口保存/checkbox、BLE 连接集变化、应用重新激活（权限复授）。
    /// 权限缺失 → 仅 A 级（语音键双击）可配，拦截层记日志降级，UI 在按键映射窗提示。
    private func syncButtonsIntercept() {
        let available = XiaomiButtonInterceptManager.isAvailable()
        guard available else {
            buttonInterceptManager.stop()
            return
        }
        guard let deviceID = connectedXiaomiDeviceIDs.first else {
            buttonInterceptManager.stop()
            return
        }
        // 三期：按前台应用取有效设置（默认←device←app 合并），app 切换时经
        // frontmostAppProvider.onActiveAppChange 重新 resolve + 刷新 intercept 层。
        let activeApp = frontmostAppProvider.currentBundleIdentifier
        let settings = config.effectiveButtonsSettings(for: deviceID, activeApp: activeApp)
        if settings.intercept {
            buttonInterceptManager.start(settings: settings)
        } else {
            buttonInterceptManager.stop()
        }
    }

    /// 应用重新激活时重估权限门控：输入监控/辅助功能在用户授权（或撤销）后经此路径刷新，
    /// 下次按键映射窗打开或 checkbox 操作即可反映最新可用态。
    func applicationDidBecomeActive(_ notification: Notification) {
        syncButtonsIntercept()
    }

    private func updateInputOptions(interactionMode: InteractionMode?, autoEnter: Bool?) {
        var config = self.config
        if let interactionMode {
            config.interactionMode = interactionMode
        }
        if let autoEnter {
            config.autoEnter = autoEnter
        }
        do {
            try config.save()
            self.config = config
            statusController?.setInputOptions(
                interactionMode: config.interactionMode,
                autoEnter: config.autoEnter
            )
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Input save failed")
        }
    }

    private func updateDeviceThemeColor(deviceID: String, color: OverlayThemeColor) {
        var config = self.config
        if color == .auto {
            config.deviceThemeColors.removeValue(forKey: deviceID)
        } else {
            config.deviceThemeColors[deviceID] = color
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceThemeColors(config.deviceThemeColors)
        } catch {
            statusController?.setStatus("Theme save failed")
        }
    }

    private func updateDefaultOutputProfile(_ profile: OutputProfile) {
        var config = self.config
        config.defaultOutputProfile = profile
        do {
            try config.save()
            self.config = config
            statusController?.setDefaultOutputProfile(profile)
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Output save failed")
        }
    }

    private func updateDeviceOutputProfile(deviceID: String, profile: OutputProfile) {
        var config = self.config
        let storedProfile = OutputProfile(
            target: config.defaultOutputProfile.target,
            transform: profile.transform,
            translationTarget: profile.translationTarget
        )
        if storedProfile.transform == config.defaultOutputProfile.transform &&
            storedProfile.translationTarget == config.defaultOutputProfile.translationTarget {
            config.deviceOutputProfiles.removeValue(forKey: deviceID)
        } else {
            config.deviceOutputProfiles[deviceID] = storedProfile
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceOutputProfiles(config.deviceOutputProfiles)
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Output save failed")
        }
    }

    private func updateDeviceOverlayPosition(deviceID: String, position: OverlayPosition) {
        var config = self.config
        if position == .bottomCenter {
            config.deviceOverlayPositions.removeValue(forKey: deviceID)
        } else {
            config.deviceOverlayPositions[deviceID] = position
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
        } catch {
            statusController?.setStatus("Position save failed")
        }
    }

    private func updateDeviceThemeSize(deviceID: String, size: OverlayThemeSize) {
        var config = self.config
        if size == .big {
            config.deviceThemeSizes.removeValue(forKey: deviceID)
        } else {
            config.deviceThemeSizes[deviceID] = size
        }
        do {
            try config.save()
            self.config = config
            statusController?.setDeviceThemeSizes(config.deviceThemeSizes)
        } catch {
            statusController?.setStatus("Theme size save failed")
        }
    }

    private func showOnboarding() {
        let controller = OnboardingWindowController(config: AppConfig.defaults) { [weak self] config in
            self?.onboardingWindowController = nil
            self?.startApp(config: config)
        }
        onboardingWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func configureApplicationIcon() {
        if let image = Self.applicationIconImage() {
            NSApp.applicationIconImage = image
            let imageView = NSImageView(frame: NSRect(x: 4, y: 4, width: 120, height: 120))
            imageView.image = image
            imageView.imageScaling = .scaleProportionallyUpOrDown
            let dockView = NSView(frame: NSRect(x: 0, y: 0, width: 128, height: 128))
            dockView.addSubview(imageView)
            NSApp.dockTile.contentView = dockView
            NSApp.dockTile.display()
        }
    }

    private static var hasSparklePublicKey: Bool {
        guard let publicKey = Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") as? String else {
            return false
        }
        return !publicKey.isEmpty && !publicKey.hasPrefix("REPLACE_WITH")
    }

    private func showPairDeviceWindow() {
        var config = AppConfig.load()
        let controller = PairDeviceWindowController(
            existingDeviceIDs: config.pairedDeviceIDs,
            existingPeripheralUUIDs: config.pairedDevices.map(\.address)
        ) { [weak self] result in
            if result.deviceClass == .xiaomiRemote2Pro {
                // RC：订阅加密 Control 特征成功（系统 Bond 完成）即配对成功；落库
                // paired_device 条目（hardware=xiaomi_remote_2_pro，addr=外设 UUID，
                // 无固件概念），跳过 checkFirmwareAfterPairing。
                // 回调内重新 load：避免开窗期间设置窗口保存的改动被旧快照覆盖
                //（VS 分支用开窗时快照为既有问题，保持原样）。
                var freshConfig = AppConfig.load()
                guard freshConfig.savePairedDevice(
                    id: result.deviceID,
                    addr: result.peripheralUUID ?? "",
                    kind: "uuid",
                    name: result.name,
                    hardware: PairedDeviceEntry.hardwareXiaomiRemote2Pro,
                    firmwareVersion: ""
                ) else {
                    self?.statusController?.setStatus("Pair save failed")
                    return
                }
                self?.config = freshConfig
                self?.statusController?.setPairedDeviceIDs(freshConfig.pairedDeviceIDs)
                self?.statusController?.setDeviceThemeColors(freshConfig.deviceThemeColors)
                self?.statusController?.setDeviceOverlayPositions(freshConfig.deviceOverlayPositions)
                self?.statusController?.setDeviceThemeSizes(freshConfig.deviceThemeSizes)
                self?.coordinator?.updatePairedDeviceIDs(freshConfig.pairedDeviceIDs)
                self?.syncF5Suppressor()
                return
            }
            let deviceID = result.deviceID
            if !config.pairedDeviceIDs.contains(deviceID) {
                config.pairedDeviceIDs.append(deviceID)
            }
            do {
                try config.save()
                self?.config = config
                self?.statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
                self?.statusController?.setDeviceThemeColors(config.deviceThemeColors)
                self?.statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
                self?.statusController?.setDeviceThemeSizes(config.deviceThemeSizes)
                self?.coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
                self?.coordinator?.checkFirmwareAfterPairing(deviceID: deviceID)
                self?.syncF5Suppressor()
            } catch {
                self?.statusController?.setStatus("Pair save failed")
            }
        }
        pairDeviceWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func updateFirmwareFromLatest(for deviceID: String) {
        let updateWindow = FirmwareUpdateWindowController(fileName: "VS-\(deviceID)")
        updateWindow.onCancel = { [weak self] in
            self?.coordinator?.cancelFirmwareUpdate()
        }
        firmwareUpdateWindowController = updateWindow
        showDockIconWhileWindowVisible(updateWindow)
        updateWindow.show()

        coordinator?.updateFirmwareFromLatest(for: deviceID, progress: { [weak self] progress in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.update(progress: progress)
            }
        }, completion: { [weak self] result in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.finish(result: result)
            }
        })
    }

    // MARK: - 设备级设置窗口（对齐 Windows 设备子菜单入口）

    /// 保存语义（对齐 Windows onSettingsChanged）：override==nil（与全局默认一致）
    /// → 清除覆盖回落默认；否则写入覆盖。仅对话框 Save 时持久化并热更。
    private func updateDeviceInteractionSettings(deviceID: String, override: InteractionSettings?) {
        var config = self.config
        if let override {
            config.deviceInteractionSettings[deviceID] = override
        } else {
            config.deviceInteractionSettings.removeValue(forKey: deviceID)
        }
        do {
            try config.save()
            self.config = config
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Interaction settings save failed")
        }
    }

    private func updateDeviceEncoderSettings(deviceID: String, override: EncoderSettings?) {
        var config = self.config
        if let override {
            config.deviceEncoderSettings[deviceID] = override
        } else {
            config.deviceEncoderSettings.removeValue(forKey: deviceID)
        }
        do {
            try config.save()
            self.config = config
            coordinator?.updateConfig(config)
        } catch {
            statusController?.setStatus("Encoder settings save failed")
        }
    }

    private func updateDeviceXiaomiSettings(deviceID: String, override: XiaomiSettings?) {
        var config = self.config
        if let override {
            config.deviceXiaomiSettings[deviceID] = override
        } else {
            config.deviceXiaomiSettings.removeValue(forKey: deviceID)
        }
        do {
            try config.save()
            self.config = config
            // 小米会话参数在会话创建时消费（resolver 实时读 config），
            // 提示文案已告知"下次连接生效"，无需重配已连接会话。
        } catch {
            statusController?.setStatus("Remote settings save failed")
        }
    }

    private func showInteractionSettings(for deviceID: String) {
        let controller = InteractionSettingsWindowController(
            deviceID: deviceID,
            settings: config.interactionSettings(for: deviceID),
            defaults: config.interactionSettings
        ) { [weak self] override in
            self?.updateDeviceInteractionSettings(deviceID: deviceID, override: override)
        }
        interactionSettingsWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func showEncoderSettings(for deviceID: String) {
        let controller = EncoderSettingsWindowController(
            deviceID: deviceID,
            settings: config.encoderSettings(for: deviceID),
            defaults: config.encoderSettings
        ) { [weak self] override in
            self?.updateDeviceEncoderSettings(deviceID: deviceID, override: override)
        }
        encoderSettingsWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    private func showRemoteSettings(for deviceID: String) {
        let controller = RemoteSettingsWindowController(
            deviceID: deviceID,
            settings: XiaomiSettings(
                gainDb: config.xiaomiSettings(for: deviceID).gainDb,
                doubleClickMs: config.xiaomiSettings(for: deviceID).doubleClickMs
            ),
            defaults: .default
        ) { [weak self] override in
            self?.updateDeviceXiaomiSettings(deviceID: deviceID, override: override)
        }
        remoteSettingsWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    /// 按键映射窗口（小米遥控器按键自定义一期；入口对齐遥控器设置）。
    private func showButtonMapping(for deviceID: String) {
        let controller = ButtonMappingWindowController(
            deviceID: deviceID,
            settings: config.buttonsSettings(for: deviceID),
            defaults: config.buttonsSettings
        ) { [weak self] override in
            self?.updateDeviceButtonsSettings(deviceID: deviceID, override: override)
        }
        controller.onInterceptToggled = { [weak self] enabled in
            self?.updateButtonsIntercept(deviceID: deviceID, enabled: enabled)
        }
        buttonMappingWindowController = controller
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    /// 保存语义同 updateDeviceEncoderSettings；语音键双击动作消费 config，
    /// 须走 coordinator.updateConfig 热更即时生效。
    private func updateDeviceButtonsSettings(deviceID: String, override: ButtonsSettings?) {
        var config = self.config
        if let override {
            config.deviceButtonsSettings[deviceID] = override
        } else {
            config.deviceButtonsSettings.removeValue(forKey: deviceID)
        }
        do {
            try config.save()
            self.config = config
            coordinator?.updateConfig(config)
            syncButtonsIntercept()
        } catch {
            statusController?.setStatus("Button mapping save failed")
        }
    }

    /// HID 拦截总开关即时启停（二期；来自按键映射窗 checkbox）：写回设备覆盖并落盘，
    /// 与全局默认一致时清除覆盖；随后即时同步拦截层。其他映射字段不受影响（只改 intercept）。
    private func updateButtonsIntercept(deviceID: String, enabled: Bool) {
        var config = self.config
        var settings = config.buttonsSettings(for: deviceID)
        settings.intercept = enabled
        if settings == config.buttonsSettings {
            config.deviceButtonsSettings.removeValue(forKey: deviceID)
        } else {
            config.deviceButtonsSettings[deviceID] = settings
        }
        do {
            try config.save()
            self.config = config
            coordinator?.updateConfig(config)
            syncButtonsIntercept()
        } catch {
            statusController?.setStatus("Button intercept save failed")
        }
    }

    /// 电量监测窗口（对齐 Windows ShowBatteryMonitorDialog）：单实例——同设备重入
    /// 仅置前；换设备销毁重建（旧监测会话丢弃）。开窗后用缓存的 usb_auto_off
    /// 状态立即同步一次。
    private func showBatteryMonitor(for deviceID: String) {
        if let existing = batteryMonitorWindowController {
            if existing.deviceID == deviceID {
                existing.show()
                return
            }
            existing.close()
            batteryMonitorWindowController = nil
        }
        let controller = BatteryMonitorWindowController(deviceID: deviceID) { [weak self] payload in
            self?.coordinator?.sendPowerLogCommand(deviceID: deviceID, payload: payload)
        }
        controller.onClosed = { [weak self, weak controller] in
            guard let controller, self?.batteryMonitorWindowController === controller else { return }
            self?.batteryMonitorWindowController = nil
        }
        batteryMonitorWindowController = controller
        if let cached = usbAutoOffByDeviceID[deviceID] {
            controller.setInitialUsbAutoOff(cached)
        }
        showDockIconWhileWindowVisible(controller)
        controller.show()
    }

    /// 本地文件固件更新（对齐 Windows Update Firmware from File...）：
    /// NSOpenPanel 选 .bin → 现有 FirmwareUpdateWindowController + BLE OTA。
    private func updateFirmwareFromFile(for deviceID: String) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.init(filenameExtension: "bin")].compactMap { $0 }
        guard panel.runModal() == .OK, let url = panel.url else { return }

        let updateWindow = FirmwareUpdateWindowController(fileName: tr(.firmwareLocalFile))
        updateWindow.onCancel = { [weak self] in
            self?.coordinator?.cancelFirmwareUpdate()
        }
        firmwareUpdateWindowController = updateWindow
        showDockIconWhileWindowVisible(updateWindow)
        updateWindow.show()

        coordinator?.updateFirmware(from: url, for: deviceID, progress: { [weak self] progress in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.update(progress: progress)
            }
        }, completion: { [weak self] result in
            DispatchQueue.main.async {
                self?.firmwareUpdateWindowController?.finish(result: result)
            }
        })
    }

    private func showFirmwareUpdatePrompt(deviceID: String,
                                          currentVersion: String,
                                          latestVersion: String,
                                          isBelowMinimum: Bool) {
        let alert = NSAlert()
        alert.messageText = isBelowMinimum
            ? tr(.firmwarePromptTitleRequired)
            : tr(.firmwarePromptTitleAvailable)
        alert.informativeText = tr(.firmwarePromptBody, deviceID, currentVersion, latestVersion)
        alert.addButton(withTitle: tr(.firmwarePromptUpdateButton))
        alert.addButton(withTitle: tr(.firmwarePromptLaterButton))
        if alert.runModal() == .alertFirstButtonReturn {
            updateFirmwareFromLatest(for: deviceID)
        }
    }

    /// 辅助功能权限缺失引导弹窗（每次启动最多一次；注入路径的静默失败显式化）。
    /// 独立 .app 的 TCC 身份与终端不同，用户必须对本 app 单独授权。
    private var accessibilityAlertShown = false

    private func showAccessibilityPermissionAlertOnce() {
        guard !accessibilityAlertShown else { return }
        accessibilityAlertShown = true
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = tr(.accessibilityAlertTitle)
        alert.informativeText = tr(.accessibilityAlertBody)
        alert.addButton(withTitle: tr(.onboardingOpenAccessibilitySettings))
        alert.addButton(withTitle: tr(.cancel))
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            Self.openAccessibilitySettingsPane()
        }
    }

    /// 打开「系统设置 → 隐私与安全性 → 辅助功能」（与 Onboarding 同一 URL 清单）。
    static func openAccessibilitySettingsPane() {
        for raw in [
            "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility",
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility",
        ] {
            if let url = URL(string: raw), NSWorkspace.shared.open(url) { return }
        }
    }

    private func showDockIconWhileWindowVisible(_ windowController: NSWindowController) {
        configureApplicationIcon()
        NSApp.setActivationPolicy(.regular)
        configureApplicationIcon()
        guard let window = windowController.window else { return }
        dockIconWindowIDs.insert(ObjectIdentifier(window))

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(windowWillCloseForDockIcon),
            name: NSWindow.willCloseNotification,
            object: window
        )
    }

    @objc private func windowWillCloseForDockIcon(_ notification: Notification) {
        if let window = notification.object as? NSWindow {
            dockIconWindowIDs.remove(ObjectIdentifier(window))
        }
        NotificationCenter.default.removeObserver(
            self,
            name: NSWindow.willCloseNotification,
            object: notification.object
        )
        hideDockIconIfNoWindowsAreVisible()
    }

    private func hideDockIconIfNoWindowsAreVisible() {
        DispatchQueue.main.async {
            if self.dockIconWindowIDs.isEmpty {
                NSApp.setActivationPolicy(.accessory)
            }
        }
    }

    private static func applicationIconImage() -> NSImage? {
        let fileManager = FileManager.default
        let cwd = URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true)
        let executableDirectory = Bundle.main.executableURL?.deletingLastPathComponent()

        let candidateURLs = [
            Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
            Bundle.main.resourceURL?.appendingPathComponent("AppIcon.icns"),
            cwd.appendingPathComponent("Resources/AppIcon.icns"),
            cwd.appendingPathComponent("desktop/macos/Resources/AppIcon.icns"),
            executableDirectory?.appendingPathComponent("../../Resources/AppIcon.icns").standardizedFileURL,
            executableDirectory?.appendingPathComponent("../../../Resources/AppIcon.icns").standardizedFileURL,
            executableDirectory?.appendingPathComponent("../../../../Resources/AppIcon.icns").standardizedFileURL
        ]

        for url in candidateURLs.compactMap({ $0 }) where fileManager.fileExists(atPath: url.path) {
            if let image = NSImage(contentsOf: url) {
                return image
            }
        }
        return nil
    }

    private func forgetDevice(_ deviceID: String) {
        var config = AppConfig.load()
        // 对齐 Windows RemovePairedDevice：连带清 paired_device 条目与全部按设备
        // 覆盖（内部已存盘）；内存 config 同步刷新，F5 门控据此卸载（忘掉最后一台
        // RC 时 stop tap）。
        config.removePairedDevice(id: deviceID)
        self.config = config
        statusController?.setPairedDeviceIDs(config.pairedDeviceIDs)
        statusController?.setDeviceThemeColors(config.deviceThemeColors)
        statusController?.setDeviceOverlayPositions(config.deviceOverlayPositions)
        statusController?.setDeviceThemeSizes(config.deviceThemeSizes)
        statusController?.setConnectedDevices([])
        coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
        syncF5Suppressor()
    }
}
