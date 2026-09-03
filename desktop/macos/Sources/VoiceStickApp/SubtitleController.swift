import AppKit

/// 字幕条：每设备一条 lane 纵向堆叠，7 秒无更新自动消除。
/// 视觉与布局逐值对齐 Windows subtitle_window.h:83-115 / subtitle_window.cc：
/// 深色毛玻璃 lane + 白色 1px 描边（alpha 32）、左侧主题色条 + 设备标签、
/// 字号 64→44 自适应至 ≤2 行、整体底部居中（底边距 32）。
final class SubtitleController {
    private struct Lane {
        var text: String
        var color: OverlayThemeColor
        var generation: Int
    }

    // 布局常量（对齐 Windows subtitle_window.h）。
    private let windowPadding: CGFloat = 8
    private let laneGap: CGFloat = 10
    private let bottomScreenMargin: CGFloat = 32
    private let textLeftInset: CGFloat = 98
    private let textRightInset: CGFloat = 26
    private let textVerticalInset: CGFloat = 18
    private let colorBarOffset: CGFloat = 12
    private let colorBarWidth: CGFloat = 6
    private let colorBarVerticalInset: CGFloat = 14
    private let colorBarRadius: CGFloat = 3
    private let laneCornerRadius: CGFloat = 14
    private let deviceLabelOffset: CGFloat = 30
    private let deviceLabelWidth: CGFloat = 52
    private let minLaneWidth: CGFloat = 520
    private let minLaneHeight: CGFloat = 92
    private let maxTextFontSize: CGFloat = 64
    private let minTextFontSize: CGFloat = 44
    private let deviceFontSize: CGFloat = 14
    private let maxWindowWidthRatio: CGFloat = 0.86
    private let maxWindowHeightRatio: CGFloat = 0.36
    private let holdSeconds: TimeInterval = 7

    private let window: NSPanel
    private let stack = NSStackView()
    private var lanes: [String: Lane] = [:]
    private var generation = 0

    init() {
        window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 960, height: 180),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.level = .screenSaver
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        window.animationBehavior = .utilityWindow
        window.alphaValue = 0

        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.distribution = .gravityAreas
        stack.spacing = laneGap
        stack.translatesAutoresizingMaskIntoConstraints = false

