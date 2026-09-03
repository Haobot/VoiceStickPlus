import AppKit

/// 编码器设置窗口（对齐 Windows EncoderSettingsDialog）：13 行设置 +
/// Restore defaults/Save/Cancel。press/double-click action=Custom key 时对应 key 行
/// 可见（NSStackView isHidden 不占位，联动刷新）。
/// 保存语义（对齐 Windows SaveSettings）：edited 从结构默认重建所有字段；
/// action != key 时对应 key 字段重置为结构默认空串；数字字段解析失败回落传入的
/// 全局默认；6 个 key 字段校验 `空 || KeySpec.parse`，失败弹 alert 中止保存；
/// 与全局默认一致 → onSave(nil)（调用方清除覆盖，回落默认）。
final class EncoderSettingsWindowController: NSWindowController, NSWindowDelegate {
    private let deviceID: String
    private var settings: EncoderSettings
    private let defaults: EncoderSettings
    private let onSave: (EncoderSettings?) -> Void

    private let toArrowButton = NSButton(
        checkboxWithTitle: "", target: nil, action: nil
    )
    private let rotationInvertButton = NSButton(
        checkboxWithTitle: "", target: nil, action: nil
    )
    private let rotateCwKeyField = NSTextField()
    private let rotateCcwKeyField = NSTextField()
    private let fastThresholdField = NSTextField()
    private let rotateCwFastKeyField = NSTextField()
    private let rotateCcwFastKeyField = NSTextField()
    private let decideWindowField = NSTextField()
    private let ledColorPopup = NSPopUpButton()
    private let pressActionPopup = NSPopUpButton()
    private let pressKeyField = NSTextField()
    private let doubleClickActionPopup = NSPopUpButton()
    private let doubleClickKeyField = NSTextField()

    /// key 行容器（联动显隐）。
    private var pressKeyRow: NSStackView?
    private var doubleClickKeyRow: NSStackView?

