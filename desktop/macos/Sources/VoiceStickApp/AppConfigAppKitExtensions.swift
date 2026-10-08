import AppKit
import Foundation
import VoiceStickCore

// N1 AppConfig 根刀 S2：打开目录两法（NSWorkspace=AppKit）留 App 扩展——
// 根结构下迁 Core 后 App 侧扩展依旧承载平台交互。

extension AppConfig {
    static func openConfigDirectory() {
        try? FileManager.default.createDirectory(at: configDirectory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(configDirectory)
    }

    static func openDebugAudioDirectory(_ directory: URL = defaultDebugAudioDirectory) {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(directory)
    }
}
