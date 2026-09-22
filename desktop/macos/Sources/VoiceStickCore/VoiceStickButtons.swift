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
    /// HID 拦截总开关（B 级键生效前提，未开启即原生透传；A 级语音键双击不受它门控）。
    public var intercept: Bool
    /// 已配置的按键映射（键为 RemoteButton.rawValue）；未配置的键回落 native。
    public var mappings: [String: ButtonMapping]

    public static let `default` = ButtonsSettings(intercept: false, mappings: [:])

    public init(intercept: Bool = false, mappings: [String: ButtonMapping] = [:]) {
        self.intercept = intercept
        self.mappings = mappings
    }

    public func mapping(for button: RemoteButton) -> ButtonMapping {
        mappings[button.rawValue] ?? .native
    }

    public mutating func setMapping(_ mapping: ButtonMapping, for button: RemoteButton) {
        mappings[button.rawValue] = mapping
    }
}
