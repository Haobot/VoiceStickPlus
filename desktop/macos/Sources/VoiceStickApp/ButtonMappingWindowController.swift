import AppKit
import VoiceStickCore

/// 按键映射窗口（对齐 Windows XiaomiKeymapDialog 的图片热区设计，2026-10-12 恢复）：
/// 左侧遥控器照片（悬停浅蓝框/选中蓝框 + 手型光标，点击热区选键）+ 右侧选中键的
/// 映射编辑（原生/禁用/按键 + KeySpec 文本 + 捕获）+ 拦截总开关（直连 RC 模式）
/// + 恢复默认/保存。
/// 保存语义（对齐 RemoteSettingsWindowController）：解析失败弹提示不写入；
/// 与 defaults 一致 → onSave(nil)（调用方清覆盖/恢复默认）。
/// 布局为手写 frame（不参与 Auto Layout）——规避 2026-10-12 修复过的
/// 「宽度欠定把窗口塌到 40pt」问题（见 git 32be9fe）。
/// 文案未进 Localization 共享表（同旧版），按生效语言在本文件内取词。
private func l10n(_ zh: String, _ en: String) -> String {
    Localization.current.effective == .zhHans ? zh : en
}

/// 遥控器照片热区表（与 Windows XiaomiKeymapDialog 同源：原始图片坐标系 228×889，
/// oval=椭圆框高亮，rect=矩形框；命中检测按矩形包含，ok 与方向键边界相接处优先 ok）。
private struct RemoteHotspot {
    let id: String
    let x1, y1, x2, y2: Int
    let oval: Bool
}

private let kRemoteImageWidth = 228
private let kRemoteImageHeight = 889
private let kRemoteHotspots: [RemoteHotspot] = [
    RemoteHotspot(id: "power", x1: 36, y1: 18, x2: 114, y2: 96, oval: true),
    RemoteHotspot(id: "mic", x1: 116, y1: 18, x2: 192, y2: 96, oval: true),
    RemoteHotspot(id: "up", x1: 67, y1: 102, x2: 157, y2: 152, oval: false),
    RemoteHotspot(id: "left", x1: 18, y1: 152, x2: 67, y2: 242, oval: false),
    RemoteHotspot(id: "ok", x1: 67, y1: 152, x2: 157, y2: 242, oval: true),
    RemoteHotspot(id: "right", x1: 157, y1: 152, x2: 206, y2: 242, oval: false),
    RemoteHotspot(id: "down", x1: 67, y1: 242, x2: 157, y2: 292, oval: false),
    RemoteHotspot(id: "back", x1: 18, y1: 303, x2: 99, y2: 382, oval: true),
    RemoteHotspot(id: "volume_up", x1: 125, y1: 300, x2: 208, y2: 393, oval: false),
    RemoteHotspot(id: "home", x1: 18, y1: 385, x2: 99, y2: 467, oval: true),
    RemoteHotspot(id: "volume_down", x1: 125, y1: 393, x2: 208, y2: 488, oval: false),
    RemoteHotspot(id: "menu", x1: 18, y1: 470, x2: 99, y2: 550, oval: true),
    RemoteHotspot(id: "tv", x1: 125, y1: 470, x2: 208, y2: 550, oval: true),
]
private let kRemoteOkHotspotIndex = 4

/// 照片画布：图片等比缩放到可用高度、水平居中；热区悬停/选中描边 + 手型光标；
/// 底部提示条。全部手写绘制，无 Auto Layout 参与。
private final class RemoteCanvasView: NSView {
    var image: NSImage?
    /// 选中热区下标（初值由控制器设置）。
    var selected: Int = kRemoteOkHotspotIndex
    var hover: Int = -1
    var onSelect: ((Int) -> Void)?

    /// 图片显示区（非翻转坐标系，原点在左下）：可用高 = 视图高 - 提示条 - 上下留白。
    private func imageFrame() -> NSRect? {
        guard image != nil else { return nil }
        let margin: CGFloat = 6
        let hintHeight: CGFloat = 24
        let availableHeight = bounds.height - hintHeight - margin * 2
        guard availableHeight > 0 else { return nil }
        let scale = availableHeight / CGFloat(kRemoteImageHeight)
        let width = CGFloat(kRemoteImageWidth) * scale
        return NSRect(
            x: (bounds.width - width) / 2,
            y: margin,
            width: width,
            height: CGFloat(kRemoteImageHeight) * scale
        )
    }

