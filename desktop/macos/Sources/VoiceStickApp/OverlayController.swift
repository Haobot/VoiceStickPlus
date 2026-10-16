import AppKit
import QuartzCore
import VoiceStickCore

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

    /// 玻璃基色（不透明）：各玻璃方案按需叠加透明度（tint/tintBoosted/glow/A 方案 tintColor）。
    var tintBase: NSColor {
        switch self {
        case .pink:
            return NSColor(calibratedRed: 1, green: 214.0 / 255.0, blue: 230.0 / 255.0, alpha: 1)
        case .green:
            return NSColor(calibratedRed: 214.0 / 255.0, green: 242.0 / 255.0, blue: 214.0 / 255.0, alpha: 1)
        case .yellow:
            return NSColor(calibratedRed: 1, green: 240.0 / 255.0, blue: 184.0 / 255.0, alpha: 1)
        case .blue:
            return NSColor(calibratedRed: 209.0 / 255.0, green: 232.0 / 255.0, blue: 1, alpha: 1)
        case .purple:
            return NSColor(calibratedRed: 230.0 / 255.0, green: 214.0 / 255.0, blue: 1, alpha: 1)
        case .white, .black:
            // 白/黑/auto 主题基色同为 RGB(252,252,252)。
            return NSColor(calibratedWhite: 252.0 / 255.0, alpha: 1)
        }
    }

    var isNeutral: Bool { self == .white || self == .black }

    /// A 原生液态玻璃：NSGlassEffectView tintColor（浓度直接由透明度滑杆驱动）。
    func glassTintColor(alpha: CGFloat) -> NSColor {
        isNeutral ? NSColor.white.withAlphaComponent(alpha) : tintBase.withAlphaComponent(alpha)
    }

    /// B 高光玻璃：tint 增艳，中性主题略降（×0.8）避免发白。
    func tintBoosted(alpha: CGFloat) -> NSColor {
        isNeutral ? tintBase.withAlphaComponent(alpha * 0.8) : tintBase.withAlphaComponent(alpha)
    }

    /// C 深色镜面：玻璃内主题色微光（系数压低保持深底），中性主题近无。
    func tintGlow(alpha: CGFloat) -> NSColor {
        isNeutral ? NSColor.white.withAlphaComponent(alpha * 0.3) : tintBase.withAlphaComponent(alpha * 0.7)
    }

    /// C 深色镜面：辉光描边渐变（对角线亮→暗，中性主题退化为银白）。
    var glowColors: [CGColor] {
        if isNeutral {
            return [
                NSColor.white.withAlphaComponent(0.55).cgColor,
                NSColor.white.withAlphaComponent(0.10).cgColor
            ]
        }
        return [tintBase.withAlphaComponent(0.85).cgColor, tintBase.withAlphaComponent(0.22).cgColor]
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

    /// B 高光玻璃外描边：比现行 border 更亮（白 60/255），高光玻璃需要更清晰的边缘。
    var borderBoosted: NSColor {
        switch self {
        case .black:
            return NSColor(calibratedWhite: 16.0 / 255.0, alpha: 90.0 / 255.0)
        default:
            return NSColor.white.withAlphaComponent(60.0 / 255.0)
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
    /// B/C 柔和投影承重视图（layer shadow 画在玻璃矩形外侧；A 原生玻璃自带层次不用投影）。
    private let shadowView = NSView()
    /// A 方案背景（实际类型 NSGlassEffectView；类型本身 macOS 26+ 起可用，
    /// 存储抹平为 NSView，取用走 glassEffect 计算属性 + #available）。
    private var glassEffectView: NSView?

    /// NSGlassEffectView 专属属性入口（cornerRadius/tintColor 不在 NSView 上）。
    @available(macOS 26.0, *)
    private var glassEffect: NSGlassEffectView? { glassEffectView as? NSGlassEffectView }
    private let glassView = NSVisualEffectView()
    private let tintView = NSView()
    /// B/C 顶部镜面高光渐变（玻璃上缘反光）。
    private let highlightView = NSView()
    private var highlightGradLayer: CAGradientLayer?
    /// B 内侧暗描边（与外侧亮描边叠加出玻璃厚度感）。
    private let innerBorderView = NSView()
    /// C 主题色渐变辉光描边（CAShapeLayer 描边作 mask 的 CAGradientLayer）。
    private let glowBorderView = NSView()
    private var glowBorderBuiltSize = NSSize.zero
    private let borderView = NSView()
    private let indicatorView = OverlayIndicatorView()
    private let textClipView = NSView()
    private let textLineView = OverlayTextLineView()
    private let hintLabel = NSTextField(labelWithString: "")
    private var metrics = OverlayThemeMetrics(size: .big)

    private var themeColor: OverlayThemeColor = .auto
    private var resolvedTheme: ResolvedOverlayTheme = .black
    /// 用户选择的玻璃方案（托盘「悬浮窗玻璃方案」全局项）。
    private var glassStyle: OverlayGlassStyle = .nativeLiquid
    /// 实际生效方案（A 在 macOS < 26 自动回退为 B）。
    private var effectiveGlassStyle: OverlayGlassStyle = .nativeLiquid
    /// 玻璃 tint 浓度（透明度滑杆，0…1；0.20 = 三方案历史默认档位）。
    private var glassTintAlpha: CGFloat = 0.20
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

        // 背景栈 z 序：投影 → 原生玻璃(A) → 材质玻璃(B/C) → tint → 高光 → 内容 → 描边。
        shadowView.wantsLayer = true
        shadowView.layer?.shadowColor = NSColor.black.cgColor
        shadowView.layer?.shadowOpacity = 0
        shadowView.layer?.shadowRadius = 16
        // 非 flipped 坐标系下 y 向上，负值投影朝下。
        shadowView.layer?.shadowOffset = NSSize(width: 0, height: -6)

        if #available(macOS 26.0, *) {
            let effect = NSGlassEffectView()
            effect.wantsLayer = true
            glassEffectView = effect
        }
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

        highlightView.wantsLayer = true
        if let layer = highlightView.layer {
            let gradient = CAGradientLayer()
            gradient.startPoint = CGPoint(x: 0.5, y: 1.0)
            gradient.endPoint = CGPoint(x: 0.5, y: 0.0)
            gradient.locations = [0, 0.6]
            layer.addSublayer(gradient)
            highlightGradLayer = gradient
        }

        innerBorderView.wantsLayer = true
        innerBorderView.layer?.cornerRadius = OverlayThemeMetrics.cornerRadius - 1.5
        innerBorderView.layer?.cornerCurve = .continuous
        innerBorderView.layer?.borderWidth = 1
        innerBorderView.layer?.masksToBounds = true

        glowBorderView.wantsLayer = true

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

        container.addSubview(shadowView)
        if let glassEffectView {
            container.addSubview(glassEffectView)
        }
        container.addSubview(glassView)
        container.addSubview(tintView)
        container.addSubview(highlightView)
        container.addSubview(indicatorView)
        container.addSubview(textClipView)
        textClipView.addSubview(textLineView)
        container.addSubview(hintLabel)
        container.addSubview(innerBorderView)
        container.addSubview(borderView)
        container.addSubview(glowBorderView)

        applyGlassStyle()
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

    func setGlassStyle(_ style: OverlayGlassStyle) {
        guard glassStyle != style else { return }
        glassStyle = style
        applyGlassStyle()
        // 墨色/描边/高光都随方案重算。
        applyResolvedTheme(resolvedTheme)
        if isVisible {
            layoutAndPosition(animated: false)
        }
    }

    /// 玻璃 tint 浓度（透明度滑杆）：A 直接映射 NSGlassEffectView.tintColor 的 alpha，
    /// B/C 按各自系数缩放（见 ResolvedOverlayTheme）。
    func setGlassTintAlpha(_ alpha: CGFloat) {
        let clamped = min(max(alpha, 0), 1)
        guard glassTintAlpha != clamped else { return }
        glassTintAlpha = clamped
        applyResolvedTheme(resolvedTheme)
    }

    // MARK: - 玻璃背景方案

    /// 窗口透明边距：B/C 带手绘柔和投影，在 12px 阴影边距外再多留 14px，
    /// 否则投影下缘（offset 6 + radius 16）会被窗口边界裁切。
    private var windowPadding: CGFloat {
        OverlayThemeMetrics.shadowPadding + (effectiveGlassStyle == .nativeLiquid ? 0 : 14)
    }

    private var effectiveCornerRadius: CGFloat {
        effectiveGlassStyle == .darkMirror ? 24 : OverlayThemeMetrics.cornerRadius
    }

    /// 按 glassStyle 重建背景栈可见性与材质。A 方案内容仍以兄弟视图叠在玻璃之上
    /// （标准 AppKit z 序可靠；NSGlassEffectView.contentView 契约只约束其自身子视图）。
    private func applyGlassStyle() {
        var style = glassStyle
        if style == .nativeLiquid {
            if #available(macOS 26.0, *) {} else {
                style = .specular // 低版本无原生液态玻璃，回退 B 高光玻璃
            }
        }
        effectiveGlassStyle = style

        let native = style == .nativeLiquid
        let specular = style == .specular
        glassEffectView?.isHidden = !native
        glassView.isHidden = native
        tintView.isHidden = native
        highlightView.isHidden = native
        shadowView.isHidden = native
        innerBorderView.isHidden = native
        borderView.isHidden = native || !specular // C 用辉光描边替代普通描边
        glowBorderView.isHidden = native || specular

        glassView.material = style == .darkMirror ? .hudWindow : .popover
        let radius = effectiveCornerRadius
        glassView.layer?.cornerRadius = radius
        tintView.layer?.cornerRadius = radius
        borderView.layer?.cornerRadius = radius
        innerBorderView.layer?.cornerRadius = max(0, radius - 1.5)
        if #available(macOS 26.0, *) {
            glassEffect?.cornerRadius = radius
        }
        shadowView.layer?.shadowOpacity = 0.38
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

    /// 0.4s 圆环倒计时后隐藏并回调（对齐 Windows ShowFinalCountdown）。
    /// 2026-10-11 从 1.2s 压到 0.4s：粘贴被这段倒计时整体延后，实测「浮窗→粘贴」
    /// 固定 +1.26s，是用户反馈延迟的主要可压缩项；倒计时仍保留（前端键暂停/侧键取消
    /// 确认语义不变，恢复上次输入确认仍是误粘贴兜底）。
    func showFinal(text: String, onHidden: (() -> Void)? = nil) {
        show(
            mode: .countdown(startedAt: CFAbsoluteTimeGetCurrent(), duration: 0.4),
            text: text.isEmpty ? "No speech" : text,
            autoHideAfter: 0.4,
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
        let padding = windowPadding

        let availableMaxWidth = min(metrics.maxContentWidth, visibleFrame.width - 48 - padding * 2)
        let sideChrome = metrics.indicatorLeftMargin + metrics.indicatorSize
            + metrics.textIndicatorGap + metrics.horizontalPadding
        let maxTextWidth = max(1, availableMaxWidth - sideChrome)

        let hintHeight = hint.isEmpty
            ? 0
            : (hintFont().boundingRectForFont.height + metrics.hintGap)
        let desiredContentHeight = metrics.verticalPadding * 2
            + max(metrics.textLineHeight + hintHeight, metrics.indicatorSize)
        let maxContentHeight = max(metrics.minContentHeight, visibleFrame.height - 120 - padding * 2)
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
        let padding = windowPadding
        let windowSize = NSSize(
            width: animatedContentSize.width + padding * 2,
            height: animatedContentSize.height + padding * 2
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
        let padding = windowPadding
        let glassRect = NSRect(
            x: padding,
            y: padding,
            width: animatedContentSize.width,
            height: animatedContentSize.height
        )
        shadowView.frame = glassRect
        if let shadowLayer = shadowView.layer {
            // 投影路径按玻璃矩形裁剪出形状；圆角用普通圆（CGPath 无连续曲率，投影无感差异）。
            shadowLayer.shadowPath = CGPath(
                roundedRect: glassRect,
                cornerWidth: effectiveCornerRadius,
                cornerHeight: effectiveCornerRadius,
                transform: nil
            )
        }
        glassEffectView?.frame = glassRect
        glassView.frame = glassRect
        tintView.frame = glassRect
        highlightView.frame = glassRect
        highlightGradLayer?.frame = highlightView.bounds
        innerBorderView.frame = glassRect.insetBy(dx: 1.5, dy: 1.5)
        glowBorderView.frame = glassRect
        borderView.frame = glassRect

        if effectiveGlassStyle == .darkMirror,
           glowBorderBuiltSize != glowBorderView.bounds.size {
            glowBorderBuiltSize = glowBorderView.bounds.size
            refreshGlowBorder()
        }

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
        let style = effectiveGlassStyle

        // A：原生玻璃（外观/tintColor 走 NSGlassEffectView 自身属性）。
        glassEffectView?.appearance = theme.glassAppearance
        if #available(macOS 26.0, *) {
            glassEffect?.tintColor = theme.glassTintColor(alpha: glassTintAlpha)
        }

        // B/C：材质玻璃 + 色彩叠层；C 强制深色玻璃（控制中心风）。
        glassView.appearance = style == .darkMirror
            ? NSAppearance(named: .darkAqua)
            : theme.glassAppearance
        switch style {
        case .nativeLiquid:
            break
        case .specular:
            tintView.layer?.backgroundColor = theme.tintBoosted(alpha: glassTintAlpha).cgColor
            borderView.layer?.borderColor = theme.borderBoosted.cgColor
            innerBorderView.layer?.borderColor = NSColor.black.withAlphaComponent(60.0 / 255.0).cgColor
            highlightGradLayer?.colors = [
                NSColor.white.withAlphaComponent(0.13).cgColor,
                NSColor.clear.cgColor
            ]
        case .darkMirror:
            tintView.layer?.backgroundColor = theme.tintGlow(alpha: glassTintAlpha).cgColor
            innerBorderView.layer?.borderColor = NSColor.black.withAlphaComponent(115.0 / 255.0).cgColor
            highlightGradLayer?.colors = [
                NSColor.white.withAlphaComponent(0.18).cgColor,
                NSColor.clear.cgColor
            ]
            refreshGlowBorder()
        }

        applyTextStyles()
        indicatorView.inkColor = inkColor(alpha: 218)
        indicatorView.trackColor = inkColor(alpha: 72)
    }

    /// C 深色镜面：主题色对角渐变描边（CAShapeLayer 描边作 mask 的 CAGradientLayer）。
    /// 尺寸未变时不重建（尺寸步进动画期间逐 tick 布局，避免每帧拆建图层）。
    private func refreshGlowBorder() {
        guard let container = glowBorderView.layer else { return }
        container.sublayers?.forEach { $0.removeFromSuperlayer() }
        let bounds = glowBorderView.bounds
        guard bounds.width > 4, bounds.height > 4 else { return }
        let inset: CGFloat = 1
        let path = CGPath(
            roundedRect: bounds.insetBy(dx: inset, dy: inset),
            cornerWidth: max(0, effectiveCornerRadius - inset),
            cornerHeight: max(0, effectiveCornerRadius - inset),
            transform: nil
        )
        let shape = CAShapeLayer()
        shape.path = path
        shape.lineWidth = 2
        shape.fillColor = NSColor.clear.cgColor
        shape.strokeColor = NSColor.black.cgColor // mask 只消费 alpha
        let gradient = CAGradientLayer()
        gradient.frame = bounds
        gradient.startPoint = CGPoint(x: 0, y: 1)
        gradient.endPoint = CGPoint(x: 1, y: 0)
        gradient.colors = resolvedTheme.glowColors
        gradient.mask = shape
        container.addSublayer(gradient)
    }

    /// 墨色：C 深色镜面恒用浅墨（深玻璃上深墨不可读，忽略主题墨色语义）。
    private func inkColor(alpha: Int) -> NSColor {
        let base = effectiveGlassStyle == .darkMirror ? ResolvedOverlayTheme.white.ink : resolvedTheme.ink
        return base.withAlphaComponent(CGFloat(alpha) / 255.0)
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
        // 文字阴影：黑色 72/255 下移 1（对齐 Windows kTextShadowAlpha + 1dp 偏移）；
        // C 深色镜面玻璃底更暗，阴影加浓带柔边保证白字可读。
        let darkMirror = effectiveGlassStyle == .darkMirror
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(darkMirror ? 0.55 : 72.0 / 255.0)
        shadow.shadowBlurRadius = darkMirror ? 3 : 0
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