    init(deviceID: String, settings: EncoderSettings, defaults: EncoderSettings,
         onSave: @escaping (EncoderSettings?) -> Void) {
        self.deviceID = deviceID
        self.settings = settings
        self.defaults = defaults
        self.onSave = onSave

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 520),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.encoderSettingsTitle, deviceID)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        toArrowButton.title = tr(.encoderToArrow)
        rotationInvertButton.title = tr(.encoderRotationInvert)
        buildContent()
        loadSettingsIntoControls()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        let sectionTitle = NSTextField(labelWithString: tr(.settingsSectionEncoder))
        sectionTitle.font = NSFont.boldSystemFont(ofSize: NSFont.systemFontSize)
        stack.addArrangedSubview(sectionTitle)
        stack.addArrangedSubview(toArrowButton)
        stack.addArrangedSubview(rotationInvertButton)
        stack.addArrangedSubview(fieldRow(tr(.encoderRotateCwKey) + ":", field: rotateCwKeyField))
        stack.addArrangedSubview(fieldRow(tr(.encoderRotateCcwKey) + ":", field: rotateCcwKeyField))
        stack.addArrangedSubview(fieldRow(tr(.encoderRotateFastThreshold) + ":", field: fastThresholdField))
        stack.addArrangedSubview(fieldRow(tr(.encoderRotateCwFastKey) + ":", field: rotateCwFastKeyField))
        stack.addArrangedSubview(fieldRow(tr(.encoderRotateCcwFastKey) + ":", field: rotateCcwFastKeyField))
        stack.addArrangedSubview(fieldRow(tr(.encoderDecideWindow) + ":", field: decideWindowField))

        for color in EncoderLedColor.allCases {
            ledColorPopup.addItem(withTitle: color.displayName)
        }
        stack.addArrangedSubview(popupRow(tr(.encoderLedColor) + ":", popup: ledColorPopup))

        for action in EncoderButtonAction.allCases {
            pressActionPopup.addItem(withTitle: action.displayName)
            doubleClickActionPopup.addItem(withTitle: action.displayName)
        }
        pressActionPopup.target = self
        pressActionPopup.action = #selector(actionPopupChanged)
        doubleClickActionPopup.target = self
        doubleClickActionPopup.action = #selector(actionPopupChanged)

        stack.addArrangedSubview(popupRow(tr(.encoderPressAction) + ":", popup: pressActionPopup))
        let pressRow = fieldRow(tr(.encoderPressKey) + ":", field: pressKeyField)
        pressKeyRow = pressRow
        stack.addArrangedSubview(pressRow)
        stack.addArrangedSubview(popupRow(tr(.encoderDoubleClickAction) + ":", popup: doubleClickActionPopup))
        let doubleClickRow = fieldRow(tr(.encoderDoubleClickKey) + ":", field: doubleClickKeyField)
        doubleClickKeyRow = doubleClickRow
        stack.addArrangedSubview(doubleClickRow)

        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 8
        let restoreButton = NSButton(
            title: tr(.restoreDefaults), target: self, action: #selector(restoreDefaults)
        )
        let saveButton = NSButton(title: tr(.save), target: self, action: #selector(saveSettings))
        saveButton.keyEquivalent = "\r"
        let cancelButton = NSButton(title: tr(.cancel), target: self, action: #selector(cancel))
        buttonRow.addArrangedSubview(restoreButton)
        buttonRow.addArrangedSubview(NSView())
        buttonRow.addArrangedSubview(saveButton)
        buttonRow.addArrangedSubview(cancelButton)
        stack.addArrangedSubview(buttonRow)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -16),
            buttonRow.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
    }

    private func fieldRow(_ title: String, field: NSTextField) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        let label = NSTextField(labelWithString: title)
        label.alignment = .right
        label.widthAnchor.constraint(equalToConstant: 190).isActive = true
        field.widthAnchor.constraint(equalToConstant: 170).isActive = true
        row.addArrangedSubview(label)
        row.addArrangedSubview(field)
        return row
    }

    private func popupRow(_ title: String, popup: NSPopUpButton) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        let label = NSTextField(labelWithString: title)
        label.alignment = .right
        label.widthAnchor.constraint(equalToConstant: 190).isActive = true
        popup.widthAnchor.constraint(greaterThanOrEqualToConstant: 170).isActive = true
        row.addArrangedSubview(label)
        row.addArrangedSubview(popup)
        return row
    }

    private func loadSettingsIntoControls() {
        toArrowButton.state = settings.toArrow ? .on : .off
        rotationInvertButton.state = settings.rotationInvert ? .on : .off
        rotateCwKeyField.stringValue = settings.rotateCwKey
        rotateCcwKeyField.stringValue = settings.rotateCcwKey
        fastThresholdField.stringValue = "\(settings.rotateFastThreshold)"
        rotateCwFastKeyField.stringValue = settings.rotateCwFastKey
        rotateCcwFastKeyField.stringValue = settings.rotateCcwFastKey
        decideWindowField.stringValue = "\(settings.rotateDecideWindowMs)"
        ledColorPopup.selectItem(withTitle: settings.ledColor.displayName)
        pressActionPopup.selectItem(withTitle: settings.pressAction.displayName)
        pressKeyField.stringValue = settings.pressKey
        doubleClickActionPopup.selectItem(withTitle: settings.doubleClickAction.displayName)
        doubleClickKeyField.stringValue = settings.doubleClickKey
        updateActionKeyVisibility()
    }

    /// key 行仅在对应 action=Custom key 时可见（NSStackView 隐藏行不占高度）。
    private func updateActionKeyVisibility() {
        pressKeyRow?.isHidden =
            pressActionPopup.titleOfSelectedItem != EncoderButtonAction.key.displayName
        doubleClickKeyRow?.isHidden =
            doubleClickActionPopup.titleOfSelectedItem != EncoderButtonAction.key.displayName
    }

    @objc private func actionPopupChanged() {
        updateActionKeyVisibility()
    }

    @objc private func restoreDefaults() {
        // 只回显不落盘（对齐 Windows RestoreDefaults：Save 才持久化）。
        settings = defaults
        loadSettingsIntoControls()
    }

    @objc private func saveSettings() {
        // edited 从结构默认重建（对齐 Windows：EncoderSettings edited; 默认构造）。
        var edited = EncoderSettings()
        edited.toArrow = toArrowButton.state == .on
        edited.rotationInvert = rotationInvertButton.state == .on
        edited.rotateCwKey = rotateCwKeyField.stringValue
        edited.rotateCcwKey = rotateCcwKeyField.stringValue
        // 数字字段：解析失败时回落全局默认。
        edited.rotateFastThreshold = Int(fastThresholdField.stringValue) ?? defaults.rotateFastThreshold
        edited.rotateCwFastKey = rotateCwFastKeyField.stringValue
        edited.rotateCcwFastKey = rotateCcwFastKeyField.stringValue
        edited.rotateDecideWindowMs = Int(decideWindowField.stringValue) ?? defaults.rotateDecideWindowMs

        let ledIndex = ledColorPopup.indexOfSelectedItem
        edited.ledColor = ledIndex >= 0 && ledIndex < EncoderLedColor.allCases.count
            ? EncoderLedColor.allCases[ledIndex]
            : defaults.ledColor

        let pressIsKey = pressActionPopup.titleOfSelectedItem == EncoderButtonAction.key.displayName
        edited.pressAction = pressIsKey ? .key : .recording
        if pressIsKey {
            edited.pressKey = pressKeyField.stringValue
        }  // 否则 pressKey 保持默认空串

        let doubleClickIsKey =
            doubleClickActionPopup.titleOfSelectedItem == EncoderButtonAction.key.displayName
        edited.doubleClickAction = doubleClickIsKey ? .key : .recording
        if doubleClickIsKey {
            edited.doubleClickKey = doubleClickKeyField.stringValue
        }  // 否则 doubleClickKey 保持默认空串

        // 校验 key_spec 语法：非空字段解析失败则提示并不保存（对齐 Windows）。
        let keys = [
            edited.rotateCwKey, edited.rotateCcwKey,
            edited.rotateCwFastKey, edited.rotateCcwFastKey,
            edited.pressKey, edited.doubleClickKey
        ]
        guard keys.allSatisfy({ $0.isEmpty || KeySpec.parse($0) != nil }) else {
            let alert = NSAlert()
            alert.messageText = window?.title ?? tr(.settingsSectionEncoder)
            alert.informativeText = tr(.encoderInvalidKey)
            alert.alertStyle = .warning
            alert.runModal()
            return
        }

        settings = edited
        close()
        // 与全局默认一致 → nil（调用方清除覆盖，回落默认）。
        onSave(settings == defaults ? nil : settings)
    }

    @objc private func cancel() {
        close()
    }
}
