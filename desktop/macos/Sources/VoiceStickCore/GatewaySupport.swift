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

    /// 由按键映射推导路由表（macOS 网关，2026-10-12 语义修正）：
    /// - key/disabled → software（桌面注入映射键 / 吞键）；
    /// - 可映射键（RemoteButton 覆盖的 11 键）的 native 也 → software——macOS 没有
    ///   程序化 OS 配对 API（无 PairAsync 等价物），系统级 HID 会话建不起来（真机
    ///   实测 hidutil/ioreg 无 IOHIDDevice 节点），HOGP 直通到 OS 的「原生」路径在
    ///   mac 上等价于死键；改由桌面按 naturalSpec 注入等价自然键，语义仍是「原生」；
    /// - power/volume_mute（RemoteButton 不含、桌面无映射面）→ 保持 passthrough，
    ///   OS 级配对可用时仍直通，也避免推成 software 却无人消费被吞。
    public static func routes(for settings: ButtonsSettings) -> [GatewayKeymapRoute] {
        routableKeys.map { key in
            guard RemoteButton(rawValue: key) != nil else {
                return GatewayKeymapRoute(key: key, route: "passthrough")
            }
            // 可映射键一律软件路由：.key/.disabled 由映射消费，.native 走自然键注入。
            return GatewayKeymapRoute(key: key, route: "software")
        }
    }

    /// 「原生」语义的桌面等价实现（网关软件路由下替代 OS HID 直通）：可映射键 →
    /// 自然键 KeySpec；menu/tv 无桌面等价语义返回 nil（消费端日志提示去映射对话框
    /// 配置，图片界面有对应热区）。voice_double_click 恒经 ATVV 层（A 级），此处
    /// 仅补全语义（其原生行为=回车）。字符串与 KeySpec 语法同源，供单测断言。
    public static func naturalSpec(for button: RemoteButton) -> KeySpec? {
        switch button {
        case .ok: return KeySpec.parse("enter")
        case .up: return KeySpec.parse("up")
        case .down: return KeySpec.parse("down")
        case .left: return KeySpec.parse("left")
        case .right: return KeySpec.parse("right")
        case .back: return KeySpec.parse("backspace")
        case .home: return KeySpec.parse("home")
        case .volUp: return KeySpec.parse("volumeup")
        case .volDown: return KeySpec.parse("volumedown")
        case .voiceDoubleClick: return KeySpec.parse("enter")
        case .menu, .tv: return nil
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
