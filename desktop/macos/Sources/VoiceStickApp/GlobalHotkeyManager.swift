import AppKit
import Carbon.HIToolbox

/// 全局热键（Carbon RegisterEventHotKey，对齐 Windows GlobalHotkeyWin）。
/// 配置串语法跨端兼容 Windows：修饰键 Alt→⌥、Win→⌘、Ctrl→⌃、Shift→⇧，
/// 主键为单字符/功能键名（如 "Alt+X"、"Ctrl+Shift+F5"）。
final class GlobalHotkeyManager {
    /// 预设热键（名称与 Windows kHotkeyPresets 一致，保证配置跨端可读）。
    static let presets: [String] = ["Alt+X", "Win+Alt+X", "Ctrl+Alt+X"]

    var onPressed: (() -> Void)?
    var onReleased: (() -> Void)?

    private(set) var registeredSpec: String?
    private var hotkeyRef: EventHotKeyRef?
    private var eventHandlerRef: EventHandlerRef?

    private static let hotkeyID = EventHotKeyID(signature: OSType(0x5653484B), id: 1) // 'VSHK'

    deinit {
        unregister()
    }

    /// 注册热键；冲突或语法非法时返回 false（调用侧提示冲突）。
    @discardableResult
    func register(spec: String) -> Bool {
        unregister()
        guard let binding = Self.binding(for: spec) else { return false }
        var pressedType = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        var releasedType = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyReleased))
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        let handler: EventHandlerUPP = { _, event, userData in
            guard let userData, let event else { return OSStatus(eventNotHandledErr) }
            let manager = Unmanaged<GlobalHotkeyManager>.fromOpaque(userData).takeUnretainedValue()
            var hotkeyID = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject),
                              EventParamType(typeEventHotKeyID), nil,
                              MemoryLayout<EventHotKeyID>.size, nil, &hotkeyID)
            guard hotkeyID.signature == GlobalHotkeyManager.hotkeyID.signature,
                  hotkeyID.id == GlobalHotkeyManager.hotkeyID.id else {
                return OSStatus(eventNotHandledErr)
            }
            let kind = GetEventKind(event)
            DispatchQueue.main.async {
                if kind == kEventHotKeyPressed {
                    manager.onPressed?()
                } else {
                    manager.onReleased?()
                }
            }
            return noErr
        }
        var types = [pressedType, releasedType]
        guard InstallEventHandler(GetApplicationEventTarget(), handler, 2, &types, selfPtr, &eventHandlerRef) == noErr else {
            return false
        }
        let status = RegisterEventHotKey(binding.keyCode, binding.modifiers, Self.hotkeyID,
                                         GetApplicationEventTarget(), 0, &hotkeyRef)
        guard status == noErr else {
            if let handlerRef = eventHandlerRef {
                RemoveEventHandler(handlerRef)
                self.eventHandlerRef = nil
            }
            return false
        }
        registeredSpec = spec
        return true
    }

    func unregister() {
        if let hotkeyRef {
            UnregisterEventHotKey(hotkeyRef)
            self.hotkeyRef = nil
        }
        if let eventHandlerRef {
            RemoveEventHandler(eventHandlerRef)
            self.eventHandlerRef = nil
        }
        registeredSpec = nil
    }

    /// 解析 "Ctrl+Alt+X" 风格绑定串为 Carbon keyCode/modifiers；非法返回 nil。
    static func binding(for spec: String) -> (keyCode: UInt32, modifiers: UInt32)? {
        var modifiers: UInt32 = 0
        var keyName = ""
        for part in spec.split(separator: "+").map({ $0.trimmingCharacters(in: .whitespaces) }) {
            switch part.lowercased() {
            case "alt", "option": modifiers |= UInt32(optionKey)
            case "win", "cmd", "command": modifiers |= UInt32(cmdKey)
            case "ctrl", "control": modifiers |= UInt32(controlKey)
            case "shift": modifiers |= UInt32(shiftKey)
            default: keyName = part
            }
        }
        guard modifiers != 0, !keyName.isEmpty, let keyCode = keyCode(for: keyName) else { return nil }
        return (keyCode, modifiers)
    }

    /// 绑定串的 macOS 显示名（"Alt+X" → "Option+X"）。
    static func displayName(for spec: String) -> String {
        spec.split(separator: "+").map { part in
            switch part.trimmingCharacters(in: .whitespaces).lowercased() {
            case "alt", "option": return "Option"
            case "win", "cmd", "command": return "Command"
            case "ctrl", "control": return "Control"
            case "shift": return "Shift"
            default: return part.uppercased()
            }
        }.joined(separator: "+")
    }

    /// NSEvent（捕获对话框用）→ 绑定串；无修饰键返回 nil。
    static func spec(for event: NSEvent) -> String? {
        var parts: [String] = []
        let flags = event.modifierFlags
        if flags.contains(.control) { parts.append("Ctrl") }
        if flags.contains(.option) { parts.append("Alt") }
        if flags.contains(.shift) { parts.append("Shift") }
        if flags.contains(.command) { parts.append("Win") }
        guard !parts.isEmpty, let name = keyName(for: event.keyCode) else { return nil }
        parts.append(name)
        return parts.joined(separator: "+")
    }

    private static func keyCode(for name: String) -> UInt32? {
        let table: [String: UInt32] = [
            "A": 0, "S": 1, "D": 2, "F": 3, "H": 4, "G": 5, "Z": 6, "X": 7, "C": 8, "V": 9,
            "B": 11, "Q": 12, "W": 13, "E": 14, "R": 15, "Y": 16, "T": 17,
            "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23, "9": 25, "7": 26, "8": 28, "0": 29,
            "O": 31, "U": 32, "I": 34, "P": 35, "RETURN": 36, "L": 37, "J": 38, "K": 41,
            "N": 46, "M": 47, "TAB": 48, "SPACE": 49, "DELETE": 51, "ESCAPE": 53,
            "F1": 122, "F2": 120, "F3": 99, "F4": 118, "F5": 96, "F6": 97,
            "F7": 98, "F8": 100, "F9": 101, "F10": 109, "F11": 103, "F12": 111,
            "LEFT": 123, "RIGHT": 124, "DOWN": 125, "UP": 126
        ]
        return table[name.uppercased()]
    }

    private static func keyName(for keyCode: UInt16) -> String? {
        let table: [UInt16: String] = [
            0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V",
            11: "B", 12: "Q", 13: "W", 14: "E", 15: "R", 16: "Y", 17: "T",
            18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5", 25: "9", 26: "7", 28: "8", 29: "0",
            31: "O", 32: "U", 34: "I", 35: "P", 36: "RETURN", 37: "L", 38: "J", 41: "K",
            46: "N", 47: "M", 48: "TAB", 49: "SPACE", 51: "DELETE", 53: "ESCAPE",
            122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
            98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12",
            123: "LEFT", 124: "RIGHT", 125: "DOWN", 126: "UP"
        ]
        return table[keyCode]
    }
}
