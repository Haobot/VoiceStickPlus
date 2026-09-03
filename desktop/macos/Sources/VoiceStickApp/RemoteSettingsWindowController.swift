import AppKit

/// 小米遥控器设置窗口（对齐 Windows RemoteSettingsDialog）：Gain (dB) 与
/// Double-click window (ms) 两个编辑框 + 生效时机提示；纯本地配置无 BLE 下发。
/// 保存语义：数字解析失败回落传入默认，gain clamp ±24、double-click clamp 200...600；
/// 与全局默认一致 → onSave(nil)（调用方清除覆盖，回落默认）。
final class RemoteSettingsWindowController: NSWindowController, NSWindowDelegate {
    private let deviceID: String
    private var settings: XiaomiSettings
    private let defaults: XiaomiSettings
    private let onSave: (XiaomiSettings?) -> Void

    private let gainField = NSTextField()
    private let doubleClickField = NSTextField()

    init(deviceID: String, settings: XiaomiSettings, defaults: XiaomiSettings,
         onSave: @escaping (XiaomiSettings?) -> Void) {
        self.deviceID = deviceID
        self.settings = settings
        self.defaults = defaults
        self.onSave = onSave

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 190),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.remoteSettingsTitle, deviceID)
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
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

        // 分组标题复用菜单文案（对齐 Windows）。
        let sectionTitle = NSTextField(labelWithString: tr(.remoteSettingsSection))
        sectionTitle.font = NSFont.boldSystemFont(ofSize: NSFont.systemFontSize)
        stack.addArrangedSubview(sectionTitle)

        stack.addArrangedSubview(fieldRow(tr(.remoteGainDb) + ":", field: gainField))
        stack.addArrangedSubview(
            fieldRow(tr(.remoteDoubleClickMs) + ":", field: doubleClickField)
        )

        // 生效时机提示（设置由 XiaomiAtvvSession.Options 在会话创建时消费，
        // 热更不重配已连接会话）。
        let hint = NSTextField(wrappingLabelWithString: tr(.remoteEffectiveNextConnect))
        hint.textColor = .secondaryLabelColor
        stack.addArrangedSubview(hint)

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
        label.widthAnchor.constraint(equalToConstant: 240).isActive = true
        field.widthAnchor.constraint(equalToConstant: 120).isActive = true
        row.addArrangedSubview(label)
        row.addArrangedSubview(field)
        return row
    }

    /// 对齐 Windows FormatGain：%g 格式（12.0 显示为 "12"）。
    private static func formatGain(_ gainDb: Double) -> String {
        String(format: "%g", gainDb)
    }

    private func loadSettingsIntoControls() {
        gainField.stringValue = Self.formatGain(settings.gainDb)
        doubleClickField.stringValue = "\(settings.doubleClickMs)"
    }

    @objc private func restoreDefaults() {
        // 只回显不落盘（对齐 Windows RestoreDefaults：Save 才持久化）。
        settings = defaults
        loadSettingsIntoControls()
    }

    @objc private func saveSettings() {
        var edited = settings
        // 数字字段：解析失败时回落默认值，越界 clamp 到消费侧允许范围。
        if let gain = Double(gainField.stringValue) {
            edited.gainDb = min(24.0, max(-24.0, gain))
        } else {
            edited.gainDb = defaults.gainDb
        }
        if let doubleClickMs = Int(doubleClickField.stringValue) {
            edited.doubleClickMs = min(600, max(200, doubleClickMs))
        } else {
            edited.doubleClickMs = defaults.doubleClickMs
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
