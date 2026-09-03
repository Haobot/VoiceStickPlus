import AppKit
import QuartzCore

/// 悬浮窗主题计量：三档尺寸参数逐值对齐 Windows overlay_window.h:143-167 +
/// SizePx 映射表（overlay_window.cc:730-760）。shadowPadding=12 是窗口级透明边距
/// （Windows 同名常量；原设计阴影已废弃，blur/yoff 均为 0，仅保留 padding）。
private struct OverlayThemeMetrics {
    let size: OverlayThemeSize

    private func pick<T>(_ big: T, _ medium: T, _ small: T) -> T {
        switch size {
        case .medium: return medium
        case .small: return small
        case .big: return big
        }
    }

    var maxContentWidth: CGFloat { pick(620, 500, 380) }
    var minContentHeight: CGFloat { pick(84, 68, 56) }
    var horizontalPadding: CGFloat { pick(20, 16, 14) }
    var verticalPadding: CGFloat { pick(16, 12, 10) }
    var indicatorSize: CGFloat { pick(26, 22, 18) }
    var indicatorLeftMargin: CGFloat { pick(18, 15, 13) }
    var textIndicatorGap: CGFloat { pick(9, 8, 7) }
    var textFontSize: CGFloat { pick(21, 18, 16) }
    var textLineHeight: CGFloat { textFontSize * 1.5 }
    var hintFontSize: CGFloat { pick(14, 12, 11) }
    var hintGap: CGFloat { pick(8, 6, 5) }
    var noHintOpticalOffset: CGFloat { pick(3, 2, 2) }
    var barWidth: CGFloat { max(2, pick(3, 3, 2)) }
    var barSpacing: CGFloat { pick(4, 3, 3) }
    var barBaseHeight: CGFloat { pick(7, 6, 5) }
    var barAmplitude: CGFloat { pick(9, 7, 6) }
    var refiningDotDiameter: CGFloat { max(pick(6, 5, 4), 4) }
    var ringLineWidth: CGFloat { pick(3, 2, 2) }
    var ringInset: CGFloat { pick(5, 4, 4) }

    static let cornerRadius: CGFloat = 20
    static let shadowPadding: CGFloat = 12
    static let positionMargin: CGFloat = 28
}

/// 主题色解析结果（auto 已按系统外观折成 white/black；对齐 Windows
/// ResolveAutoThemeColor 的亮度分档语义：暗背景→白主题白字，亮背景→黑主题黑字）。
private enum ResolvedOverlayTheme {
    case white
    case black
    case pink
    case green
    case yellow
    case blue
    case purple

    /// 文字墨色（对齐 Windows InkRgb：黑主题 16，其余 246）。
    var ink: NSColor {
        switch self {
        case .black:
            return NSColor(calibratedWhite: 16.0 / 255.0, alpha: 1)
        default:
            return NSColor(calibratedWhite: 246.0 / 255.0, alpha: 1)
        }
    }

    /// 玻璃 tint（对齐 GlassBackdropWindow::ThemeTint，mix 0.12 由调用侧 alpha 表达）。
    var tint: NSColor {
        switch self {
        case .pink:
            return NSColor(calibratedRed: 1, green: 214.0 / 255.0, blue: 230.0 / 255.0, alpha: 0.12)
        case .green:
            return NSColor(calibratedRed: 214.0 / 255.0, green: 242.0 / 255.0, blue: 214.0 / 255.0, alpha: 0.12)
        case .yellow:
            return NSColor(calibratedRed: 1, green: 240.0 / 255.0, blue: 184.0 / 255.0, alpha: 0.12)
        case .blue:
            return NSColor(calibratedRed: 209.0 / 255.0, green: 232.0 / 255.0, blue: 1, alpha: 0.12)
        case .purple:
            return NSColor(calibratedRed: 230.0 / 255.0, green: 214.0 / 255.0, blue: 1, alpha: 0.12)
        case .white, .black:
            // 白/黑/auto 主题 tint 同为 RGB(252,252,252)。
            return NSColor(calibratedWhite: 252.0 / 255.0, alpha: 0.12)
        }
    }

