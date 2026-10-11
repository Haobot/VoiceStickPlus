import Foundation

/// macOS 文件日志：把 stderr（NSLog 的落点）重定向到
/// ~/Library/Application Support/VoiceStick/VoiceStickApp.log。
///
/// 为什么需要：统一日志（log show/Console）对第三方 app 的 NSLog 消息体做隐私脱敏，
/// 只能看到 "<private>"，无法事后取证耗时（2026-10-11 排查识别延迟时实证）；而 Windows
/// 端早有 VoiceStickApp.log（log.cc），AGENTS 的排障入口也一直指向它。此处以最小侵入
/// （进程入口 dup2 一次，不动任何 NSLog 调用点）补齐 macOS 侧同等能力。
///
/// 轮转：对齐 Windows C8 —— 单文件 8MB，超限挪 .old（只留一代历史），轮转失败则继续
/// 追加不动原文件。轮转只在启动时做一次（fd 已固定，运行中无法安全换文件）。
enum AppFileLog {
    private static let maxBytes: UInt64 = 8 * 1024 * 1024

    /// 进程入口调用一次；失败静默降级（日志仍进统一日志，只是脱敏）。
    static func redirectStderr() {
        let fm = FileManager.default
        let dir = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("VoiceStick", isDirectory: true)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let logURL = dir.appendingPathComponent("VoiceStickApp.log")

        if let attrs = try? fm.attributesOfItem(atPath: logURL.path),
           let size = attrs[.size] as? NSNumber,
           size.uint64Value > maxBytes {
            let oldURL = dir.appendingPathComponent("VoiceStickApp.log.old")
            try? fm.removeItem(at: oldURL)
            try? fm.moveItem(at: logURL, to: oldURL)
        }

        let fd = open(logURL.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
        guard fd >= 0 else { return }

        let fmt = ISO8601DateFormatter()
        let banner = "\n===== VoiceStickApp started \(fmt.string(from: Date())) pid=\(getpid()) =====\n"
        banner.withCString { cstr in
            _ = write(fd, cstr, strlen(cstr))
        }

        // NSLog 同时写 stderr 与统一日志；LaunchServices 启动时 stderr 指向 /dev/null，
        // dup2 后全部落到文件。stdout 一并重定向，兜住 Swift print。
        _ = dup2(fd, 1)
        _ = dup2(fd, 2)
        close(fd)
    }
}