    /// 图片坐标（原点左上）→ 热区命中；未命中返回 -1。
    private func hotspotIndex(at point: NSPoint) -> Int {
        guard let frame = imageFrame() else { return -1 }
        let scale = frame.width / CGFloat(kRemoteImageWidth)
        let ix = (point.x - frame.minX) / scale
        let iy = (frame.maxY - point.y) / scale
        guard ix >= 0, ix < CGFloat(kRemoteImageWidth),
              iy >= 0, iy < CGFloat(kRemoteImageHeight) else {
            return -1
        }
        func contains(_ hs: RemoteHotspot) -> Bool {
            ix >= CGFloat(hs.x1) && ix <= CGFloat(hs.x2)
                && iy >= CGFloat(hs.y1) && iy <= CGFloat(hs.y2)
        }
        // ok 优先：ok 与方向键热区边界相接，重叠命中时取 ok（与 Windows 一致）。
        if contains(kRemoteHotspots[kRemoteOkHotspotIndex]) {
            return kRemoteOkHotspotIndex
        }
        for index in kRemoteHotspots.indices where index != kRemoteOkHotspotIndex {
            if contains(kRemoteHotspots[index]) { return index }
        }
        return -1
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.white.setFill()
        bounds.fill()
        guard let frame = imageFrame(), let image else {
            let placeholder = l10n("遥控器照片缺失", "Remote photo missing")
            let attrs: [NSAttributedString.Key: Any] = [
                .foregroundColor: NSColor.systemGray,
                .font: NSFont.systemFont(ofSize: 13),
            ]
            let size = (placeholder as NSString).size(withAttributes: attrs)
            (placeholder as NSString).draw(
                at: NSPoint(x: (bounds.width - size.width) / 2, y: bounds.midY - size.height / 2),
                withAttributes: attrs
            )
            drawHintStrip()
            return
        }
        image.draw(in: frame, from: .zero, operation: .sourceOver, fraction: 1)
        if hover >= 0, hover != selected {
            drawOutline(hover, selected: false, in: frame)
        }
        if selected >= 0, selected < kRemoteHotspots.count {
            drawOutline(selected, selected: true, in: frame)
        }
        drawHintStrip()
    }

    private func drawOutline(_ index: Int, selected: Bool, in frame: NSRect) {
        let hs = kRemoteHotspots[index]
        let scale = frame.width / CGFloat(kRemoteImageWidth)
        let rect = NSRect(
            x: frame.minX + CGFloat(hs.x1) * scale,
            y: frame.maxY - CGFloat(hs.y2) * scale,
            width: CGFloat(hs.x2 - hs.x1) * scale,
            height: CGFloat(hs.y2 - hs.y1) * scale
        )
        // 选中 3px 蓝（RGB 22/119/255），悬停 2px 浅蓝——与 Windows 对话框同色。
        let color = selected
            ? NSColor(srgbRed: 22.0 / 255.0, green: 119.0 / 255.0, blue: 1.0, alpha: 1.0)
            : NSColor(srgbRed: 145.0 / 255.0, green: 200.0 / 255.0, blue: 1.0, alpha: 1.0)
        color.setStroke()
        let path = hs.oval ? NSBezierPath(ovalIn: rect) : NSBezierPath(rect: rect)
        path.lineWidth = selected ? 3 : 2
        path.stroke()
    }

    private func drawHintStrip() {
        let hint = l10n("点击图片中的按钮进行配置", "Click a button in the photo to configure")
        let attrs: [NSAttributedString.Key: Any] = [
            .foregroundColor: NSColor.systemGray,
            .font: NSFont.systemFont(ofSize: 11),
        ]
        let size = (hint as NSString).size(withAttributes: attrs)
        (hint as NSString).draw(
            at: NSPoint(x: (bounds.width - size.width) / 2, y: 5),
            withAttributes: attrs
        )
    }

    override func mouseDown(with event: NSEvent) {
        let index = hotspotIndex(at: convert(event.locationInWindow, from: nil))
        guard index >= 0 else { return }
        selected = index
        hover = index
        needsDisplay = true
        onSelect?(index)
    }

    override func mouseMoved(with event: NSEvent) {
        let index = hotspotIndex(at: convert(event.locationInWindow, from: nil))
        if index != hover {
            hover = index
            needsDisplay = true
        }
        (index >= 0 ? NSCursor.pointingHand : NSCursor.arrow).set()
    }