    /// 1px 描边（黑主题深色 68/255，其余白色 34/255）。
    var border: NSColor {
        switch self {
        case .black:
            return NSColor(calibratedWhite: 16.0 / 255.0, alpha: 68.0 / 255.0)
        default:
            return NSColor.white.withAlphaComponent(34.0 / 255.0)
        }
    }

    /// 玻璃观感：白主题（暗背景用）压深色玻璃，黑/彩色主题用浅色玻璃。
    var glassAppearance: NSAppearance? {
        switch self {
        case .white:
            return NSAppearance(named: .darkAqua)
        default:
            return NSAppearance(named: .aqua)
        }
    }
}

final class OverlayController {
    /// 指示器模式（对齐 Windows OverlayWindow::Mode）。
    fileprivate enum IndicatorMode {
        case listening
        case refining
        case countdown(startedAt: CFAbsoluteTime, duration: TimeInterval)
        case paused
        case error
        case info
    }

    private let window: NSPanel
    private let glassView = NSVisualEffectView()
    private let tintView = NSView()
    private let borderView = NSView()
    private let indicatorView = OverlayIndicatorView()
    private let textClipView = NSView()
    private let textLineView = OverlayTextLineView()
    private let hintLabel = NSTextField(labelWithString: "")
    private var metrics = OverlayThemeMetrics(size: .big)

    private var themeColor: OverlayThemeColor = .auto
    private var resolvedTheme: ResolvedOverlayTheme = .black
    private var position: OverlayPosition = .bottomCenter
    private var stackIndex = 0
    private var mode: IndicatorMode = .paused
    private var isVisible = false
    private var refining = false

    private var text = ""
    private var hint = ""
    private var largestVisibleContentSize = NSSize.zero
    private var hideWorkItem: DispatchWorkItem?
    private var pendingHideCompletion: (() -> Void)?
    private var tickTimer: Timer?
    private var appearanceObserver: NSKeyValueObservation?

    // 文字横向滚动（对齐 Windows text_scroll_* + kTextTransitionMs=140 ease-out）。
    private var scrollFromOffset: CGFloat = 0
    private var scrollToOffset: CGFloat = 0
    private var lastScrollOffset: CGFloat = 0
    private var textTransitionStartedAt: CFAbsoluteTime = 0

    // 尺寸步进动画（对齐 Windows kAnimationStepMs=16、宽步进 40、高步进 18、snap 6）。
    private var animatedContentSize = NSSize.zero
    private var targetContentSize = NSSize.zero
    private static let tickInterval: TimeInterval = 0.016
    private static let widthResizeStep: CGFloat = 40
    private static let heightResizeStep: CGFloat = 18
    private static let resizeSnap: CGFloat = 6
    private static let textTransitionDuration: CFTimeInterval = 0.140

    init() {
        window = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 644, height: 108),
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
        window.alphaValue = 1

        let container = NSView()
        container.wantsLayer = true
        window.contentView = container

        glassView.blendingMode = .behindWindow
        glassView.state = .active
        glassView.material = .popover
        glassView.wantsLayer = true
        glassView.layer?.cornerRadius = OverlayThemeMetrics.cornerRadius
        glassView.layer?.cornerCurve = .continuous
        glassView.layer?.masksToBounds = true

        tintView.wantsLayer = true
        tintView.layer?.cornerRadius = OverlayThemeMetrics.cornerRadius
        tintView.layer?.cornerCurve = .continuous
        tintView.layer?.masksToBounds = true

        borderView.wantsLayer = true
        borderView.layer?.cornerRadius = OverlayThemeMetrics.cornerRadius
        borderView.layer?.cornerCurve = .continuous
        borderView.layer?.borderWidth = 1
        borderView.layer?.backgroundColor = NSColor.clear.cgColor

        textClipView.wantsLayer = true
        textClipView.layer?.masksToBounds = true

        hintLabel.alignment = .center
        hintLabel.lineBreakMode = .byClipping
        hintLabel.maximumNumberOfLines = 1

