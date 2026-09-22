import AppKit

/// 设置窗口（布局与显隐规则对齐 Windows SettingsDialog）：
/// - 通用：界面语言下拉（System/English/简体中文）+ 开发者模式勾选（始终可见，实时重排）。
/// - 语音识别：Provider（VoiceStick Cloud 仅老配置仍为其时插入，对齐 Windows 下拉下线策略）、
///   API Key/资源 ID（开发者模式可见；资源 ID 仅 Volcengine）、热词。
///   腾讯云的 API Key 框即 SecretId（对齐 Windows，SecretKey/AppId 只走 config.toml 手编）。
/// - 文本精修：LLM Base URL/API Key/Model（开发者模式），精修勾选与提示词（勾选可见）。
/// - 输出：输出目标（开发者模式；macOS 无微信输入法项）。
/// - 系统（开发者模式）：开机自启（macOS 13+）、IMU 调试。
/// - 音频文件（开发者模式）：调试音频开关与目录。
/// 语言保存在 AppDelegate onConfigChanged 生效（重建托盘菜单）；本窗口自身靠
/// 关闭后重开重建文案（AppDelegate 保存后丢弃缓存的本控制器）。
final class SettingsWindowController: NSWindowController {
    private let languagePopup = NSPopUpButton()
    private let developerModeButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let providerPopup = NSPopUpButton()
    private let apiKeyField = NSSecureTextField()
    private let applyTrialAPIKeyButton = NSButton(title: "", target: nil, action: nil)
    private let resourcePopup = NSPopUpButton()
    private let hotwordsTextView = NSTextView()
    private let hotwordsScrollView = NSScrollView()
    private let llmBaseURLField = NSTextField()
    private let llmAPIKeyField = NSSecureTextField()
    private let llmModelField = NSTextField()
    private let refineButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let refinePromptTextView = NSTextView()
    private let refinePromptScrollView = NSScrollView()
    private let outputTargetPopup = NSPopUpButton()
    private let launchAtLoginButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let imuDebugButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let debugAudioButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let debugAudioDirectoryField = NSTextField()
    private let statusLabel = NSTextField(labelWithString: "")

    private let stack = NSStackView()
    private var apiKeyRow: NSStackView?
    private var resourceRow: NSStackView?
    private var refinePromptRow: NSStackView?
    private var llmBaseURLRow: NSStackView?
    private var llmAPIKeyRow: NSStackView?
    private var llmModelRow: NSStackView?
    private var outputSectionTitle: NSView?
    private var outputRow: NSStackView?
    private var systemSectionTitle: NSView?
    private var launchAtLoginRow: NSStackView?
    private var imuDebugRow: NSStackView?
    private var audioSectionTitle: NSView?
    private var debugAudioRow: NSStackView?
    private var debugDirRow: NSStackView?

    private var currentDisplayedProvider: ASRProvider = .volcengine
    /// 开发者模式勾选状态（实时重排用，Save 时落盘）。
    private var developerMode = false
    var onConfigChanged: ((AppConfig) -> Void)?

    private var config: AppConfig