    override func mouseExited(with event: NSEvent) {
        if hover != -1 {
            hover = -1
            needsDisplay = true
        }
        NSCursor.arrow.set()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for area in trackingAreas { removeTrackingArea(area) }
        addTrackingArea(NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
            owner: self,
            userInfo: nil
        ))
    }
}

final class ButtonMappingWindowController: NSWindowController, NSTextFieldDelegate {
    private let deviceID: String
    private var settings: ButtonsSettings
    private let defaults: ButtonsSettings
    private let onSave: (ButtonsSettings?) -> Void
    var onInterceptToggled: ((Bool) -> Void)?

    /// hidesIntercept：网关模式入口专用——拦截是直连 RC 的 HID 能力，网关链路
    /// （固件路由→gateway_key 事件）不消费该开关，隐藏避免误导。
    private let hidesIntercept: Bool

    private let canvas = RemoteCanvasView()
    private let keyNameLabel = NSTextField(labelWithString: "")
    private let actionPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let keyField = NSTextField(frame: .zero)
    private let hintLabel = NSTextField(wrappingLabelWithString: "")
    private let interceptCheck = NSButton(
        checkboxWithTitle: l10n("启用按键拦截（HID 按键自定义生效前提）", "Enable key interception (required for HID key customization)"),
        target: nil,
        action: nil
    )
    private let captureButton = NSButton(
        title: l10n("捕获…", "Capture…"), target: nil, action: nil
    )
    // 长按连发调节区（方向/返回删除/音量类注入键的全局节拍；网关模式隐藏）。
    private let repeatTitle = NSTextField(labelWithString: "")
    private let repeatDelayLabel = NSTextField(labelWithString: "")
    private let repeatDelaySlider = NSSlider(value: 0, minValue: 0, maxValue: 0, target: nil, action: nil)
    private let repeatDelayValue = NSTextField(labelWithString: "")
    private let repeatIntervalLabel = NSTextField(labelWithString: "")
    private let repeatIntervalSlider = NSSlider(value: 0, minValue: 0, maxValue: 0, target: nil, action: nil)
    private let repeatIntervalValue = NSTextField(labelWithString: "")
    private let restoreButton = NSButton(
        title: tr(.restoreDefaults), target: nil, action: nil
    )
    private let saveButton = NSButton(title: tr(.save), target: nil, action: nil)
    private var hotkeyCapture: HotkeyCaptureWindowController?

    /// 照片资源（缺失时降级占位不崩溃）。双形态分发：
    /// - 发布 .app：build-macos.sh 拷入 Contents/Resources，走 Bundle.main；
    /// - SwiftPM 裸构建（swift build/run）：资源随可执行文件旁的 `<Package>_<Target>.bundle`
    ///   分发，Bundle.main 查不到，需在可执行文件目录泛化扫 .bundle。
    /// 不用 Bundle.module：其 accessor 找不到 bundle 时 fatalError，会崩掉发布 .app。
    private static let remoteImage: NSImage? = {
        guard let path = remotePhotoPath() else {
            NSLog("ButtonMapping: remote photo missing in bundle \(Bundle.main.bundlePath)")
            return nil
        }
        return NSImage(contentsOfFile: path)
    }()

