import Foundation

/// gateway_keymap 路由表条目（set 命令与 gateway_keymap 报告帧共用；route 为
/// "software"/"passthrough"，用字符串承载避免引入路由枚举的反序列化分歧）。
public struct GatewayKeymapRoute: Codable, Equatable {
    public let key: String
    public let route: String

    public init(key: String, route: String) {
        self.key = key
        self.route = route
    }
}

/// 网关模式辅助（纯逻辑，供单测）：按键路由表推导与目标机名规整。
/// P2 macOS 网关适配，设计见 Doc/Plan/xiaomi-gateway-p2-macos.md。
public enum GatewaySupport {
    /// 协议允许软件路由的键全集（Doc/Ref/protocol.md gateway_keymap_set；语音键不参与
    /// 路由）。含 macOS 无映射 UI 的 volume_mute/power——也要显式推 passthrough，
    /// 避免继承上一目标（Windows）的 software 路由后键在此变死键。
    public static let routableKeys: [String] = [
        "ok", "right", "left", "down", "up", "menu", "home", "back",
        "volume_up", "volume_down", "volume_mute", "power", "tv",
    ]

    /// 由按键映射推导路由表（对齐 Windows PushGatewayKeymapRoutesFor 语义）：
    /// action=key → software（软件路由到桌面端注入）；action=disabled → software
    /// （路由到软件侧由桌面端吞掉——HOGP 直通会放行到 OS，达不成「禁用」语义）；
    /// action=native/未映射键 → passthrough。
    public static func routes(for settings: ButtonsSettings) -> [GatewayKeymapRoute] {
        routableKeys.map { key in
            let action = RemoteButton(rawValue: key).map { settings.mapping(for: $0).action }
            switch action {
            case .key, .disabled:
                return GatewayKeymapRoute(key: key, route: "software")
            default:
                return GatewayKeymapRoute(key: key, route: "passthrough")
            }
        }
    }

    /// gateway_target_info 主机名规整：去首尾空白；UTF-8 超过 23 字节按字节截断后
    /// 回退到字符边界（协议上限 23 字节，超长固件拒收）；空白串返回 nil（固件忽略
    /// 缺名/空名，桌面端跳过发送）。
    public static func targetInfoName(_ raw: String) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        var bytes = Array(trimmed.utf8)
        if bytes.count > 23 {
            bytes = Array(bytes.prefix(23))
        }
        // 截断可能把多字节字符切一半：从尾部回退到合法 UTF-8 边界。
        while !bytes.isEmpty && String(bytes: bytes, encoding: .utf8) == nil {
            bytes.removeLast()
        }
        guard !bytes.isEmpty else { return nil }
        return String(bytes: bytes, encoding: .utf8)
    }
}