        container.addSubview(glassView)
        container.addSubview(tintView)
        container.addSubview(indicatorView)
        container.addSubview(textClipView)
        textClipView.addSubview(textLineView)
        container.addSubview(hintLabel)
        container.addSubview(borderView)

        applyResolvedTheme(.black)
        resolveAutoTheme()

        // auto 主题跟随系统深浅外观（Windows 为屏幕背景亮度采样；macOS 无录屏
        // 权限无法采样，用系统外观近似——差异已记录于 Doc/Ref/desktop-config.md）。
        appearanceObserver = NSApp.observe(\.effectiveAppearance, options: [.new]) { [weak self] _, _ in
            self?.resolveAutoTheme()
        }
    }

    // MARK: - 配置入口

    func setThemeColor(_ color: OverlayThemeColor) {
        guard themeColor != color else { return }
        themeColor = color
        resolveAutoTheme()
    }

    func setThemeSize(_ size: OverlayThemeSize) {
        guard metrics.size != size else { return }
        metrics = OverlayThemeMetrics(size: size)
        largestVisibleContentSize = .zero
        animatedContentSize = .zero
        if isVisible {
            layoutAndPosition(animated: false)
        }
    }

    func setPosition(_ position: OverlayPosition) {
        guard self.position != position else { return }
        self.position = position
        if isVisible {
            layoutAndPosition(animated: false)
        }
    }

    func setStackIndex(_ index: Int) {
        guard stackIndex != index else { return }
        stackIndex = index
        if isVisible {
            layoutAndPosition(animated: false)
        }
    }

    // MARK: - 展示 API（对齐 Windows OverlayWindow::Show*）

    func showListening(text: String) {
        refining = false
        show(mode: .listening, text: text.isEmpty ? "Listening..." : text)
    }

    /// ASR 部分结果/最终结果显示：退出精修态（对齐 Windows ShowPartial）。
    func showPartial(_ text: String) {
        refining = false
        show(mode: .listening, text: text.isEmpty ? "Processing..." : text)
    }

    /// 进入精修态：三点跳动指示器 + ASR 原文（对齐 Windows ShowRefining）。
    func showRefining(_ text: String) {
        refining = true
        show(mode: .refining, text: text.isEmpty ? "Processing..." : text, skipTextTransition: true)
    }

    /// 流式精修追加：跳过滚动过渡动画直接显示累积文本（对齐 Windows AppendPartial）。
    func appendPartial(_ text: String) {
        show(mode: refining ? .refining : .listening,
             text: text.isEmpty ? "Processing..." : text,
             skipTextTransition: true)
    }

    /// 1.2s 圆环倒计时后隐藏并回调（对齐 Windows ShowFinalCountdown）。
    func showFinal(text: String, onHidden: (() -> Void)? = nil) {
        show(
            mode: .countdown(startedAt: CFAbsoluteTimeGetCurrent(), duration: 1.2),
            text: text.isEmpty ? "No speech" : text,
            autoHideAfter: 1.2,
            onHidden: onHidden
        )
    }

    func showPausedFinal(text: String) {
        show(mode: .paused, text: text.isEmpty ? "No speech" : text, hint: "Front: Send    Side: Cancel")
    }

    func showError(_ text: String, onHidden: (() -> Void)? = nil) {
        show(mode: .error, text: text.isEmpty ? "ASR Error" : text, autoHideAfter: 2, onHidden: onHidden)
    }

    /// 中性信息（圆点指示器），自定义时长自动隐藏（对齐 Windows ShowTimedMessage）。
    func showTimedMessage(_ text: String, duration: TimeInterval, onHidden: (() -> Void)? = nil) {
        show(mode: .info, text: text, autoHideAfter: max(duration, 0.001), onHidden: onHidden)
    }

    /// 立即隐藏（Windows StartFadeOut 直接置 alpha 0，无渐变）。
    func hide(onHidden: (() -> Void)? = nil) {
        hideWorkItem?.cancel()
        hideWorkItem = nil
        let completion = onHidden ?? pendingHideCompletion
        pendingHideCompletion = nil
        stopTickTimer()
        refining = false
        isVisible = false
        largestVisibleContentSize = .zero
        animatedContentSize = .zero
        lastScrollOffset = 0
        scrollFromOffset = 0
        scrollToOffset = 0
        textTransitionStartedAt = 0
        window.orderOut(nil)
        completion?()
    }

    // MARK: - 私有：展示与布局

    private func show(
        mode: IndicatorMode,
        text: String,
        hint: String = "",
        autoHideAfter delay: TimeInterval? = nil,
        skipTextTransition: Bool = false,
        onHidden: (() -> Void)? = nil
    ) {
        let update = {
            self.hideWorkItem?.cancel()
            self.pendingHideCompletion = onHidden
            if case .refining = mode {} else { self.refining = false }

            // 文字滚动过渡（对齐 Windows Show 的 skip_text_transition 分支）。
            if skipTextTransition {
                self.scrollFromOffset = self.lastScrollOffset
                self.scrollToOffset = self.lastScrollOffset
                self.textTransitionStartedAt = 0
            } else if self.isVisible, text != self.text {
                self.scrollFromOffset = self.lastScrollOffset
                self.textTransitionStartedAt = CFAbsoluteTimeGetCurrent()
            } else if !self.isVisible {
                self.textTransitionStartedAt = 0
                self.scrollFromOffset = 0
                self.scrollToOffset = 0
                self.lastScrollOffset = 0
            }

            self.mode = mode
            self.text = text
            self.hint = hint
            self.hintLabel.isHidden = hint.isEmpty
            self.applyTextStyles()
            self.hintLabel.stringValue = hint

            self.layoutAndPosition(animated: self.isVisible)
            if !self.isVisible {
                self.isVisible = true
                self.window.orderFrontRegardless()
            }
            self.updateTickTimer()

            if let delay {
                let workItem = DispatchWorkItem { [weak self] in
                    self?.hide()
                }
                self.hideWorkItem = workItem
                DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: workItem)
            }
        }
        if Thread.isMainThread {
            update()
        } else {
            DispatchQueue.main.async(execute: update)
        }
    }

    /// 计算目标内容尺寸与窗口位置（布局算法对齐 Windows Reposition：
    /// 宽度恒为档位最大内容宽，高度按内容只增不减）。
    private func layoutAndPosition(animated: Bool) {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let visibleFrame = screen.visibleFrame
        let shadow = OverlayThemeMetrics.shadowPadding

        let availableMaxWidth = min(metrics.maxContentWidth, visibleFrame.width - 48 - shadow * 2)
        let sideChrome = metrics.indicatorLeftMargin + metrics.indicatorSize
            + metrics.textIndicatorGap + metrics.horizontalPadding
        let maxTextWidth = max(1, availableMaxWidth - sideChrome)

        let hintHeight = hint.isEmpty
            ? 0
            : (hintFont().boundingRectForFont.height + metrics.hintGap)
        let desiredContentHeight = metrics.verticalPadding * 2
            + max(metrics.textLineHeight + hintHeight, metrics.indicatorSize)
        let maxContentHeight = max(metrics.minContentHeight, visibleFrame.height - 120 - shadow * 2)
        var contentHeight = min(maxContentHeight, max(metrics.minContentHeight, ceil(desiredContentHeight)))
        var contentWidth = availableMaxWidth

        // 宽高只增不减直到 hide 复位（对齐 Windows largest_visible_*）。
        contentWidth = max(contentWidth, largestVisibleContentSize.width)
        contentHeight = max(contentHeight, largestVisibleContentSize.height)
        largestVisibleContentSize = NSSize(width: contentWidth, height: contentHeight)
        targetContentSize = largestVisibleContentSize

        if !animated || animatedContentSize == .zero {
            animatedContentSize = targetContentSize
        }
        applyWindowFrame()
        layoutContent()
    }

    private func applyWindowFrame() {
        guard let screen = NSScreen.main ?? NSScreen.screens.first else { return }
        let shadow = OverlayThemeMetrics.shadowPadding
        let windowSize = NSSize(
            width: animatedContentSize.width + shadow * 2,
            height: animatedContentSize.height + shadow * 2
        )
        let origin = frameOrigin(in: screen.visibleFrame, windowSize: windowSize)
        window.setFrame(NSRect(origin: origin, size: windowSize), display: true)
    }

    private func frameOrigin(in visibleFrame: NSRect, windowSize: NSSize) -> CGPoint {
        let gap: CGFloat = 14
        let stackStep = windowSize.height + gap
        let margin = OverlayThemeMetrics.positionMargin
        switch position {
        case .center:
            let direction = stackDirection(for: stackIndex)
            let offset = CGFloat(abs(direction)) * stackStep * CGFloat(direction.signum())
            return CGPoint(
                x: visibleFrame.midX - windowSize.width / 2,
                y: clamp(
                    visibleFrame.midY - windowSize.height / 2 + offset,
                    min: visibleFrame.minY + margin,
                    max: visibleFrame.maxY - margin - windowSize.height
                )
            )
        case .bottomCenter:
            // 对齐 Windows kBottomCenter：底部间隙 max(84, 屏高/9)，多设备向上堆叠。
            let bottomGap = max(84, visibleFrame.height / 9)
            return CGPoint(
                x: visibleFrame.midX - windowSize.width / 2,
                y: visibleFrame.minY + bottomGap + CGFloat(stackIndex) * stackStep
            )
        case .topLeft:
            return CGPoint(
                x: visibleFrame.minX + margin,
                y: visibleFrame.maxY - margin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .topRight:
            return CGPoint(
                x: visibleFrame.maxX - margin - windowSize.width,
                y: visibleFrame.maxY - margin - windowSize.height - CGFloat(stackIndex) * stackStep
            )
        case .bottomLeft:
            return CGPoint(
                x: visibleFrame.minX + margin,
                y: visibleFrame.minY + margin + CGFloat(stackIndex) * stackStep
            )
        case .bottomRight:
            return CGPoint(
                x: visibleFrame.maxX - margin - windowSize.width,
                y: visibleFrame.minY + margin + CGFloat(stackIndex) * stackStep
            )
        }
    }

    private func stackDirection(for index: Int) -> Int {
        guard index > 0 else { return 0 }
        let magnitude = (index + 1) / 2
        return index % 2 == 1 ? -magnitude : magnitude
    }

    private func clamp(_ value: CGFloat, min minValue: CGFloat, max maxValue: CGFloat) -> CGFloat {
        Swift.max(minValue, Swift.min(maxValue, value))
    }

    /// 内容布局（手工 frame，对齐 Windows PaintText 的坐标系）。
    private func layoutContent() {
        guard let container = window.contentView else { return }
        let shadow = OverlayThemeMetrics.shadowPadding
        let glassRect = NSRect(
            x: shadow,
            y: shadow,
            width: animatedContentSize.width,
            height: animatedContentSize.height
        )
        glassView.frame = glassRect
        tintView.frame = glassRect
        borderView.frame = glassRect

        let indicatorX = glassRect.minX + metrics.indicatorLeftMargin
        indicatorView.frame = NSRect(
            x: indicatorX,
            y: glassRect.midY - metrics.indicatorSize / 2,
            width: metrics.indicatorSize,
            height: metrics.indicatorSize
        )
        indicatorView.setMode(mode, metrics: metrics)

        let textX = indicatorX + metrics.indicatorSize + metrics.textIndicatorGap
        let textWidth = max(1, glassRect.maxX - metrics.horizontalPadding - textX)
        textClipView.frame = NSRect(x: textX, y: glassRect.minY, width: textWidth, height: glassRect.height)
        textLineView.frame = textClipView.bounds
        textLineView.setText(text, font: textFont(), ink: inkColor(alpha: 248))

        // 文字+提示块整体垂直居中；无提示时向下加光学偏移（对齐 Windows）。
        let hintHeight = hint.isEmpty ? 0 : hintFont().boundingRectForFont.height
        let gap = hint.isEmpty ? 0 : metrics.hintGap
        let blockHeight = metrics.textLineHeight + gap + hintHeight
        var blockBottom = glassRect.midY - blockHeight / 2
        if hint.isEmpty {
            blockBottom -= metrics.noHintOpticalOffset
        }
        // 文本行中心（textLineView 自身坐标系，原点在 clipView 左下）。
        let textCenterYInClip = (blockBottom - glassRect.minY) + hintHeight + gap + metrics.textLineHeight / 2
        textLineView.textCenterY = textCenterYInClip
        hintLabel.font = hintFont()
        hintLabel.sizeToFit()
        let hintWidth = min(hintLabel.frame.width, textWidth)
        hintLabel.frame = NSRect(
            x: textX + max(0, (textWidth - hintWidth) / 2),
            y: blockBottom,
            width: hintWidth,
            height: hintHeight
        )

        // 文字横向滚动目标（对齐 Windows text_scroll_to_offset_）。
        let measuredTextWidth = textLineView.measuredTextWidth
        scrollToOffset = max(0, measuredTextWidth - textWidth)
        textLineView.centeredOffset = max(0, (textWidth - measuredTextWidth) / 2)
        applyScrollOffset(currentScrollOffset())
    }

    // MARK: - 主题

    private func resolveAutoTheme() {
        let resolved: ResolvedOverlayTheme
        switch themeColor {
        case .auto:
            let dark = NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            resolved = dark ? .white : .black
        case .white: resolved = .white
        case .black: resolved = .black
        case .pink: resolved = .pink
        case .green: resolved = .green
        case .yellow: resolved = .yellow
        case .blue: resolved = .blue
        case .purple: resolved = .purple
        }
        applyResolvedTheme(resolved)
    }

    private func applyResolvedTheme(_ theme: ResolvedOverlayTheme) {
        resolvedTheme = theme
        glassView.appearance = theme.glassAppearance
        tintView.layer?.backgroundColor = theme.tint.cgColor
        borderView.layer?.borderColor = theme.border.cgColor
        applyTextStyles()
        indicatorView.inkColor = inkColor(alpha: 218)
        indicatorView.trackColor = inkColor(alpha: 72)
    }

    private func inkColor(alpha: Int) -> NSColor {
        resolvedTheme.ink.withAlphaComponent(CGFloat(alpha) / 255.0)
    }

    private func textFont() -> NSFont {
        .systemFont(ofSize: metrics.textFontSize, weight: .regular)
    }

    private func hintFont() -> NSFont {
        .systemFont(ofSize: metrics.hintFontSize, weight: .semibold)
    }

    private func applyTextStyles() {
        hintLabel.font = hintFont()
        hintLabel.textColor = inkColor(alpha: 180)
        // 文字阴影：黑色 72/255 下移 1（对齐 Windows kTextShadowAlpha + 1dp 偏移）。
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(72.0 / 255.0)
        shadow.shadowBlurRadius = 0
        shadow.shadowOffset = NSSize(width: 0, height: -1)
        hintLabel.shadow = shadow
        textLineView.textShadow = shadow
    }

    // MARK: - 动画节拍

    private func needsContinuousTick() -> Bool {
        if animatedContentSize != targetContentSize { return true }
        if textTransitionStartedAt != 0 { return true }
        switch mode {
        case .listening, .refining, .countdown:
            return true
        case .paused, .error, .info:
            return false
        }
    }

    private func updateTickTimer() {
        if needsContinuousTick() {
            guard tickTimer == nil else { return }
            let timer = Timer(timeInterval: Self.tickInterval, repeats: true) { [weak self] _ in
                self?.tick()
            }
            RunLoop.main.add(timer, forMode: .common)
            tickTimer = timer
        } else {
            stopTickTimer()
        }
    }

    private func stopTickTimer() {
        tickTimer?.invalidate()
        tickTimer = nil
    }

    private func tick() {
        var moved = false
        if animatedContentSize != targetContentSize {
            animatedContentSize = NSSize(
                width: step(animatedContentSize.width, targetContentSize.width, Self.widthResizeStep),
                height: step(animatedContentSize.height, targetContentSize.height, Self.heightResizeStep)
            )
            applyWindowFrame()
            moved = true
        }
        if textTransitionStartedAt != 0 {
            applyScrollOffset(currentScrollOffset())
        }
        if moved {
            layoutContent()
        }
        indicatorView.setMode(mode, metrics: metrics)
        indicatorView.needsDisplay = true
        updateTickTimer()
    }

    private func step(_ current: CGFloat, _ target: CGFloat, _ stepSize: CGFloat) -> CGFloat {
        let delta = target - current
        let distance = abs(delta)
        if distance <= Self.resizeSnap { return target }
        return current + (delta > 0 ? 1 : -1) * min(distance, stepSize)
    }

    private func currentScrollOffset() -> CGFloat {
        guard textTransitionStartedAt != 0 else { return scrollToOffset }
        let elapsed = CGFloat(CFAbsoluteTimeGetCurrent() - textTransitionStartedAt)
        let progress = clamp(elapsed / CGFloat(Self.textTransitionDuration), min: 0, max: 1)
        let eased = 1 - (1 - progress) * (1 - progress)
        let value = scrollFromOffset + (scrollToOffset - scrollFromOffset) * eased
        if progress >= 1 {
            textTransitionStartedAt = 0
        }
        return value
    }

    private func applyScrollOffset(_ offset: CGFloat) {
        lastScrollOffset = offset
        textLineView.scrollOffset = offset
    }
}