    private static func remotePhotoPath() -> String? {
        if let path = Bundle.main.path(forResource: "xiaomi_remote_2pro", ofType: "png") {
            return path
        }
        var bases = [Bundle.main.bundleURL]
        if let resourceURL = Bundle.main.resourceURL { bases.append(resourceURL) }
        for base in bases {
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: base.path)
            else { continue }
            for name in names where name.hasSuffix(".bundle") {
                if let bundle = Bundle(path: base.appendingPathComponent(name).path),
                   let path = bundle.path(forResource: "xiaomi_remote_2pro", ofType: "png") {
                    return path
                }
            }
        }
        return nil
    }

    init(deviceID: String, settings: ButtonsSettings, defaults: ButtonsSettings,
         hidesIntercept: Bool = false,
         onSave: @escaping (ButtonsSettings?) -> Void) {
        self.deviceID = deviceID
        self.settings = settings
        self.defaults = defaults
        self.hidesIntercept = hidesIntercept
        self.onSave = onSave

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 640, height: 640),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = l10n("按键映射 — \(deviceID)", "Button Mapping — \(deviceID)")
        window.isReleasedWhenClosed = false
        // 热区悬停高亮依赖鼠标移动事件。
        window.acceptsMouseMovedEvents = true
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

        // ===== 左侧：遥控器照片画布（手写 frame，宽度不参与约束求解） =====
        canvas.frame = NSRect(x: 10, y: 54, width: 170, height: 576)
        canvas.image = Self.remoteImage
        // 默认选中「上方向」（power 不可映射，不宜做首屏选中）。
        canvas.selected = 2
        canvas.onSelect = { [weak self] _ in self?.refreshPanel() }
        contentView.addSubview(canvas)

        // ===== 右侧：选中键信息与映射编辑 =====
        keyNameLabel.font = NSFont.boldSystemFont(ofSize: 18)
        keyNameLabel.frame = NSRect(x: 196, y: 598, width: 430, height: 30)
        contentView.addSubview(keyNameLabel)

        actionPopup.frame = NSRect(x: 196, y: 562, width: 200, height: 26)
        actionPopup.addItems(withTitles: [
            l10n("原生", "Native"), l10n("禁用", "Disabled"), l10n("按键", "Key"),
        ])
        actionPopup.target = self
        actionPopup.action = #selector(actionChanged)
        contentView.addSubview(actionPopup)

        keyField.frame = NSRect(x: 196, y: 524, width: 300, height: 26)
        keyField.placeholderString = "ctrl+shift+v"
        keyField.delegate = self
        contentView.addSubview(keyField)

        captureButton.bezelStyle = .rounded
        captureButton.target = self
        captureButton.action = #selector(captureKey)
        captureButton.frame = NSRect(x: 504, y: 523, width: 122, height: 28)
        contentView.addSubview(captureButton)

        hintLabel.textColor = .secondaryLabelColor
        hintLabel.frame = NSRect(x: 196, y: 424, width: 430, height: 96)
        contentView.addSubview(hintLabel)

        interceptCheck.state = settings.intercept ? .on : .off
        interceptCheck.target = self
        interceptCheck.action = #selector(interceptToggled)
        interceptCheck.isHidden = hidesIntercept
        interceptCheck.frame = NSRect(x: 196, y: 392, width: 430, height: 20)
        contentView.addSubview(interceptCheck)

        // ===== 长按连发区（全局节拍，非单键属性；直连 HID inject 与网关 key 沿共用）=====
        repeatTitle.stringValue = l10n(
            "长按连发 — 按住映射键自动重复（语音键不适用）",
            "Hold-to-Repeat — held mapped keys auto-repeat (voice key excluded)"
        )
        repeatTitle.font = NSFont.boldSystemFont(ofSize: 12)
        repeatTitle.frame = NSRect(x: 196, y: 360, width: 430, height: 18)
        contentView.addSubview(repeatTitle)

        repeatDelayLabel.stringValue = l10n("首次延迟", "Initial delay")
        repeatDelayLabel.font = NSFont.systemFont(ofSize: 12)
        repeatDelayLabel.frame = NSRect(x: 196, y: 330, width: 64, height: 18)
        contentView.addSubview(repeatDelayLabel)
        repeatDelaySlider.minValue = Double(ButtonsSettings.repeatDelayRange.lowerBound)
        repeatDelaySlider.maxValue = Double(ButtonsSettings.repeatDelayRange.upperBound)
        repeatDelaySlider.intValue = Int32(settings.repeatDelayMs)
        repeatDelaySlider.target = self
        repeatDelaySlider.action = #selector(repeatDelayChanged)
        repeatDelaySlider.frame = NSRect(x: 264, y: 328, width: 236, height: 22)
        contentView.addSubview(repeatDelaySlider)
        repeatDelayValue.font = NSFont.systemFont(ofSize: 12)
        repeatDelayValue.textColor = .secondaryLabelColor
        repeatDelayValue.frame = NSRect(x: 508, y: 330, width: 118, height: 18)
        contentView.addSubview(repeatDelayValue)

        repeatIntervalLabel.stringValue = l10n("重复间隔", "Repeat interval")
        repeatIntervalLabel.font = NSFont.systemFont(ofSize: 12)
        repeatIntervalLabel.frame = NSRect(x: 196, y: 302, width: 64, height: 18)
        contentView.addSubview(repeatIntervalLabel)
        repeatIntervalSlider.minValue = Double(ButtonsSettings.repeatIntervalRange.lowerBound)
        repeatIntervalSlider.maxValue = Double(ButtonsSettings.repeatIntervalRange.upperBound)
        repeatIntervalSlider.intValue = Int32(settings.repeatIntervalMs)
        repeatIntervalSlider.target = self
        repeatIntervalSlider.action = #selector(repeatIntervalChanged)
        repeatIntervalSlider.frame = NSRect(x: 264, y: 300, width: 236, height: 22)
        contentView.addSubview(repeatIntervalSlider)
        repeatIntervalValue.font = NSFont.systemFont(ofSize: 12)
        repeatIntervalValue.textColor = .secondaryLabelColor
        repeatIntervalValue.frame = NSRect(x: 508, y: 302, width: 118, height: 18)
        contentView.addSubview(repeatIntervalValue)
        refreshRepeatValues()

        // ===== 底部：恢复默认 / 保存 =====
        restoreButton.target = self
        restoreButton.action = #selector(restoreDefaults)
        restoreButton.frame = NSRect(x: 196, y: 12, width: 130, height: 32)
        contentView.addSubview(restoreButton)

        saveButton.target = self
        saveButton.action = #selector(saveSettings)
        saveButton.bezelStyle = .rounded
        saveButton.keyEquivalent = "\r"
        saveButton.frame = NSRect(x: 500, y: 12, width: 126, height: 32)
        contentView.addSubview(saveButton)

        refreshPanel()
    }

    // MARK: - 状态同步

    /// 当前选中热区对应的可映射键；mic → 语音键双击（照片上的语音键在桌面侧的
    /// 映射面即 A 级双击动作）。power 自 2026-10-19 起可映射（对齐 Windows 12 键
    /// 全集），走常规 B 级路径（需 intercept 开启）。
    private func currentButton() -> RemoteButton? {
        guard canvas.selected >= 0, canvas.selected < kRemoteHotspots.count else { return nil }
        let id = kRemoteHotspots[canvas.selected].id
        if id == "mic" { return .voiceDoubleClick }
        return RemoteButton(rawValue: id)
    }

    private func refreshPanel() {
        guard canvas.selected >= 0, canvas.selected < kRemoteHotspots.count else { return }
        guard let button = currentButton() else { return }
        keyNameLabel.stringValue = Self.buttonName(button)
        if button == .power {
            hintLabel.stringValue = Self.powerHint
        } else {
            hintLabel.stringValue = Self.normalHint
        }
        actionPopup.isEnabled = true
        captureButton.isEnabled = true

        let mapping = settings.mapping(for: button)
        actionPopup.selectItem(at: mapping.action == .native ? 0 : (mapping.action == .disabled ? 1 : 2))
        keyField.stringValue = mapping.action == .key ? mapping.key : ""
        keyField.isEnabled = mapping.action == .key
    }

    @objc private func interceptToggled() {
        settings.intercept = interceptCheck.state == .on
        onInterceptToggled?(settings.intercept)
    }

    /// 长按连发节拍滑块（连续拖动即时生效；数值就近取整，保存随整体 settings 落盘）。
    @objc private func repeatDelayChanged() {
        settings.repeatDelayMs = Int(repeatDelaySlider.doubleValue.rounded())
        refreshRepeatValues()
    }

    @objc private func repeatIntervalChanged() {
        settings.repeatIntervalMs = Int(repeatIntervalSlider.doubleValue.rounded())
        refreshRepeatValues()
    }

    private func refreshRepeatValues() {
        repeatDelayValue.stringValue = "\(settings.repeatDelayMs) ms"
        repeatIntervalValue.stringValue = "\(settings.repeatIntervalMs) ms"
    }

    @objc private func actionChanged() {
        guard let button = currentButton() else { return }
        switch actionPopup.indexOfSelectedItem {
        case 0:
            settings.mappings.removeValue(forKey: button.rawValue)
        case 1:
            settings.setMapping(ButtonMapping(action: .disabled, key: ""), for: button)
        case 2:
            settings.setMapping(
                ButtonMapping(action: .key, key: keyField.stringValue), for: button
            )
        default:
            break
        }
        keyField.isEnabled = actionPopup.indexOfSelectedItem == 2
        if actionPopup.indexOfSelectedItem == 2, keyField.stringValue.isEmpty {
            window?.makeFirstResponder(keyField)
        }
    }

    /// KeySpec 文本即时提交（与旧列表版一致：保存时统一校验，输入过程不打断）。
    func controlTextDidChange(_ obj: Notification) {
        guard (obj.object as? NSTextField) === keyField,
              let button = currentButton(),
              actionPopup.indexOfSelectedItem == 2 else {
            return
        }
        settings.setMapping(
            ButtonMapping(action: .key, key: keyField.stringValue), for: button
        )
    }

    @objc private func captureKey() {
        guard let button = currentButton() else { return }
        // 按键映射场景：单键（Backspace 等）也允许，不要求修饰键
        //（对齐 Windows xiaomi_keymap_dialog 的 require_modifier=false）。
        let controller = HotkeyCaptureWindowController(requireModifier: false)
        controller.onCapture = { [weak self] spec in
            guard let self else { return }
            self.keyField.stringValue = spec
            if self.actionPopup.indexOfSelectedItem != 2 {
                self.actionPopup.selectItem(at: 2)
            }
            self.keyField.isEnabled = true
            self.settings.setMapping(ButtonMapping(action: .key, key: spec), for: button)
            self.hotkeyCapture = nil
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
        // KeySpec 校验前置（对齐旧列表版与 Windows 对话框：非法弹提示且不写入），
        // 通过者 trim 归一后落盘。
        var saved = settings
        for (raw, mapping) in saved.mappings where mapping.action == .key {
            guard let button = RemoteButton(rawValue: raw) else { continue }
            let trimmedKey = mapping.key.trimmingCharacters(in: .whitespaces)
            guard !trimmedKey.isEmpty, KeySpec.parse(trimmedKey) != nil else {
                let alert = NSAlert()
                alert.alertStyle = .warning
                alert.messageText = l10n("按键映射无效", "Invalid key mapping")
                alert.informativeText = l10n(
                    "「\(Self.buttonName(button))」的目标按键 \"\(mapping.key)\" 无法识别。",
                    "The target key \"\(mapping.key)\" for \"\(Self.buttonName(button))\" could not be parsed."
                )
                alert.runModal()
                return
            }
            saved.setMapping(ButtonMapping(action: .key, key: trimmedKey), for: button)
        }
        if saved == defaults {
            onSave(nil)
        } else {
            onSave(saved)
        }
        window?.close()
    }

    // MARK: - 文案

    /// 说明文案（选中可映射键时显示）。
    private static var normalHint: String {
        l10n(
            "「按键」动作支持修饰键组合（如 ctrl+shift+v）；「禁用」吸掉该键；「原生」按系统等价按键注入（方向/回车/音量等）。语音键双击为 A 级（不经 HID 拦截，拦截关闭时仍生效）。",
            "\"Key\" supports modifier combos (e.g. ctrl+shift+v); \"Disabled\" swallows the key; \"Native\" injects the system-equivalent key (arrows/enter/volume...). Voice double-click is A-level (ATVV layer, effective even when interception is off)."
        )
    }

    /// 电源键说明：原生 = 系统电源语义（可能触发睡眠/关机），自定义/禁用后桌面
    /// 拦截原生系统事件（需开启 HID 拦截）；网关模式下原生 = 直通。
    private static var powerHint: String {
        l10n(
            "「原生」保持系统电源键行为；「按键」/「禁用」由桌面接管并吞掉系统电源事件（需开启 HID 拦截，未映射时按电源可能触发睡眠/关机对话框）。",
            "\"Native\" keeps the OS power-button behavior; \"Key\"/\"Disabled\" is handled by the desktop, which swallows the OS power events (requires HID interception; when unmapped, pressing power may trigger the sleep/shutdown dialog)."
        )
    }

    /// 键显示名（未进 Localization 共享表，按生效语言取词，见文件头注释）。
    private static func buttonName(_ button: RemoteButton) -> String {
        let zh = Localization.current.effective == .zhHans
        switch button {
        case .ok: return "OK"
        case .home: return zh ? "主页" : "Home"
        case .back: return zh ? "返回" : "Back"
        case .menu: return zh ? "菜单" : "Menu"
        case .tv: return "TV"
        case .power: return zh ? "电源" : "Power"
        case .right: return zh ? "右方向" : "Right"
        case .left: return zh ? "左方向" : "Left"
        case .down: return zh ? "下方向" : "Down"
        case .up: return zh ? "上方向" : "Up"
        case .volUp: return zh ? "音量+" : "Volume Up"
        case .volDown: return zh ? "音量−" : "Volume Down"
        case .voiceDoubleClick: return zh ? "语音键双击" : "Voice Double-Click"
        }
    }
}
