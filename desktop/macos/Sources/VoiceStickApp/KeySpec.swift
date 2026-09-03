import CoreGraphics
import Foundation

/// 一次按键注入的规格（对齐 Windows key_spec.h 的 KeySpec/ParseKeySpec）：
/// 修饰键（Ctrl/Alt/Shift/Win 固定序；macOS 上映射 ⌃/⌥/⇧/⌘）+ 主键 + 规范化显示文本
/// （如 "Ctrl+Shift+V"）。供编码器旋转/单击/双击的自定义按键注入使用。
///
/// 语法：单键（up/down/left/right/enter|return/esc|escape/tab/space/backspace/delete/
/// insert/pageup/pagedown/home/end/volumeup/volumedown/volumemute/f1-f24/单字符 A-Z0-9）
/// 或修饰键组合（ctrl|control/alt/shift/win|windows|meta + 单键，"+" 分隔，
/// 大小写与前后空白不敏感）。仅修饰键、未知键名、多个主键均解析失败。
struct KeySpec: Equatable {
    /// 音量键在 macOS 上没有虚拟键码，走 NX_SYSDEFINED 媒体键事件。
    enum MediaKey: Equatable {
        case volumeUp
        case volumeDown
        case volumeMute
    }

    var control = false
    var option = false   // Alt
    var shift = false
    var command = false  // Win
    /// 普通键的 CGKeyCode；媒体键为 nil（看 mediaKey）。
    /// f21-f24 语法合法但 macOS 无对应虚拟键码：解析放行（配置跨端兼容），keyCode 为 nil，注入侧忽略。
    let keyCode: CGKeyCode?
    let mediaKey: MediaKey?
    let displayText: String

    static func parse(_ text: String) -> KeySpec? {
        var modifiers: (ctrl: Bool, alt: Bool, shift: Bool, win: Bool) = (false, false, false, false)
        var main: (keyCode: CGKeyCode?, mediaKey: MediaKey?, display: String)?

        for rawPart in text.split(separator: "+", omittingEmptySubsequences: false) {
            let part = rawPart.trimmingCharacters(in: .whitespaces).lowercased()
            if part.isEmpty { return nil }

            switch part {
            case "ctrl", "control":
                if modifiers.ctrl { return nil }
                modifiers.ctrl = true
            case "alt":
                if modifiers.alt { return nil }
                modifiers.alt = true
            case "shift":
                if modifiers.shift { return nil }
                modifiers.shift = true
            case "win", "windows", "meta":
                if modifiers.win { return nil }
                modifiers.win = true
            default:
                if main != nil { return nil }  // 多个主键
                guard let resolved = resolveMainKey(part) else { return nil }
                main = resolved
            }
        }
        guard let main else { return nil }  // 仅修饰键

        var display = ""
        if modifiers.ctrl { display += "Ctrl+" }
        if modifiers.alt { display += "Alt+" }
        if modifiers.shift { display += "Shift+" }
        if modifiers.win { display += "Win+" }
        display += main.display

        return KeySpec(
            control: modifiers.ctrl,
            option: modifiers.alt,
            shift: modifiers.shift,
            command: modifiers.win,
            keyCode: main.keyCode,
            mediaKey: main.mediaKey,
            displayText: display
        )
    }

    /// 主键名 → 键码/媒体键 + 显示名（对齐 Windows MainKeyVkey + VkeyDisplayName）。
    private static func resolveMainKey(_ lower: String) -> (CGKeyCode?, MediaKey?, String)? {
        if lower.count == 1, let char = lower.uppercased().first,
           let code = alphanumericKeyCodes[char] {
            return (code, nil, String(char))
        }
        switch lower {
        case "volumeup": return (nil, .volumeUp, "VolumeUp")
        case "volumedown": return (nil, .volumeDown, "VolumeDown")
        case "volumemute": return (nil, .volumeMute, "VolumeMute")
        default: break
        }
        if let (code, name) = namedKeys[lower] {
            return (code, nil, name)
        }
        if lower.hasPrefix("f"), lower.count >= 2 {
            let digits = lower.dropFirst()
            guard digits.allSatisfy({ $0.isNumber }), let num = Int(digits), num >= 1, num <= 24 else {
                return nil
            }
            return (functionKeyCodes[num], nil, "F\(num)")
        }
        return nil
    }

    // MARK: - 键码表（ANSI 布局）

    private static let alphanumericKeyCodes: [Character: CGKeyCode] = [
        "A": 0, "S": 1, "D": 2, "F": 3, "H": 4, "G": 5, "Z": 6, "X": 7,
        "C": 8, "V": 9, "B": 11, "Q": 12, "W": 13, "E": 14, "R": 15,
        "Y": 16, "T": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
        "5": 23, "9": 25, "7": 26, "8": 28, "0": 29, "O": 31, "U": 32,
        "I": 34, "P": 35, "L": 37, "J": 38, "K": 40, "N": 45, "M": 46
    ]

    private static let namedKeys: [String: (CGKeyCode, String)] = [
        "space": (49, "Space"),
        "enter": (36, "Enter"), "return": (36, "Enter"),
        "esc": (53, "Esc"), "escape": (53, "Esc"),
        "tab": (48, "Tab"),
        "backspace": (51, "Backspace"),
        "delete": (117, "Delete"),   // 前进删除（Windows VK_DELETE 语义）
        "insert": (114, "Insert"),   // kVK_Help，PC 键盘上即 Insert
        "up": (126, "Up"), "down": (125, "Down"),
        "left": (123, "Left"), "right": (124, "Right"),
        "pageup": (116, "PageUp"), "pagedown": (121, "PageDown"),
        "home": (115, "Home"), "end": (119, "End")
    ]

    private static let functionKeyCodes: [Int: CGKeyCode] = [
        1: 122, 2: 120, 3: 99, 4: 118, 5: 96, 6: 97, 7: 98, 8: 100,
        9: 101, 10: 109, 11: 103, 12: 111, 13: 105, 14: 107, 15: 113,
        16: 106, 17: 64, 18: 79, 19: 80, 20: 90
    ]
}
