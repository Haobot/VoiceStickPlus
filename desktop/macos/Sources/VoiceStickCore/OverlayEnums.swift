// N1 闸6b：Overlay 三枚举（主题色/尺寸/位置）随状态出口链下沉 Core——
// displayName（tr 本地化）留 App 扩展（DomainDisplayNames.swift）。

public enum OverlayThemeColor: String, CaseIterable {
    case auto
    case white
    case black
    case pink
    case green
    case yellow
    case blue
    case purple

}

/// 悬浮窗主题大小（对齐 Windows OverlayThemeSize，默认 big）。
public enum OverlayThemeSize: String, CaseIterable {
    case big
    case medium
    case small

}

/// 悬浮窗位置（对齐 Windows OverlayPosition，默认 bottom_center）。
public enum OverlayPosition: String, CaseIterable {
    case center
    case bottomCenter = "bottom_center"
    case topLeft = "top_left"
    case topRight = "top_right"
    case bottomLeft = "bottom_left"
    case bottomRight = "bottom_right"

}

