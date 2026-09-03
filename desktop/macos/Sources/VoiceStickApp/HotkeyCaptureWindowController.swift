import AppKit

/// 自定义热键捕获窗口（对齐 Windows HotkeySettingsDialog）：按下修饰键+主键录入，
/// Esc 取消，无修饰键时提示，确定按钮在捕获到有效组合前禁用。
final class HotkeyCaptureWindowController: NSWindowController {
    var onCapture: ((String) -> Void)?

    private let valueField = NSTextField(labelWithString: "")
    private let hintLabel = NSTextField(labelWithString: "")
    private let okButton = NSButton(title: "", target: nil, action: nil)
    private var capturedSpec: String?
    private var eventMonitor: Any?

    init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 360, height: 180),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = tr(.hotkeyCaptureTitle)
        super.init(window: window)
        hintLabel.stringValue = tr(.hotkeyCaptureHint)
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
        // Esc 取消；其余键尝试捕获为热键组合。
        if event.keyCode == 53 {
            cancel()
            return nil
        }
        guard let spec = GlobalHotkeyManager.spec(for: event) else {
            hintLabel.stringValue = tr(.hotkeyCaptureMissingModifier)
            return nil
        }
        capturedSpec = spec
        valueField.stringValue = GlobalHotkeyManager.displayName(for: spec)
        hintLabel.stringValue = tr(.hotkeyCaptureApplyHint)
        okButton.isEnabled = true
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
