import Foundation

// N1 切5 闸6b：三组 UI 出口协议——InputInjector/SubtitleController/DebugAudioRecorder
// 的实例经 AppDelegate 注入（具体类与 CGEvent/AV 等框架全留 App 合规）。
// 省略尾参（start 的 devicePrefix 默认）由协议 extension 重载承接。

/// 输入注入出口（对齐 InputInjector 8 面；KeySpec 在 Core）。
public protocol VoiceStickInputServing: AnyObject {
    var onAccessibilityPermissionMissing: (() -> Void)? { get set }
    func paste(text: String, pressEnter: Bool)
    func clickLeftButton()
    func moveMouse(dx: Int, dy: Int)
    func sendArrowDown()
    func sendArrowUp()
    func sendEnter()
    func sendKeyCombo(_ spec: KeySpec)
}

/// 字幕覆盖层出口（对齐 SubtitleController 2 面；OverlayThemeColor 在 Core）。
public protocol VoiceStickSubtitleServing: AnyObject {
    func show(text: String, deviceID: String, color: OverlayThemeColor)
    func hideAll()
}

/// 调试录音出口（对齐 DebugAudioRecorder 2 面）。
/// 新增面（音频质量实验台 2026-10-31）带默认实现，既有 conformance 不破：
/// 带后缀/指定起始时间的 start、写侧车的 finish、finalText 补写。
public protocol VoiceStickDebugRecordServing: AnyObject {
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String)
    func append(_ data: Data)
    func finish()
    func discard()
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String,
               filenameSuffix: String, startedAt: Date?)
    func finish(sidecar: DebugAudioSidecar?)
    func updateLastSidecar(finalText: String)
}

public extension VoiceStickDebugRecordServing {
    func start(deviceID: String?, sessionID: UInt32?) {
        start(deviceID: deviceID, sessionID: sessionID, devicePrefix: "VS-")
    }

    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String,
               filenameSuffix: String, startedAt: Date?) {
        start(deviceID: deviceID, sessionID: sessionID, devicePrefix: devicePrefix)
    }

    func finish(sidecar: DebugAudioSidecar?) { finish() }

    func updateLastSidecar(finalText: String) {}
}