// MARK: - 指示器视图（逐帧绘制，公式对齐 Windows PaintIndicator）

private final class OverlayIndicatorView: NSView {
    typealias Mode = OverlayController.IndicatorMode

    private var mode: Mode = .paused
    private var metrics = OverlayThemeMetrics(size: .big)
    var inkColor = NSColor.white
    var trackColor = NSColor.white.withAlphaComponent(0.3)

    func setMode(_ mode: Mode, metrics: OverlayThemeMetrics) {
        self.mode = mode
        self.metrics = metrics
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let size = min(bounds.width, bounds.height)
        let cx = bounds.midX
        let cy = bounds.midY
        let t = CFAbsoluteTimeGetCurrent().truncatingRemainder(dividingBy: 100_000)

        switch mode {
        case .listening:
            let totalWidth = 3 * metrics.barWidth + 2 * metrics.barSpacing
            let startX = cx - totalWidth / 2
            inkColor.setFill()
            for i in 0..<3 {
                let phase = t * 4.2 + Double(i) * 0.9
                let height = metrics.barBaseHeight
                    + metrics.barAmplitude * CGFloat(0.5 + 0.5 * sin(phase))
                let x = startX + CGFloat(i) * (metrics.barWidth + metrics.barSpacing)
                let rect = NSRect(x: x, y: cy - height / 2, width: metrics.barWidth, height: height)
                NSBezierPath(roundedRect: rect, xRadius: metrics.barWidth / 2, yRadius: metrics.barWidth / 2).fill()
            }
        case .refining:
            let diameter = metrics.refiningDotDiameter
            let totalWidth = 3 * diameter + 2 * metrics.barSpacing
            let startX = cx - totalWidth / 2
            inkColor.setFill()
            for i in 0..<3 {
                // 每点相位错开 0.35s，wave 越大越靠上（本视图 y 轴向上，加即向上）。
                let phase = t * 3.0 + Double(i) * 0.35
                let wave = CGFloat(0.5 + 0.5 * sin(phase * 2.0 * .pi / 1.2))
                let x = startX + CGFloat(i) * (diameter + metrics.barSpacing)
                let y = cy - diameter / 2 + wave * diameter * 0.5
                NSBezierPath(ovalIn: NSRect(x: x, y: y, width: diameter, height: diameter)).fill()
            }
        case .countdown(let startedAt, let duration):
            let inset = metrics.ringInset
            let ringRect = NSRect(
                x: cx - size / 2 + inset,
                y: cy - size / 2 + inset,
                width: size - inset * 2,
                height: size - inset * 2
            )
            trackColor.setStroke()
            let track = NSBezierPath(ovalIn: ringRect)
            track.lineWidth = metrics.ringLineWidth
            track.stroke()
            let elapsed = CFAbsoluteTimeGetCurrent() - startedAt
            let remaining = max(0, min(1, 1 - elapsed / duration))
            if remaining > 0 {
                inkColor.setStroke()
                // 从顶部 (−90°) 顺时针扫 −360×remaining；翻转坐标系下逆时针为正，
                // 顶部为 90°，顺时针扫掠用负角度。
                let ring = NSBezierPath()
                ring.appendArc(
                    withCenter: NSPoint(x: ringRect.midX, y: ringRect.midY),
                    radius: ringRect.width / 2,
                    startAngle: 90,
                    endAngle: 90 - 360 * CGFloat(remaining),
                    clockwise: true
                )
                ring.lineWidth = metrics.ringLineWidth
                ring.lineCapStyle = .round
                ring.stroke()
            }
        case .paused:
            inkColor.setStroke()
            let inset = metrics.ringInset
            let ringRect = NSRect(
                x: cx - size / 2 + inset,
                y: cy - size / 2 + inset,
                width: size - inset * 2,
                height: size - inset * 2
            )
            let ring = NSBezierPath(ovalIn: ringRect)
            ring.lineWidth = metrics.ringLineWidth
            ring.stroke()
        case .error:
            let errorColor = NSColor(calibratedRed: 200.0 / 255.0, green: 60.0 / 255.0, blue: 60.0 / 255.0, alpha: 1)
            errorColor.setStroke()
            let inset = metrics.ringInset
            let ringRect = NSRect(
                x: cx - size / 2 + inset,
                y: cy - size / 2 + inset,
                width: size - inset * 2,
                height: size - inset * 2
            )
            let ring = NSBezierPath(ovalIn: ringRect)
            ring.lineWidth = metrics.ringLineWidth
            ring.stroke()
            let xInset = size / 3
            let xPath = NSBezierPath()
            xPath.lineWidth = metrics.ringLineWidth
            xPath.lineCapStyle = .round
            xPath.move(to: NSPoint(x: cx - size / 2 + xInset, y: cy - size / 2 + xInset))
            xPath.line(to: NSPoint(x: cx + size / 2 - xInset, y: cy + size / 2 - xInset))
            xPath.move(to: NSPoint(x: cx + size / 2 - xInset, y: cy - size / 2 + xInset))
            xPath.line(to: NSPoint(x: cx - size / 2 + xInset, y: cy + size / 2 - xInset))
            xPath.stroke()
        case .info:
            inkColor.setFill()
            let inset = size / 4
            NSBezierPath(ovalIn: NSRect(
                x: cx - size / 2 + inset,
                y: cy - size / 2 + inset,
                width: size - inset * 2,
                height: size - inset * 2
            )).fill()
        }
    }
}

