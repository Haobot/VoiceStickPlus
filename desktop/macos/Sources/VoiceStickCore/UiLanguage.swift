import Foundation

// N1 AppConfig 根刀 S1：UiLanguage 随根下迁（displayName 的 tr 留 App 扩展）。
// 纯 Locale 语义（effective / fromLocaleName / init(configValue:)）。

/// UI 界面语言（对齐 Windows UiLanguage；rawValue 即 config.toml `ui_language` 序列化值，
/// 跨端配置兼容，勿改字符串）。
public enum UiLanguage: String, CaseIterable {
    case system
    case en = "en"
    case zhHans = "zh-Hans"

    /// 设置窗语言下拉显示名（按当前界面语言；对齐 Windows kSettingsLanguage* 三选项）。

    /// 生效语言（对齐 Windows EffectiveUiLanguage）：system 时读系统首选语言，
    /// zh 前缀（zh/zh-*/zh_*）→ 简体中文，否则 → 英文。
    public var effective: UiLanguage {
        guard self == .system else { return self }
        return Self.fromLocaleName(Locale.preferredLanguages.first)
    }

    /// 对齐 Windows UiLanguageFromLocaleName。
    public static func fromLocaleName(_ name: String?) -> UiLanguage {
        guard let name else { return .en }
        let locale = name.lowercased()
        if locale == "zh" || locale.hasPrefix("zh-") || locale.hasPrefix("zh_") {
            return .zhHans
        }
        return .en
    }

    /// 对齐 Windows UiLanguageFromName：zh_CN/zh-CN/zh 兼容，非法值回 system。
    public init(configValue: String) {
        switch configValue {
        case "en":
            self = .en
        case "zh-Hans", "zh_CN", "zh-CN", "zh":
            self = .zhHans
        default:
            self = .system
        }
    }
}
