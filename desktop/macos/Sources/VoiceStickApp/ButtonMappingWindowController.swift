import AppKit
import VoiceStickCore

/// 按键映射窗口（小米遥控器按键自定义，对齐 Windows kMenuXiaomiKeymap）：
/// 拦截总开关 checkbox（即时生效，经 onInterceptToggled 由调用方落盘并同步拦截层）
/// + 12 键动作行（原生/禁用/按键 + KeySpec 文本 + 捕获按钮）+ 恢复默认/保存。
/// 保存语义（对齐 RemoteSettingsWindowController）：解析失败弹提示不写入；
/// 与全局默认一致 → onSave(nil)（调用方清除覆盖回落默认）。
/// 文案未进 Localization 共享表（新增键较多，表完整性断言会强制双端补齐），
/// 暂在本文件内按生效语言取词；后续需要时下沉 L10nKey。
private func l10n(_ zh: String, _ en: String) -> String {
    Localization.current.effective == .zhHans ? zh : en
}
final class ButtonMappingWindowController: NSWindowController {
    private let deviceID: String
    private var settings: ButtonsSettings
    private let defaults: ButtonsSettings
    private let onSave: (ButtonsSettings?) -> Void
    var onInterceptToggled: ((Bool) -> Void)?

    private var popups: [RemoteButton: NSPopUpButton] = [:]
    private var keyFields: [RemoteButton: NSTextField] = [:]
    private let interceptCheck = NSButton(
        checkboxWithTitle: l10n("启用按键拦截（HID 按键自定义生效前提）", "Enable key interception (required for HID key customization)"),
        target: nil,
        action: nil
    )
    private var hotkeyCapture: HotkeyCaptureWindowController?
    private var captureButtons: [RemoteButton: NSButton] = [:]

    init(deviceID: String, settings: ButtonsSettings, defaults: ButtonsSettings,
         onSave: @escaping (ButtonsSettings?) -> Void) {
        self.deviceID = deviceID
        self.settings = settings
        self.defaults = defaults
        self.onSave = onSave

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 560, height: 520),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = l10n("按键映射 — \(deviceID)", "Button Mapping — \(deviceID)")
        window.isReleasedWhenClosed = false
        super.init(window: window)
        buildContent()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        showWindow(nil)
        window?.center()
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: - UI

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let outer = NSStackView()
        outer.orientation = .vertical
        outer.alignment = .leading
        outer.spacing = 10
        outer.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(outer)

