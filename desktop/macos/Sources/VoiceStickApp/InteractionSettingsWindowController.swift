import AppKit

/// 设备交互设置窗口（对齐 Windows InteractionSettingsDialog）：
/// Wake Sensitivity 下拉（Windows 第一行标签是空串 bug，macOS 用正常标签
/// "Wake Sensitivity"）、Double-tap 勾选、Tap/AirMouse 灵敏度滑块 1...10（数值
/// 实时刷新——Windows 滑块数值不刷新是 quirk，macOS 修复）。
/// 保存语义：与全局默认一致 → onSave(nil)（调用方清除覆盖，回落默认）。
final class InteractionSettingsWindowController: NSWindowController, NSWindowDelegate {
    private let deviceID: String
    private var settings: InteractionSettings
    private let defaults: InteractionSettings
    private let onSave: (InteractionSettings?) -> Void

    private let wakePopup = NSPopUpButton()
    private let tapToArrowButton = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let tapSensitivitySlider = NSSlider()
    private let tapSensitivityValue = NSTextField(labelWithString: "5")
    private let airMouseXSlider = NSSlider()
    private let airMouseXValue = NSTextField(labelWithString: "5")
    private let airMouseYSlider = NSSlider()
    private let airMouseYValue = NSTextField(labelWithString: "5")

    init(deviceID: String, settings: InteractionSettings, defaults: InteractionSettings,
         onSave: @escaping (InteractionSettings?) -> Void) {
        self.deviceID = deviceID
        self.settings = settings
        self.defaults = defaults
        self.onSave = onSave

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 240),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.interactionSettingsTitle, deviceID)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        tapToArrowButton.title = tr(.settingsTapToArrow)
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
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(stack)

        for sensitivity in ImuWakeSensitivity.allCases {
            wakePopup.addItem(withTitle: sensitivity.displayName)
        }

        configureSlider(tapSensitivitySlider, valueLabel: tapSensitivityValue)
        configureSlider(airMouseXSlider, valueLabel: airMouseXValue)
        configureSlider(airMouseYSlider, valueLabel: airMouseYValue)

        stack.addArrangedSubview(labelRow(tr(.settingsWakeSensitivity) + ":", control: wakePopup))
        stack.addArrangedSubview(tapToArrowButton)
        stack.addArrangedSubview(sliderRow(tr(.settingsTapSensitivity) + ":", slider: tapSensitivitySlider,
                                           valueLabel: tapSensitivityValue))
        stack.addArrangedSubview(sliderRow(tr(.settingsAirMouseX) + ":", slider: airMouseXSlider,
                                           valueLabel: airMouseXValue))
        stack.addArrangedSubview(sliderRow(tr(.settingsAirMouseY) + ":", slider: airMouseYSlider,
                                           valueLabel: airMouseYValue))

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
            buttonRow.widthAnchor.constraint(equalTo: stack.widthAnchor),
            wakePopup.widthAnchor.constraint(greaterThanOrEqualToConstant: 180)
        ])
    }

    private func labelRow(_ title: String, control: NSView) -> NSStackView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        let label = NSTextField(labelWithString: title)
        label.alignment = .right
        label.widthAnchor.constraint(equalToConstant: 170).isActive = true
        row.addArrangedSubview(label)
        row.addArrangedSubview(control)
        return row
    }

    private func sliderRow(_ title: String, slider: NSSlider, valueLabel: NSTextField) -> NSStackView {
        let row = labelRow(title, control: slider)
        slider.widthAnchor.constraint(equalToConstant: 170).isActive = true
        valueLabel.widthAnchor.constraint(equalToConstant: 24).isActive = true
        row.addArrangedSubview(valueLabel)
        return row
    }

    private func configureSlider(_ slider: NSSlider, valueLabel: NSTextField) {
        slider.minValue = 1
        slider.maxValue = 10
        slider.numberOfTickMarks = 10
        slider.allowsTickMarkValuesOnly = true
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(sliderChanged(_:))
    }

    private func loadSettingsIntoControls() {
        wakePopup.selectItem(withTitle: settings.imuWakeSensitivity.displayName)
        tapToArrowButton.state = settings.tapToArrow ? .on : .off
        tapSensitivitySlider.integerValue = settings.tapSensitivity
        tapSensitivityValue.stringValue = "\(settings.tapSensitivity)"
        airMouseXSlider.integerValue = settings.airMouseSensitivityX
        airMouseXValue.stringValue = "\(settings.airMouseSensitivityX)"
        airMouseYSlider.integerValue = settings.airMouseSensitivityY
        airMouseYValue.stringValue = "\(settings.airMouseSensitivityY)"
    }

    @objc private func sliderChanged(_ sender: NSSlider) {
        let value = sender.integerValue
        switch sender {
        case tapSensitivitySlider: tapSensitivityValue.stringValue = "\(value)"
        case airMouseXSlider: airMouseXValue.stringValue = "\(value)"
        case airMouseYSlider: airMouseYValue.stringValue = "\(value)"
        default: break
        }
    }

    @objc private func restoreDefaults() {
        // 只回显不落盘（对齐 Windows RestoreDefaults：Save 才持久化）。
        settings = defaults
        loadSettingsIntoControls()
    }

    @objc private func saveSettings() {
        var edited = InteractionSettings()
        let wakeIndex = wakePopup.indexOfSelectedItem
        edited.imuWakeSensitivity = wakeIndex >= 0 && wakeIndex < ImuWakeSensitivity.allCases.count
            ? ImuWakeSensitivity.allCases[wakeIndex]
            : defaults.imuWakeSensitivity
        edited.tapToArrow = tapToArrowButton.state == .on
        edited.tapSensitivity = tapSensitivitySlider.integerValue
        edited.airMouseSensitivityX = airMouseXSlider.integerValue
        edited.airMouseSensitivityY = airMouseYSlider.integerValue
        settings = edited
        close()
        // 与全局默认一致 → nil（调用方清除覆盖，回落默认）。
        onSave(settings == defaults ? nil : settings)
    }

    @objc private func cancel() {
        close()
    }
}
