import AppKit
import Foundation

final class InputInjector {
    func paste(text: String, pressEnter: Bool) {
        guard !text.isEmpty else { return }

        let pasteboard = NSPasteboard.general
        let previousItems = pasteboard.pasteboardItems?.map(PasteboardItemSnapshot.init)

        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.setString(text, forType: .string)
        let temporaryChangeCount = pasteboard.changeCount
        sendCommandV()

        if pressEnter {
            NSLog("InputInjector auto_enter")
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                self.releaseCommandKey()
                self.sendReturn()
            }
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            guard pasteboard.changeCount == temporaryChangeCount else { return }

            pasteboard.prepareForNewContents(with: .currentHostOnly)
            let restoredItems = previousItems?.map(\.pasteboardItem) ?? []
            if !restoredItems.isEmpty {
                pasteboard.writeObjects(restoredItems)
            }
        }
    }

    private func sendCommandV() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandDown = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: true)
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false)
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandDown?.flags = .maskCommand
        keyDown?.flags = .maskCommand
        keyUp?.flags = .maskCommand
        commandUp?.flags = []
        commandDown?.post(tap: .cghidEventTap)
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
        commandUp?.post(tap: .cghidEventTap)
    }

    private func releaseCommandKey() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandUp?.flags = []
        commandUp?.post(tap: .cghidEventTap)
    }

    func sendEnter() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: false)
        keyDown?.flags = []
        keyUp?.flags = []
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
    }

    /// 注入一组按键（对齐 Windows InputInjector::SendKeyCombo）：修饰键 + 主键一次点按。
    /// 音量键走 NX_SYSDEFINED 媒体键事件；f21-f24 无 macOS 键码，忽略注入。
    func sendKeyCombo(_ spec: KeySpec) {
        if let mediaKey = spec.mediaKey {
            sendMediaKey(mediaKey)
            return
        }
        guard let keyCode = spec.keyCode else {
            NSLog("InputInjector key without macOS keycode ignored: \(spec.displayText)")
            return
        }

        var flags = CGEventFlags()
        if spec.control { flags.insert(.maskControl) }
        if spec.option { flags.insert(.maskAlternate) }
        if spec.shift { flags.insert(.maskShift) }
        if spec.command { flags.insert(.maskCommand) }

        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false)
        keyDown?.flags = flags
        keyUp?.flags = flags
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
    }

    func sendArrowUp() {
        sendKeyCombo(KeySpec(keyCode: 126, mediaKey: nil, displayText: "Up"))
    }

    func sendArrowDown() {
        sendKeyCombo(KeySpec(keyCode: 125, mediaKey: nil, displayText: "Down"))
    }

    /// 媒体键（音量增/减/静音）：NSEvent systemDefined，subtype 8，data1 = keyCode<<16 | down<<8。
    /// keyCode：0=音量增，1=音量减，7=静音。
    private func sendMediaKey(_ key: KeySpec.MediaKey) {
        let rawCode: Int
        switch key {
        case .volumeUp: rawCode = 0
        case .volumeDown: rawCode = 1
        case .volumeMute: rawCode = 7
        }
        for keyDown in [true, false] {
            let data1 = (rawCode << 16) | ((keyDown ? 0x0A : 0x0B) << 8)
            if let event = NSEvent.otherEvent(
                with: .systemDefined,
                location: .zero,
                modifierFlags: [],
                timestamp: 0,
                windowNumber: 0,
                context: nil,
                subtype: 8,
                data1: data1,
                data2: -1
            ) {
                event.cgEvent?.post(tap: .cghidEventTap)
            }
        }
    }

    private func sendReturn() {
        sendEnter()
    }
}

private struct PasteboardItemSnapshot {
    private let contents: [(type: NSPasteboard.PasteboardType, data: Data)]

    init(item: NSPasteboardItem) {
        contents = item.types.compactMap { type in
            guard let data = item.data(forType: type) else { return nil }
            return (type, data)
        }
    }

    var pasteboardItem: NSPasteboardItem {
        let item = NSPasteboardItem()
        for content in contents {
            item.setData(content.data, forType: content.type)
        }
        return item
    }
}
