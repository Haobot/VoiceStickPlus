import AppKit
import Foundation

final class InputInjector {
    func paste(text: String, pressEnter: Bool) {
        guard !text.isEmpty else { return }

        let pasteboard = NSPasteboard.general
        let previousItems = pasteboard.pasteboardItems?.map(PasteboardItemSnapshot.init)

        pasteboard.clearContents()
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

            pasteboard.clearContents()
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

    private func sendReturn() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: false)
        keyDown?.flags = []
        keyUp?.flags = []
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
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