        NSLayoutConstraint.activate([
            outer.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 16),
            outer.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 16),
            outer.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -16),
            outer.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -12),
        ])

        interceptCheck.state = settings.intercept ? .on : .off
        interceptCheck.target = self
        interceptCheck.action = #selector(interceptToggled)
        outer.addArrangedSubview(interceptCheck)

        let hint = NSTextField(wrappingLabelWithString: l10n(
            "「按键」动作支持修饰键组合（如 ctrl+shift+v）；「禁用」吸掉该键；未配置的键保持系统原生行为。语音键双击为 A 级（不经 HID 拦截，拦截关闭时仍生效）。",
            "\"Key\" supports modifier combos (e.g. ctrl+shift+v); \"Disabled\" swallows the key; unmapped keys keep native behavior. Voice double-click is A-level (ATVV layer, effective even when interception is off)."
        ))
        hint.textColor = .secondaryLabelColor
        hint.preferredMaxLayoutWidth = 520
        outer.addArrangedSubview(hint)

        // 键行区放滚动视图（12 行超出一屏）。
        let rows = NSStackView()
        rows.orientation = .vertical
        rows.alignment = .leading
        rows.spacing = 4
        for button in RemoteButton.allCases {
            rows.addArrangedSubview(makeRow(button))
        }
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .noBorder
        scroll.documentView = rows
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(equalToConstant: 300).isActive = true
        outer.addArrangedSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: outer.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: outer.trailingAnchor),
        ])
        // documentView 行宽跟随滚动区。
        if let documentView = scroll.documentView {
            documentView.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor).isActive = true
        }

        let buttonRow = NSStackView()
        buttonRow.orientation = .horizontal
        buttonRow.spacing = 8
        let restoreButton = NSButton(
            title: tr(.restoreDefaults), target: self, action: #selector(restoreDefaults)
        )
        let saveButton = NSButton(title: tr(.save), target: self, action: #selector(saveSettings))
        saveButton.bezelStyle = .rounded
        saveButton.keyEquivalent = "\r"
        buttonRow.addArrangedSubview(restoreButton)
        buttonRow.addArrangedSubview(saveButton)
        outer.addArrangedSubview(buttonRow)
    }

    private func makeRow(_ button: RemoteButton) -> NSView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        row.widthAnchor.constraint(equalToConstant: 520).isActive = true

        let label = NSTextField(labelWithString: Self.buttonName(button))
        label.widthAnchor.constraint(equalToConstant: 96).isActive = true
        row.addArrangedSubview(label)

        let popup = NSPopUpButton()
        popup.addItems(withTitles: [
            l10n("原生", "Native"),
            l10n("禁用", "Disabled"),
            l10n("按键", "Key"),
        ])
        popup.widthAnchor.constraint(equalToConstant: 92).isActive = true
        let mapping = settings.mapping(for: button)
        switch mapping.action {
        case .native: popup.selectItem(at: 0)
        case .disabled: popup.selectItem(at: 1)
        case .key: popup.selectItem(at: 2)
        }
        popup.target = self
        popup.action = #selector(actionChanged(_:))
        popups[button] = popup
        row.addArrangedSubview(popup)

        let field = NSTextField()
        field.placeholderString = "ctrl+shift+v"
        field.widthAnchor.constraint(equalToConstant: 180).isActive = true
        field.stringValue = mapping.key
        keyFields[button] = field
        row.addArrangedSubview(field)

        let capture = NSButton(
            title: l10n("捕获…", "Capture…"), target: self, action: #selector(captureKey(_:))
        )
        capture.bezelStyle = .rounded
        captureButtons[button] = capture
        row.addArrangedSubview(capture)

        return row
    }

    // MARK: - 状态同步

    @objc private func interceptToggled() {
        let enabled = interceptCheck.state == .on
        settings.intercept = enabled
        onInterceptToggled?(enabled)
    }

    @objc private func actionChanged(_ sender: NSPopUpButton) {
        guard let button = popups.first(where: { $0.value === sender })?.key else { return }
        // 切回「按键」时若字段为空，聚焦字段提示补录（保存时仍会校验）。
        if sender.indexOfSelectedItem == 2, keyFields[button]?.stringValue.isEmpty == true {
            window?.makeFirstResponder(keyFields[button])
        }
    }

    @objc private func captureKey(_ sender: NSButton) {
        guard let button = captureButtons.first(where: { $0.value === sender })?.key else { return }
        let controller = HotkeyCaptureWindowController()
        controller.onCapture = { [weak self] spec in
            self?.keyFields[button]?.stringValue = spec
            self?.hotkeyCapture = nil
        }
        hotkeyCapture = controller
        controller.show()
    }

    // MARK: - 保存

    @objc private func restoreDefaults() {
        onSave(nil)
        window?.close()
    }

    @objc private func saveSettings() {
        // KeySpec 校验前置（对齐 Windows 按键映射对话框：非法弹提示且不写入）。
        for button in RemoteButton.allCases {
            guard popups[button]?.indexOfSelectedItem == 2,
                  let field = keyFields[button] else { continue }
            let text = field.stringValue.trimmingCharacters(in: .whitespaces)
            guard !text.isEmpty, KeySpec.parse(text) != nil else {
                let alert = NSAlert()
                alert.alertStyle = .warning
                alert.messageText = l10n("按键映射无效", "Invalid key mapping")
                alert.informativeText = l10n(
                    "「\(Self.buttonName(button))」的目标按键 \"\(field.stringValue)\" 无法识别。",
                    "The target key \"\(field.stringValue)\" for \"\(Self.buttonName(button))\" could not be parsed."
                )
                alert.runModal()
                return
            }
            field.stringValue = text
        }

        var saved = settings
        saved.mappings = [:]
        for button in RemoteButton.allCases {
            switch popups[button]?.indexOfSelectedItem ?? 0 {
            case 1:
                saved.setMapping(ButtonMapping(action: .disabled, key: ""), for: button)
            case 2:
                let key = keyFields[button]?.stringValue ?? ""
                saved.setMapping(ButtonMapping(action: .key, key: key), for: button)
            default:
                break  // 原生：不落盘
            }
        }
        if saved == defaults {
            onSave(nil)
        } else {
            onSave(saved)
        }
        window?.close()
    }

    // MARK: - 文案

    /// 键显示名（未进 Localization 共享表，按生效语言取词，见类注释）。
    private static func buttonName(_ button: RemoteButton) -> String {
        let zh = Localization.current.effective == .zhHans
        switch button {
        case .ok: return "OK"
        case .home: return zh ? "主页" : "Home"
        case .back: return zh ? "返回" : "Back"
        case .menu: return zh ? "菜单" : "Menu"
        case .tv: return zh ? "TV" : "TV"
        case .right: return zh ? "右方向" : "Right"
        case .left: return zh ? "左方向" : "Left"
        case .down: return zh ? "下方向" : "Down"
        case .up: return zh ? "上方向" : "Up"
        case .volUp: return zh ? "音量+" : "Volume Up"
        case .volDown: return zh ? "音量-" : "Volume Down"
        case .voiceDoubleClick: return zh ? "语音键双击" : "Voice Double-Click"
        }
    }

    
}
