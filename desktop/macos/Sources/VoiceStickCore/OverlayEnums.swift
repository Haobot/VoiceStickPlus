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

/// 悬浮窗背景玻璃方案（液态玻璃实验三选一：全局选项，不按设备）。实验期存
/// UserDefaults（key `overlay.glassStyle`），用户定稿后迁 config.toml 并同步 Windows。
public enum OverlayGlassStyle: String, CaseIterable {
    /// A：系统原生液态玻璃（NSGlassEffectView，macOS 26+；低版本自动回退 specular）。
    case nativeLiquid = "native_liquid"
    /// B：高光玻璃（popover 材质 + 顶部镜面高光 + 双层描边 + tint 增强 + 柔和投影）。
    case specular
    /// C：深色镜面玻璃（强制深色玻璃 + 主题色渐变辉光描边 + 恒浅墨色，控制中心风格）。
    case darkMirror = "dark_mirror"
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

