import Foundation

/// HID usage → 遥控器按钮 映射 + 拦截处置决策（小米遥控器按键自定义二期，macOS）。
/// 纯逻辑、无 IOKit/AppKit 依赖，放 VoiceStickCore 供单测（对齐 ButtonsSettings 分层）。
///
/// 背景（方案 Doc/Plan/xiaomi-remote-button-mapping.md §4.2）：对这款蓝牙键盘 HID 设备
/// 独占 seize 已被真机否决（0xe00002c1 NotPermitted），正式采用「非独占 IOHIDManager
/// 观察 + CGEvent tap 关联归因」。本类型只负责「usage → 按钮」与「按钮 → 处置」两件
/// 纯逻辑事，IOHID/tap 的薄封装在 app 层 XiaomiButtonInterceptManager。
///
/// usage 只按 16 位 usage id 匹配（对齐方案 §3.1 逆向键码表：payload 为最多 3 个
/// 16 位小端 usage，未带 usage page）。page 歧义在真机复测时再收窄——列为待验证项。
public enum RemoteButtonDecision: Equatable {
    /// 放行：OS 原生消费（intercept=false、action=native、或不可拦截键如电源）。
    case native
    /// 吞掉：吸掉该键不产生任何行为（action=disabled）。
    case suppress
    /// 注入：吸掉原键并注入目标 KeySpec 文本（action=key）。
    /// key 文本做不做语法校验——由运行时解析，空串/非法回落 native（放行原键）。
    case inject(key: String)
}

/// HID usage → RemoteButton 映射表与决策函数。
public enum RemoteButtonHIDMap {
    /// HID usage id → RemoteButton。
    /// 值来源（方案 §3.1 / Doc/Ref/protocol.md 键码表）：
    ///   0x28=ok, 0x4A=home, 0xF1=back(非标准), 0x65=menu, 0x35=tv,
    ///   0x4F/0x50=right/left, 0x51/0x52=down/up, 0x80/0x81=vol_up/vol_down。
    /// 不入表 → button(forUsage:) 返回 nil，处置一律 native：
    ///   0x66=power（C 级不可拦截）、0x7F=mute（预留）、其余未知 usage。
    public static let usageToButton: [UInt32: RemoteButton] = [
        0x28: .ok,
        0x4A: .home,
        0xF1: .back,
        0x65: .menu,
        0x35: .tv,
        0x4F: .right,
        0x50: .left,
        0x51: .down,
        0x52: .up,
        0x80: .volUp,
        0x81: .volDown,
    ]

    /// B 级可拦截键（排除 voiceDoubleClick——该键走 ATVV 会话层，无 HID 暴露，一期已生效）。
    public static let interceptableButtons: [RemoteButton] =
        RemoteButton.allCases.filter { $0.requiresIntercept }

    /// HID usage id → RemoteButton；未映射（power/mute/未知）返回 nil。
    public static func button(forUsage usage: UInt32) -> RemoteButton? {
        usageToButton[usage]
    }

    /// usage → macOS virtual keycode（仅 macOS HID→CGEvent 翻译层实际会产生 keyDown
    /// 的 usage 入表；真机实测：0xF1(back) 不产生 keyDown，0x80/0x81(vol) 走
    /// systemDefined 而非 keyDown，0x65(menu) 未见 keyDown）。IOHID 驱动处置后
    /// CGEvent tap 按此表反映射匹配要吞的原生 keyDown（二期 macOS 拦截重构）。
    public static let usageToMacKeyCode: [UInt32: Int64] = [
        0x28: 0x24,  // ok → kVK_Return
        0x4A: 0x73,  // home → kVK_Home
        0x4F: 0x7C,  // right → kVK_RightArrow
        0x50: 0x7B,  // left → kVK_LeftArrow
        0x51: 0x7D,  // down → kVK_DownArrow
        0x52: 0x7E,  // up → kVK_UpArrow
        0x35: 0x32,  // tv → kVK_ANSI_Grave（keyboard page 0x35 = `，macOS 忠实翻译）
    ]

    /// usage → macOS virtual keycode；macOS 不产生 keyDown 的 usage 返回 nil。
    public static func macKeyCode(forUsage usage: UInt32) -> Int64? {
        usageToMacKeyCode[usage]
    }

    /// 音量键的 systemDefined 键码（NX_KEYTYPE_SOUND_UP/DOWN；data1 高 16 位）
    /// ↔ usage 映射。音量键的原生事件形态是 subtype=8 的 NSSystemDefined。
    public static let volumeSystemKeyToUsage: [Int64: UInt32] = [
        0: 0x80,  // NX_KEYTYPE_SOUND_UP → vol_up
        1: 0x81,  // NX_KEYTYPE_SOUND_DOWN → vol_down
    ]

    /// 依据拦截总开关与单键映射产出处置：
    ///   - intercept=false → 一律 native（HID 拦截是 B 级键生效前提，未开启即原生透传）；
    ///   - action=native → native；disabled → suppress；key → inject(key)。
    ///   - inject 的 key 文本不做语法校验（运行时解析，失败回落 native）。
    public static func decision(intercept: Bool, mapping: ButtonMapping) -> RemoteButtonDecision {
        guard intercept else { return .native }
        switch mapping.action {
        case .native: return .native
        case .disabled: return .suppress
        case .key: return .inject(key: mapping.key)
        }
    }
}
