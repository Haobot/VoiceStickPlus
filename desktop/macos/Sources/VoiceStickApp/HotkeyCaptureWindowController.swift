import AppKit

/// 自定义热键捕获窗口（对齐 Windows HotkeySettingsDialog / shortcut_capture 的
/// require_modifier 场景位）：
/// - requireModifier=true（全局热键场景，Windows hotkey_settings_dialog 同口径）：
///   按下修饰键+主键录入，无修饰键时提示，确定按钮在捕获到有效组合前禁用；
/// - requireModifier=false（按键映射场景，对齐 xiaomi_keymap_dialog
///   `require_modifier = false`「单键也可」）：单键（如 Backspace）直接允许，
///   键名按 KeySpec 口径产出（热键表把 mac keycode 51 叫 DELETE，进 KeySpec.parse
///   会解成 117 前进删除——映射场景必须走自己的换算）。
/// Esc 取消（要绑 Esc 可在映射窗手动输入 esc）。
final class HotkeyCaptureWindowController: NSWindowController {
    var onCapture: ((String) -> Void)?

    private let requireModifier: Bool

    private let valueField = NSTextField(labelWithString: "")
    private let hintLabel = NSTextField(labelWithString: "")
    private let okButton = NSButton(title: "", target: nil, action: nil)
    private var capturedSpec: String?
    private var eventMonitor: Any?

    init(requireModifier: Bool = true) {
        self.requireModifier = requireModifier
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 360, height: 180),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.hotkeyCaptureTitle)
        super.init(window: window)
        hintLabel.stringValue = requireModifier
            ? tr(.hotkeyCaptureHint)
            : tr(.hotkeyCaptureAnyKeyHint)
        buildContent()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    private func buildContent() {
        guard let contentView = window?.contentView else { return }
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false

        hintLabel.textColor = .secondaryLabelColor
        hintLabel.font = .systemFont(ofSize: 12)
        hintLabel.maximumNumberOfLines = 2
        hintLabel.lineBreakMode = .byWordWrapping

        valueField.font = .systemFont(ofSize: 16, weight: .medium)
        valueField.alignment = .center
        valueField.stringValue = "—"

        okButton.title = tr(.ok)
        okButton.target = self
        okButton.action = #selector(confirm)
        okButton.isEnabled = false
        okButton.keyEquivalent = "\r"
        let cancelButton = NSButton(title: tr(.cancel), target: self, action: #selector(cancel))

        let buttonRow = NSStackView(views: [cancelButton, okButton])
        buttonRow.orientation = .horizontal
        buttonRow.spacing = 8

        stack.addArrangedSubview(hintLabel)
        stack.addArrangedSubview(valueField)
        stack.addArrangedSubview(buttonRow)
        buttonRow.alignment = .centerX
        contentView.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor)
        ])
    }

    func show() {
        window?.center()
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        eventMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            self?.handleKeyDown(event) ?? event
        }
    }

    private func handleKeyDown(_ event: NSEvent) -> NSEvent? {
        // Esc 取消；其余键按场景尝试捕获（全局热键要求修饰键，映射场景单键即可）。
        if event.keyCode == 53 {
            cancel()
            return nil
        }
        let spec: String?
        if requireModifier {
            spec = GlobalHotkeyManager.spec(for: event)
            if spec == nil {
                hintLabel.stringValue = tr(.hotkeyCaptureMissingModifier)
                return nil
            }
        } else {
            spec = Self.mappingSpec(for: event)
            if spec == nil {
                // 无法表达为 KeySpec 的键码：保持原提示，等下一个键。
                return nil
            }
        }
        guard let spec else { return nil }
        capturedSpec = spec
        valueField.stringValue = GlobalHotkeyManager.displayName(for: spec)
        hintLabel.stringValue = tr(.hotkeyCaptureApplyHint)
        okButton.isEnabled = true
        return nil
    }

    // MARK: - 按键映射场景（单键允许）

    /// 按键映射场景的绑定串：产出 KeySpec 语法（修饰键可选、单键允许）。
    /// 与 GlobalHotkeyManager.spec 的差异在键名口径——见 keySpecName 注释。
    private static func mappingSpec(for event: NSEvent) -> String? {
        guard let key = keySpecName(for: event) else { return nil }
        var parts: [String] = []
        let flags = event.modifierFlags
        if flags.contains(.control) { parts.append("ctrl") }
        if flags.contains(.option) { parts.append("alt") }
        if flags.contains(.shift) { parts.append("shift") }
        if flags.contains(.command) { parts.append("win") }
        parts.append(key)
        return parts.joined(separator: "+")
    }

    /// mac keycode → KeySpec 主键名（与 ConfigDomain.KeySpec 口径对齐）。
    /// 关键差异：mac 51 = 退格（"backspace"）——热键表名 "DELETE" 进 KeySpec.parse
    /// 会解成 117 前进删除，键位会错；117 前进删除才是 "delete"。
    private static func keySpecName(for event: NSEvent) -> String? {
        switch event.keyCode {
        case 36: return "enter"
        case 48: return "tab"
        case 49: return "space"
        case 51: return "backspace"
        case 114: return "insert"
        case 115: return "home"
        case 116: return "pageup"
        case 117: return "delete"
        case 119: return "end"
        case 121: return "pagedown"
        case 123: return "left"
        case 124: return "right"
        case 125: return "down"
        case 126: return "up"
        case 122: return "f1"
        case 120: return "f2"
        case 99: return "f3"
        case 118: return "f4"
        case 96: return "f5"
        case 97: return "f6"
        case 98: return "f7"
        case 100: return "f8"
        case 101: return "f9"
        case 109: return "f10"
        case 103: return "f11"
        case 111: return "f12"
        default: break
        }
        // 字母/数字取基础字符（Shift 不取字，修饰仍进 spec：Shift+B → "shift+B"）。
        // 限 ASCII：KeySpec 单主键只认 A-Z0-9，其它单字符进去也过不了保存校验。
        if let text = event.charactersIgnoringModifiers, text.count == 1,
           let first = text.uppercased().first, first.isASCII,
           first.isLetter || first.isNumber {
            return String(first)
        }
        return nil
    }

    @objc private func confirm() {
        guard let spec = capturedSpec else { return }
        stopMonitor()
        onCapture?(spec)
        window?.close()
    }

    @objc private func cancel() {
        stopMonitor()
        window?.close()
    }

    private func stopMonitor() {
        if let eventMonitor {
            NSEvent.removeMonitor(eventMonitor)
            self.eventMonitor = nil
        }
    }

    deinit {
        stopMonitor()
    }
}
