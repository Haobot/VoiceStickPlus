import AppKit

// @main 入口（替代顶层 main.swift）：SwiftPM 测试 target 依赖 executable target 的前提。
// 行为与原 main.swift 完全一致。
@main
struct VoiceStickMain {
    static func main() {
        // 文件日志先行：NSLog（stderr）落 VoiceStickApp.log，排障入口与 Windows 对齐。
        AppFileLog.redirectStderr()
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}
