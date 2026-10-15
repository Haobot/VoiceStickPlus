import Foundation

/// 小米遥控器可映射按钮（对齐 Windows kXiaomiMappableButtons 与
/// Doc/Plan/xiaomi-remote-button-mapping.md 的动作模型）。
/// rawValue 与协议键名/TOML 键名一致（`Doc/Ref/protocol.md` gateway_keymap 键表）。
public enum RemoteButton: String, CaseIterable, Codable, Equatable {
    case ok
    case home
    case back
    case menu
    case tv
    case right
    case left
    case down
    case up
    case volUp = "volume_up"
    case volDown = "volume_down"
    /// 语音键双击（A 级）：走 ATVV 会话层合成 button_double_click，无 HID 暴露，
    /// 不参与 HID 拦截（一期已生效，见 RemoteButtonHIDMap.interceptableButtons 注释）。
    case voiceDoubleClick = "voice_double_click"

    /// 是否属于 B 级（HID 拦截层处置的键）；语音键双击走 ATVV 层，恒 false。
    public var requiresIntercept: Bool { self != .voiceDoubleClick }

    /// 是否参与长按连发（inject 动作按住重复注入，对齐系统键盘自动重复）：
    /// 方向/返回（常映射删除）/音量需要；ok/home/menu/tv 连发无意义（回车/主屏
    /// 连击有害无益）；语音键走 ATVV 层不经 HID 拦截，天然不参与。
    public var supportsRepeat: Bool {
        switch self {
        case .up, .down, .left, .right, .back, .volUp, .volDown:
            return true
        case .ok, .home, .menu, .tv, .voiceDoubleClick:
            return false
        }
    }
}

/// 单键动作（对齐方案动作词汇 v1：原生 / 禁用 / 键盘按键）。
public enum ButtonMappingAction: String, Codable, Equatable, CaseIterable {
    case native
    case disabled
    case key
}

/// 单键映射配置：action=key 时 key 为 KeySpec 文本（KeySpec.parse 消费）。
public struct ButtonMapping: Codable, Equatable {
    public var action: ButtonMappingAction
    public var key: String

    public init(action: ButtonMappingAction, key: String) {
        self.action = action
        self.key = key
    }

    public static let native = ButtonMapping(action: .native, key: "")
}

/// 小米遥控器按键映射设置（[device.<id>.buttons] 反序列化产物；全局默认即
/// `default`：拦截关闭、全部原生）。
public struct ButtonsSettings: Codable, Equatable {
    /// 长按连发默认节拍（对齐系统键盘手感：首次延迟 300ms、重复间隔 80ms ≈ 12.5 次/秒）。
    public static let defaultRepeatDelayMs = 300
    public static let defaultRepeatIntervalMs = 80
    /// 节拍合法区间；解析/构造越界回落默认值（对齐 AirMouseSettings.applyClamps）。
    public static let repeatDelayRange = 100...1000
    public static let repeatIntervalRange = 30...500

    /// HID 拦截总开关（B 级键生效前提，未开启即原生透传；A 级语音键双击不受它门控）。
    public var intercept: Bool
    /// 已配置的按键映射（键为 RemoteButton.rawValue）；未配置的键回落 native。
    public var mappings: [String: ButtonMapping]
    /// 长按连发：首次重复延迟与重复间隔（毫秒）。作用于 supportsRepeat 按钮的
    /// inject 动作（macOS HID 拦截层消费；网关键沿与语音键不适用）。
    public var repeatDelayMs: Int
    public var repeatIntervalMs: Int

    public static let `default` = ButtonsSettings()

    public init(intercept: Bool = false,
                mappings: [String: ButtonMapping] = [:],
                repeatDelayMs: Int = ButtonsSettings.defaultRepeatDelayMs,
                repeatIntervalMs: Int = ButtonsSettings.defaultRepeatIntervalMs) {
        self.intercept = intercept
        self.mappings = mappings
        self.repeatDelayMs = ButtonsSettings.repeatDelayRange.contains(repeatDelayMs)
            ? repeatDelayMs : ButtonsSettings.defaultRepeatDelayMs
        self.repeatIntervalMs = ButtonsSettings.repeatIntervalRange.contains(repeatIntervalMs)
            ? repeatIntervalMs : ButtonsSettings.defaultRepeatIntervalMs
    }

    public func mapping(for button: RemoteButton) -> ButtonMapping {
        mappings[button.rawValue] ?? .native
    }

    public mutating func setMapping(_ mapping: ButtonMapping, for button: RemoteButton) {
        mappings[button.rawValue] = mapping
    }
}

public extension ButtonsSettings {
    /// 解码宽容：旧配置/旧序列化无 repeat_* 键时回落默认（synthesized Decodable
    /// 要求键在场的坑，同 XiaomiConfigFile 手写解码先例）。
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            intercept: try container.decodeIfPresent(Bool.self, forKey: .intercept) ?? false,
            mappings: try container.decodeIfPresent([String: ButtonMapping].self, forKey: .mappings) ?? [:],
            repeatDelayMs: try container.decodeIfPresent(Int.self, forKey: .repeatDelayMs)
                ?? ButtonsSettings.defaultRepeatDelayMs,
            repeatIntervalMs: try container.decodeIfPresent(Int.self, forKey: .repeatIntervalMs)
                ?? ButtonsSettings.defaultRepeatIntervalMs
        )
    }
}