// MARK: - 单行滚动文字视图（裁剪 + 横向滚动，对齐 Windows PaintText）

private final class OverlayTextLineView: NSView {
    private var text = ""
    private var font = NSFont.systemFont(ofSize: 21)
    private var ink = NSColor.white
    var textShadow: NSShadow?
    var scrollOffset: CGFloat = 0 { didSet { needsDisplay = true } }
    var centeredOffset: CGFloat = 0 { didSet { needsDisplay = true } }
    /// 文本行中心 y（自身坐标系）；nil 时按整视图垂直居中。
    var textCenterY: CGFloat? = nil { didSet { needsDisplay = true } }
    private(set) var measuredTextWidth: CGFloat = 0

    func setText(_ text: String, font: NSFont, ink: NSColor) {
        self.text = text
        self.font = font
        self.ink = ink
        let attributed = NSAttributedString(string: text.isEmpty ? " " : text, attributes: [.font: font])
        measuredTextWidth = ceil(attributed.size().width)
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard !text.isEmpty else { return }
        var attributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: ink
        ]
        if let textShadow {
            attributes[.shadow] = textShadow
        }
        // 非翻转坐标系下 draw(at:) 的点在文本块下缘，按自然字高绕 textCenterY 居中。
        let naturalHeight = ceil(font.boundingRectForFont.height)
        let centerY = textCenterY ?? bounds.midY
        let y = centerY - naturalHeight / 2
        let x = centeredOffset - scrollOffset
        NSAttributedString(string: text, attributes: attributes).draw(at: NSPoint(x: x, y: y))
    }
}