        let contentView = NSView()
        contentView.wantsLayer = true
        contentView.addSubview(stack)
        window.contentView = contentView
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: windowPadding),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -windowPadding),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: windowPadding),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -windowPadding)
        ])
    }

    func show(text: String, deviceID: String, color: OverlayThemeColor) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        DispatchQueue.main.async {
            self.generation += 1
            let generation = self.generation
            self.lanes[AppConfig.normalizedDeviceID(deviceID)] = Lane(
                text: trimmed,
                color: color,
                generation: generation
            )
            self.render()
            DispatchQueue.main.asyncAfter(deadline: .now() + self.holdSeconds) { [weak self] in
                self?.hideLane(deviceID: deviceID, generation: generation)
            }
        }
    }

    func hideAll() {
        DispatchQueue.main.async {
            self.lanes.removeAll()
            self.render()
        }
    }

    private func hideLane(deviceID: String, generation: Int) {
        let key = AppConfig.normalizedDeviceID(deviceID)
        guard lanes[key]?.generation == generation else { return }
        lanes.removeValue(forKey: key)
        render()
    }

    private func render() {
        stack.arrangedSubviews.forEach {
            stack.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }

        for key in lanes.keys.sorted() {
            guard let lane = lanes[key] else { continue }
            stack.addArrangedSubview(makeLaneView(deviceID: key, lane: lane))
        }

        guard !lanes.isEmpty else {
            hideWindow()
            return
        }
        reposition()
        if !window.isVisible {
            window.orderFrontRegardless()
        }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 1
        }
    }

    private func makeLaneView(deviceID: String, lane: Lane) -> NSView {
        let laneWidth = laneWidth(for: lane.text)
        let textWidth = max(1, laneWidth - textLeftInset - textRightInset)
        let font = fittedFont(for: lane.text, width: textWidth)
        let laneHeight = self.laneHeight(for: lane.text, width: laneWidth, font: font)

        // 深色毛玻璃 lane（对齐 Windows 玻璃 + 白色 1px 描边 alpha 32）。
        let container = NSVisualEffectView()
        container.material = .popover
        container.blendingMode = .behindWindow
        container.state = .active
        container.appearance = NSAppearance(named: .darkAqua)
        container.wantsLayer = true
        container.translatesAutoresizingMaskIntoConstraints = false
        container.layer?.cornerRadius = laneCornerRadius
        container.layer?.cornerCurve = .continuous
        container.layer?.masksToBounds = true
        container.layer?.borderWidth = 1
        container.layer?.borderColor = NSColor.white.withAlphaComponent(32.0 / 255.0).cgColor

        let deviceColor = Self.nsColor(for: lane.color)

        let colorBar = NSView()
        colorBar.wantsLayer = true
        colorBar.translatesAutoresizingMaskIntoConstraints = false
        colorBar.layer?.backgroundColor = deviceColor.cgColor
        colorBar.layer?.cornerRadius = colorBarRadius

        let deviceLabel = NSTextField(labelWithString: deviceID)
        deviceLabel.font = .monospacedSystemFont(ofSize: deviceFontSize, weight: .bold)
        deviceLabel.textColor = deviceColor
        deviceLabel.alignment = .center
        deviceLabel.translatesAutoresizingMaskIntoConstraints = false

        let textLabel = NSTextField(labelWithString: lane.text)
        textLabel.font = font
        textLabel.textColor = .white
        textLabel.alignment = .center
        textLabel.maximumNumberOfLines = 2
        textLabel.lineBreakMode = .byWordWrapping
        textLabel.usesSingleLineMode = false
        textLabel.preferredMaxLayoutWidth = textWidth
        textLabel.translatesAutoresizingMaskIntoConstraints = false
        // 文字阴影：黑色 alpha 88/255、偏移 2（对齐 Windows kTextShadowAlpha 偏移 2dp）。
        textLabel.shadow = {
            let shadow = NSShadow()
            shadow.shadowColor = NSColor.black.withAlphaComponent(88.0 / 255.0)
            shadow.shadowBlurRadius = 0
            shadow.shadowOffset = NSSize(width: 0, height: -2)
            return shadow
        }()

        container.addSubview(colorBar)
        container.addSubview(deviceLabel)
        container.addSubview(textLabel)

        NSLayoutConstraint.activate([
            colorBar.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: colorBarOffset),
            colorBar.topAnchor.constraint(equalTo: container.topAnchor, constant: colorBarVerticalInset),
            colorBar.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -colorBarVerticalInset),
            colorBar.widthAnchor.constraint(equalToConstant: colorBarWidth),

            deviceLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: deviceLabelOffset),
            deviceLabel.centerYAnchor.constraint(equalTo: container.centerYAnchor),
            deviceLabel.widthAnchor.constraint(equalToConstant: deviceLabelWidth),

            textLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: textLeftInset),
            textLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -textRightInset),
            textLabel.centerYAnchor.constraint(equalTo: container.centerYAnchor),

            container.widthAnchor.constraint(equalToConstant: laneWidth),
            container.heightAnchor.constraint(equalToConstant: laneHeight)
        ])

        return container
    }

    private func reposition() {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visibleFrame = screen.visibleFrame
        let width = currentWindowWidth
        let measuredHeight = lanes.values.reduce(CGFloat(0)) { total, lane in
            let textWidth = max(1, width - 2 * windowPadding - textLeftInset - textRightInset)
            let font = fittedFont(for: lane.text, width: textWidth)
            return total + laneHeight(for: lane.text, width: width - 2 * windowPadding, font: font)
        } + CGFloat(max(0, lanes.count - 1)) * laneGap + 2 * windowPadding
        let height = min(measuredHeight, visibleFrame.height * maxWindowHeightRatio)
        let x = visibleFrame.midX - width / 2
        let y = visibleFrame.minY + bottomScreenMargin
        window.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true)
    }

    private func hideWindow() {
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            window.animator().alphaValue = 0
        } completionHandler: {
            self.window.orderOut(nil)
        }
    }

    private var currentWindowWidth: CGFloat {
        let maxLane = lanes.values.map { laneWidth(for: $0.text) }.max() ?? minLaneWidth
        return max(maxLane, minLaneWidth) + 2 * windowPadding
    }

    private func laneWidth(for text: String) -> CGFloat {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return 960 }
        let maxWidth = screen.visibleFrame.width * maxWindowWidthRatio - 2 * windowPadding
        let singleLine = measuredSingleLineWidth(text, font: NSFont.systemFont(ofSize: maxTextFontSize, weight: .semibold))
        let desired = singleLine + textLeftInset + textRightInset
        return min(maxWidth, max(minLaneWidth, desired))
    }

    /// 字号从 64 递减到 44，使文本在给定宽度内 ≤2 行（对齐 Windows FitSubtitleText）。
    private func fittedFont(for text: String, width: CGFloat) -> NSFont {
        var size = maxTextFontSize
        while size > minTextFontSize {
            let font = NSFont.systemFont(ofSize: size, weight: .semibold)
            if lineCount(for: text, font: font, width: width) <= 2 {
                return font
            }
            size -= 2
        }
        return NSFont.systemFont(ofSize: minTextFontSize, weight: .semibold)
    }

    private func lineCount(for text: String, font: NSFont, width: CGFloat) -> Int {
        let height = measuredTextHeight(text, font: font, width: width)
        let lineHeight = ceil(font.boundingRectForFont.height)
        guard lineHeight > 0 else { return 1 }
        return max(1, Int((height / lineHeight).rounded()))
    }

    private func laneHeight(for text: String, width: CGFloat, font: NSFont) -> CGFloat {
        let textWidth = max(1, width - textLeftInset - textRightInset)
        let textHeight = measuredTextHeight(text, font: font, width: textWidth)
        return max(minLaneHeight, ceil(textHeight) + 2 * textVerticalInset)
    }

    private func measuredTextHeight(_ text: String, font: NSFont, width: CGFloat) -> CGFloat {
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: font])
        let rect = attributed.boundingRect(
            with: NSSize(width: width, height: .greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading]
        )
        return ceil(rect.height)
    }

    private func measuredSingleLineWidth(_ text: String, font: NSFont) -> CGFloat {
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: font])
        return ceil(attributed.size().width)
    }

    /// 设备主题色（对齐 Windows subtitle_window.cc ColorValue；auto 按系统外观折白/黑）。
    private static func nsColor(for color: OverlayThemeColor) -> NSColor {
        switch color {
        case .auto:
            let dark = NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            return dark ? NSColor.white : NSColor(calibratedRed: 0x10 / 255.0, green: 0x10 / 255.0, blue: 0x10 / 255.0, alpha: 1)
        case .white:
            return NSColor.white
        case .black:
            return NSColor(calibratedRed: 0x10 / 255.0, green: 0x10 / 255.0, blue: 0x10 / 255.0, alpha: 1)
        case .pink:
            return NSColor(calibratedRed: 0xff / 255.0, green: 0x6b / 255.0, blue: 0x9e / 255.0, alpha: 1)
        case .green:
            return NSColor(calibratedRed: 0x4f / 255.0, green: 0xd6 / 255.0, blue: 0x8c / 255.0, alpha: 1)
        case .yellow:
            return NSColor(calibratedRed: 0xff / 255.0, green: 0xc7 / 255.0, blue: 0x42 / 255.0, alpha: 1)
        case .blue:
            return NSColor(calibratedRed: 0x61 / 255.0, green: 0xad / 255.0, blue: 0xff / 255.0, alpha: 1)
        case .purple:
            return NSColor(calibratedRed: 0xb8 / 255.0, green: 0x8c / 255.0, blue: 0xff / 255.0, alpha: 1)
        }
    }
}