    init(config: AppConfig = AppConfig.load()) {
        self.config = config
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 560, height: 560),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.settingsTitle)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        buildContent()
        loadConfigIntoFields()
        resizeWindowToFit(animated: false)
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(apiKeyFieldDidChange),
            name: NSControl.textDidChangeNotification,
            object: apiKeyField
        )
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    func show() {
        config = AppConfig.load()
        loadConfigIntoFields()
        resizeWindowToFit(animated: false)
        showWindow(nil)
        window?.makeFirstResponder(providerPopup)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        // ===== 通用 =====
        stack.addArrangedSubview(sectionTitle(tr(.settingsSectionGeneral)))
        languagePopup.addItems(withTitles: UiLanguage.allCases.map { $0.displayName })
        stack.addArrangedSubview(row(label: tr(.settingsLanguage), control: languagePopup))
        developerModeButton.title = tr(.settingsDeveloperMode)
        developerModeButton.target = self
        developerModeButton.action = #selector(developerModeToggled)
        stack.addArrangedSubview(row(label: "", control: developerModeButton))

        // ===== 语音识别 =====
        stack.addArrangedSubview(sectionTitle(tr(.settingsSectionAsr)))
        configureProviderPopup()
        stack.addArrangedSubview(row(label: tr(.settingsProvider), control: providerPopup))
        configureApplyTrialAPIKeyButton()
        let apiKeyRow = row(label: tr(.settingsApiKey), control: apiKeyControl())
        self.apiKeyRow = apiKeyRow
        stack.addArrangedSubview(apiKeyRow)
        configureResourcePopup()
        let resourceRow = row(label: tr(.settingsResourceId), control: resourcePopup)
        self.resourceRow = resourceRow
        stack.addArrangedSubview(resourceRow)
        configureMultiline(textView: hotwordsTextView, scrollView: hotwordsScrollView, height: 78)
        stack.addArrangedSubview(row(label: tr(.settingsHotwords), control: hotwordsScrollView))
        stack.addArrangedSubview(hintRow(tr(.settingsHotwordsHint)))

        // ===== 文本精修 =====
        stack.addArrangedSubview(sectionTitle(tr(.settingsSectionRefine)))
        let llmBaseURLRow = row(label: tr(.settingsLlmBaseUrl), control: llmBaseURLField)
        let llmAPIKeyRow = row(label: tr(.settingsApiKey), control: llmAPIKeyField)
        let llmModelRow = row(label: tr(.settingsLlmModel), control: llmModelField)
        self.llmBaseURLRow = llmBaseURLRow
        self.llmAPIKeyRow = llmAPIKeyRow
        self.llmModelRow = llmModelRow
        stack.addArrangedSubview(llmBaseURLRow)
        stack.addArrangedSubview(llmAPIKeyRow)
        stack.addArrangedSubview(llmModelRow)
        refineButton.title = tr(.settingsRefineText)
        refineButton.target = self
        refineButton.action = #selector(refineToggled)
        stack.addArrangedSubview(row(label: "", control: refineButton))
        configureMultiline(textView: refinePromptTextView, scrollView: refinePromptScrollView, height: 72)
        let refinePromptRow = row(label: tr(.settingsRefinePrompt), control: refinePromptScrollView)
        self.refinePromptRow = refinePromptRow
        stack.addArrangedSubview(refinePromptRow)

        // ===== 输出 =====
        let outputTitle = sectionTitle(tr(.settingsSectionOutput))
        outputSectionTitle = outputTitle
        stack.addArrangedSubview(outputTitle)
        outputTargetPopup.addItems(withTitles: [
            OutputTarget.focusedApp.displayName,
            OutputTarget.subtitle.displayName
        ])
        let outputRow = row(label: tr(.settingsOutputTarget), control: outputTargetPopup)
        self.outputRow = outputRow
        stack.addArrangedSubview(outputRow)

        // ===== 系统 =====
        let systemTitle = sectionTitle(tr(.settingsSectionSystem))
        systemSectionTitle = systemTitle
        stack.addArrangedSubview(systemTitle)
        launchAtLoginButton.title = tr(.settingsLaunchAtLogin)
        let launchAtLoginRow = row(label: "", control: launchAtLoginButton)
        self.launchAtLoginRow = launchAtLoginRow
        stack.addArrangedSubview(launchAtLoginRow)
        imuDebugButton.title = tr(.settingsShowImuDebug)
        let imuDebugRow = row(label: "", control: imuDebugButton)
        self.imuDebugRow = imuDebugRow
        stack.addArrangedSubview(imuDebugRow)

        // ===== 音频文件 =====
        let audioTitle = sectionTitle(tr(.settingsSectionAudioFiles))
        audioSectionTitle = audioTitle
        stack.addArrangedSubview(audioTitle)
        debugAudioButton.title = tr(.settingsDebugAudio)
        let debugAudioRow = row(label: "", control: debugAudioButton)
        self.debugAudioRow = debugAudioRow
        stack.addArrangedSubview(debugAudioRow)
        let debugDirControl = NSStackView()
        debugDirControl.orientation = .horizontal
        debugDirControl.alignment = .centerY
        debugDirControl.spacing = 8
        debugAudioDirectoryField.isEditable = false
        debugAudioDirectoryField.lineBreakMode = .byTruncatingMiddle
        let chooseButton = NSButton(title: tr(.settingsChooseDir), target: self, action: #selector(chooseDebugDirectory))
        debugDirControl.addArrangedSubview(debugAudioDirectoryField)
        debugDirControl.addArrangedSubview(chooseButton)
        debugAudioDirectoryField.widthAnchor.constraint(equalToConstant: 260).isActive = true
        let debugDirRow = row(label: tr(.settingsDebugDir), control: debugDirControl)
        self.debugDirRow = debugDirRow
        stack.addArrangedSubview(debugDirRow)

        // ===== 底部按钮行 =====
        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 10
        let openFolderButton = NSButton(title: tr(.settingsOpenConfigFolder), target: self, action: #selector(openConfigFolder))
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let saveButton = NSButton(title: tr(.save), target: self, action: #selector(saveSettings))
        saveButton.keyEquivalent = "\r"
        buttonRow.addArrangedSubview(openFolderButton)
        buttonRow.addArrangedSubview(statusLabel)
        buttonRow.addArrangedSubview(spacer)
        buttonRow.addArrangedSubview(saveButton)
        stack.addArrangedSubview(buttonRow)
        buttonRow.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        statusLabel.textColor = .secondaryLabelColor

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -24),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 24)
        ])
    }

    private func configureResourcePopup() {
        resourcePopup.addItems(withTitles: AppConfig.supportedResourceIDs)
    }

    private func configureProviderPopup() {
        providerPopup.target = self
        providerPopup.action = #selector(providerSelectionChanged)
    }

    /// 重建 Provider 下拉项（对齐 Windows：VoiceStick Cloud 已下线，仅老配置仍为其时插入）。
    private func rebuildProviderPopupItems() {
        providerPopup.removeAllItems()
        if config.asrProvider == .voiceStickCloud {
            providerPopup.addItem(withTitle: ASRProvider.voiceStickCloud.displayName)
        }
        providerPopup.addItems(withTitles: [
            ASRProvider.volcengine.displayName,
            ASRProvider.tencent.displayName
        ])
    }

    private func configureApplyTrialAPIKeyButton() {
        applyTrialAPIKeyButton.title = tr(.settingsApplyTrial)
        applyTrialAPIKeyButton.target = self
        applyTrialAPIKeyButton.action = #selector(applyTrialAPIKey)
    }

    private func apiKeyControl() -> NSStackView {
        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 8
        apiKeyField.widthAnchor.constraint(greaterThanOrEqualToConstant: 190).isActive = true
        applyTrialAPIKeyButton.widthAnchor.constraint(equalToConstant: 102).isActive = true
        stack.addArrangedSubview(apiKeyField)
        stack.addArrangedSubview(applyTrialAPIKeyButton)
        return stack
    }

    private func configureMultiline(textView: NSTextView, scrollView: NSScrollView, height: CGFloat) {
        scrollView.hasVerticalScroller = true
        scrollView.borderType = .bezelBorder
        scrollView.heightAnchor.constraint(equalToConstant: height).isActive = true
        scrollView.translatesAutoresizingMaskIntoConstraints = false

        textView.isRichText = false
        textView.isEditable = true
        textView.isSelectable = true
        textView.font = .systemFont(ofSize: 13)
        textView.textColor = .textColor
        textView.backgroundColor = .textBackgroundColor
        textView.drawsBackground = true
        textView.textContainerInset = NSSize(width: 4, height: 4)
        textView.minSize = NSSize(width: 0, height: scrollView.contentSize.height)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.autoresizingMask = [.width]
        textView.frame = NSRect(origin: .zero, size: NSSize(width: 300, height: height))
        textView.textContainer?.containerSize = NSSize(
            width: textView.frame.width,
            height: CGFloat.greatestFiniteMagnitude
        )
        textView.textContainer?.widthTracksTextView = true
        scrollView.documentView = textView
    }

    private func loadConfigIntoFields() {
        if let languageIndex = UiLanguage.allCases.firstIndex(of: config.uiLanguage) {
            languagePopup.selectItem(at: languageIndex)
        }
        developerMode = config.developerMode
        developerModeButton.state = developerMode ? .on : .off

        currentDisplayedProvider = config.asrProvider
        rebuildProviderPopupItems()
        providerPopup.selectItem(withTitle: config.asrProvider.displayName)
        apiKeyField.stringValue = apiKey(for: config.asrProvider)
        hotwordsTextView.string = config.asrHotwords.joined(separator: ",")

        llmBaseURLField.stringValue = config.llmBaseURL
        llmAPIKeyField.stringValue = config.llmAPIKey
        llmModelField.stringValue = config.llmModel
        refineButton.state = config.refineEnabled ? .on : .off
        // 提示词留空表示用内置默认：编辑框显示默认全文，保存时与默认一致则回写空串
        //（对齐 Windows LoadConfigIntoControls/SaveSettings 的往返语义）。
        refinePromptTextView.string = config.refinePrompt.isEmpty
            ? LLMRefinementClient.buildPrompt(override: "", hotwords: [])
            : config.refinePrompt

        outputTargetPopup.selectItem(withTitle: config.defaultOutputProfile.target.displayName)
        launchAtLoginButton.state = config.launchAtLogin ? .on : .off
        imuDebugButton.state = config.showIMUDebug ? .on : .off
        debugAudioButton.state = config.debugAudioCache ? .on : .off
        debugAudioDirectoryField.stringValue = config.debugAudioDirectory.path

        if resourcePopup.itemTitles.contains(config.resourceID) {
            resourcePopup.selectItem(withTitle: config.resourceID)
        }
        updateVisibility()
        statusLabel.stringValue = ""
    }

    /// 行显隐（对齐 Windows Relayout 的 vis 谓词）：隐藏行在 NSStackView 中不占位。
    private func updateVisibility() {
        apiKeyRow?.isHidden = !developerMode
        resourceRow?.isHidden = !(developerMode && currentDisplayedProvider == .volcengine)
        llmBaseURLRow?.isHidden = !developerMode
        llmAPIKeyRow?.isHidden = !developerMode
        llmModelRow?.isHidden = !developerMode
        refinePromptRow?.isHidden = refineButton.state != .on

        outputSectionTitle?.isHidden = !developerMode
        outputRow?.isHidden = !developerMode

        systemSectionTitle?.isHidden = !developerMode
        if #available(macOS 13.0, *) {
            launchAtLoginRow?.isHidden = !developerMode
        } else {
            // SMAppService.mainApp 需 macOS 13+，低版本整行隐藏
            launchAtLoginRow?.isHidden = true
        }
        imuDebugRow?.isHidden = !developerMode

        audioSectionTitle?.isHidden = !developerMode
        debugAudioRow?.isHidden = !developerMode
        debugDirRow?.isHidden = !developerMode

        updateApplyTrialButton()
    }

    /// 按可见行动态调整窗口高度（保持顶边位置不动）。
    private func resizeWindowToFit(animated: Bool) {
        guard let window, let contentView = window.contentView else { return }
        contentView.layoutSubtreeIfNeeded()
        let targetHeight = stack.fittingSize.height + 48
        guard abs(contentView.frame.height - targetHeight) > 1 else { return }
        var frame = window.frame
        let delta = targetHeight - contentView.frame.height
        frame.size.height += delta
        frame.origin.y -= delta
        window.setFrame(frame, display: true, animate: animated)
    }

    @objc private func developerModeToggled() {
        developerMode = developerModeButton.state == .on
        updateVisibility()
        resizeWindowToFit(animated: true)
    }

    @objc private func refineToggled() {
        updateVisibility()
        resizeWindowToFit(animated: true)
    }

    @objc private func providerSelectionChanged() {
        saveDisplayedAPIKey()
        currentDisplayedProvider = selectedProvider()
        config.asrProvider = currentDisplayedProvider
        apiKeyField.stringValue = apiKey(for: currentDisplayedProvider)
        updateVisibility()
        resizeWindowToFit(animated: true)
    }

    @objc private func apiKeyFieldDidChange() {
        updateApplyTrialButton()
    }

    @objc private func applyTrialAPIKey() {
        saveDisplayedAPIKey()
        guard currentDisplayedProvider == .voiceStickCloud else { return }

        applyTrialAPIKeyButton.isEnabled = false
        statusLabel.stringValue = tr(.settingsApplyingTrial)
        VoiceStickCloudAPI.applyTrialAPIKey(
            cloudURL: config.voiceStickCloudURL,
            deviceID: config.pairedDeviceIDs.first
        ) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.applyTrialAPIKeyButton.isEnabled = true
                switch result {
                case .success(.apiKey(let apiKey)):
                    self.config.voiceStickAPIKey = apiKey
                    self.apiKeyField.stringValue = apiKey
                    self.statusLabel.stringValue = tr(.settingsTrialApplied)
                    self.updateApplyTrialButton()
                case .success(.url(let url)):
                    self.statusLabel.stringValue = tr(.settingsTrialPageOpened)
                    if !NSWorkspace.shared.open(url) {
                        self.showErrorAlert(
                            title: tr(.settingsTrialOpenFailedTitle),
                            message: url.absoluteString
                        )
                    }
                case .failure(let error):
                    self.statusLabel.stringValue = ""
                    self.showErrorAlert(
                        title: tr(.settingsTrialFailedTitle),
                        message: error.localizedDescription
                    )
                    self.updateApplyTrialButton()
                }
            }
        }
    }

    @objc private func chooseDebugDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = URL(fileURLWithPath: debugAudioDirectoryField.stringValue)
        if panel.runModal() == .OK, let url = panel.url {
            debugAudioDirectoryField.stringValue = url.path
        }
    }

    @objc private func saveSettings() {
        saveDisplayedAPIKey()
        let provider = selectedProvider()
        let resourceID = resourcePopup.titleOfSelectedItem ?? config.resourceID

        var outputProfile = config.defaultOutputProfile
        outputProfile.target = outputTargetPopup.titleOfSelectedItem == OutputTarget.subtitle.displayName
            ? .subtitle
            : .focusedApp

        // 与内置默认一致的精修提示词回写空串（对齐 Windows SaveSettings）。
        let refinePrompt = refinePromptTextView.string
        let defaultRefinePrompt = LLMRefinementClient.buildPrompt(override: "", hotwords: [])
        let savedRefinePrompt = refinePrompt == defaultRefinePrompt ? "" : refinePrompt

        config = AppConfig(
            asrProvider: provider,
            voiceStickAPIKey: config.voiceStickAPIKey,
            voiceStickCloudURL: config.voiceStickCloudURL,
            volcengineAPIKey: config.volcengineAPIKey,
            tencentSecretID: config.tencentSecretID,
            tencentSecretKey: config.tencentSecretKey,
            tencentAppid: config.tencentAppid,
            tencentEngineModelType: config.tencentEngineModelType,
            tencentHotwordID: config.tencentHotwordID,
            llmBaseURL: llmBaseURLField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines),
            llmAPIKey: llmAPIKeyField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines),
            llmModel: llmModelField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines),
            refineEnabled: refineButton.state == .on,
            refinePrompt: savedRefinePrompt,
            llmDisableThinking: config.llmDisableThinking,
            interactionMode: config.interactionMode,
            uiLanguage: selectedLanguage(),
            resourceID: resourceID,
            asrHotwords: AppConfig.hotwordList(hotwordsTextView.string),
            pairedDeviceIDs: config.pairedDeviceIDs,
            deviceThemeColors: config.deviceThemeColors,
            deviceOverlayPositions: config.deviceOverlayPositions,
            deviceThemeSizes: config.deviceThemeSizes,
            defaultOutputProfile: outputProfile,
            deviceOutputProfiles: config.deviceOutputProfiles,
            autoEnter: config.autoEnter,
            debugAudioCache: debugAudioButton.state == .on,
            debugAudioDirectory: URL(fileURLWithPath: debugAudioDirectoryField.stringValue, isDirectory: true),
            pairedDevices: config.pairedDevices,
            deviceXiaomiSettings: config.deviceXiaomiSettings,
            xiaomiSuppressF5: config.xiaomiSuppressF5,
            globalHotkeyEnabled: config.globalHotkeyEnabled,
            globalHotkey: config.globalHotkey,
            launchAtLogin: launchAtLoginButton.state == .on,
            developerMode: developerMode,
            showIMUDebug: imuDebugButton.state == .on,
            interactionSettings: config.interactionSettings,
            deviceInteractionSettings: config.deviceInteractionSettings,
            encoderSettings: config.encoderSettings,
            deviceEncoderSettings: config.deviceEncoderSettings,
            deviceButtonsSettings: config.deviceButtonsSettings
        )

        do {
            try config.save()
            onConfigChanged?(config)
            statusLabel.stringValue = tr(.settingsSaved)
            window?.close()
        } catch {
            statusLabel.stringValue = ""
            showErrorAlert(title: tr(.settingsSaveFailedTitle), message: error.localizedDescription)
        }
    }

    /// 语言下拉按 indexOfSelectedItem 映射 UiLanguage.allCases（显示名随界面语言变，
    /// 用索引而非标题比对）。
    private func selectedLanguage() -> UiLanguage {
        let index = languagePopup.indexOfSelectedItem
        guard index >= 0, index < UiLanguage.allCases.count else { return config.uiLanguage }
        return UiLanguage.allCases[index]
    }

    @objc private func openConfigFolder() {
        AppConfig.openConfigDirectory()
    }

    private func selectedProvider() -> ASRProvider {
        switch providerPopup.titleOfSelectedItem {
        case ASRProvider.voiceStickCloud.displayName:
            return .voiceStickCloud
        case ASRProvider.volcengine.displayName:
            return .volcengine
        case ASRProvider.tencent.displayName:
            return .tencent
        default:
            return config.asrProvider
        }
    }

    private func apiKey(for provider: ASRProvider) -> String {
        switch provider {
        case .voiceStickCloud:
            return config.voiceStickAPIKey
        case .volcengine:
            return config.volcengineAPIKey
        case .tencent:
            return config.tencentSecretID
        }
    }

    private func saveDisplayedAPIKey() {
        let value = apiKeyField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        switch currentDisplayedProvider {
        case .voiceStickCloud:
            config.voiceStickAPIKey = value
        case .volcengine:
            config.volcengineAPIKey = value
        case .tencent:
            config.tencentSecretID = value
        }
    }

    private func updateApplyTrialButton() {
        let isCloud = currentDisplayedProvider == .voiceStickCloud
        let isEmpty = apiKeyField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        applyTrialAPIKeyButton.isHidden = !(developerMode && isCloud && isEmpty)
    }

    private func showErrorAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: tr(.ok))
        if let window {
            alert.beginSheetModal(for: window)
        } else {
            alert.runModal()
        }
    }

    private func sectionTitle(_ title: String) -> NSTextField {
        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 13, weight: .semibold)
        label.textColor = .secondaryLabelColor
        return label
    }

    private func row(label: String, control: NSView) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 12
        let labelView = NSTextField(labelWithString: label)
        labelView.alignment = .right
        labelView.textColor = .secondaryLabelColor
        labelView.widthAnchor.constraint(equalToConstant: 120).isActive = true
        if control is NSTextField || control is NSPopUpButton || control is NSStackView || control is NSScrollView {
            control.widthAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        }
        row.addArrangedSubview(labelView)
        row.addArrangedSubview(control)
        return row
    }

    private func hintRow(_ text: String) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 12

        let spacer = NSView()
        spacer.widthAnchor.constraint(equalToConstant: 120).isActive = true

        let label = NSTextField(labelWithString: text)
        label.textColor = .secondaryLabelColor
        label.font = .systemFont(ofSize: 11)
        label.widthAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true

        row.addArrangedSubview(spacer)
        row.addArrangedSubview(label)
        return row
    }
}
