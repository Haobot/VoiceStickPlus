import Foundation
import VoiceStickCore

/// DebugAudio 会话记录器：纯内存累积 + 结束时一次性写盘。
///
/// 文件名 `yyyyMMdd-HHmmss-{VS-|VG-|RC-<devID>}-session-<N>[suffix].ogg`；
/// 前缀按会话来源（`AudioSessionSource.filenamePrefix`，VS-/VG-/RC- = Stick 麦/
/// 网关中继/遥控器直连）。`finish(sidecar:)` 另写同名 `.json` 侧车（传输层
/// 指标 + 来源 + 可后补的识别文本），供 scripts/e2e_test/audio_lab.py 消费。
final class DebugAudioRecorder {
    private let enabled: Bool
    private let directory: URL
    private var currentDeviceID: String?
    private var currentDevicePrefix = "VS-"
    private var currentFilenameSuffix = ""
    private var currentSessionID: UInt32?
    private var currentStartedAt: Date?
    private var currentAudio = Data()
    /// 最近一次成功落盘的侧车路径（updateLastSidecar 用）。
    private var lastSidecarURL: URL?

    init(enabled: Bool, directory: URL) {
        self.enabled = enabled
        self.directory = directory
    }

    /// devicePrefix 按会话来源参数化（VS-/VG-/RC-，见 AudioSessionSource）。
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String = "VS-") {
        start(deviceID: deviceID, sessionID: sessionID, devicePrefix: devicePrefix,
              filenameSuffix: "", startedAt: nil)
    }

    /// filenameSuffix 插在扩展名前（如 ".raw" → `…-session-3.raw.ogg`）；
    /// startedAt 允许配对记录器（增强前/后双份）共享同一时间戳基名。
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String,
               filenameSuffix: String, startedAt: Date?) {
        guard enabled else { return }
        currentDeviceID = deviceID
        currentDevicePrefix = devicePrefix
        currentFilenameSuffix = filenameSuffix
        currentSessionID = sessionID
        currentStartedAt = startedAt ?? Date()
        currentAudio.removeAll(keepingCapacity: true)
    }

    func append(_ data: Data) {
        guard enabled, !data.isEmpty else { return }
        currentAudio.append(data)
    }

    func finish() {
        finish(sidecar: nil)
    }

    func finish(sidecar: DebugAudioSidecar?) {
        guard enabled, !currentAudio.isEmpty else {
            reset()
            return
        }

        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let fileURL = directory.appendingPathComponent(fileName(), isDirectory: false)
            try currentAudio.write(to: fileURL, options: .atomic)
            NSLog("Debug audio saved: \(fileURL.path)")
            if let sidecar {
                let sidecarURL = URL(fileURLWithPath: (fileURL.path as NSString)
                    .deletingPathExtension + ".json")
                if sidecar.write(to: sidecarURL) {
                    lastSidecarURL = sidecarURL
                } else {
                    NSLog("Debug audio sidecar write failed: \(sidecarURL.path)")
                }
            }
        } catch {
            NSLog("Debug audio save failed: \(error.localizedDescription)")
        }

        reset()
    }

    /// ASR 完成后补写现场识别文本到最近一次落盘的侧车（无侧车即忽略）。
    func updateLastSidecar(finalText: String) {
        guard enabled, let url = lastSidecarURL else { return }
        if !DebugAudioSidecar.updateFinalText(finalText, at: url) {
            NSLog("Debug audio sidecar finalText update failed: \(url.path)")
        }
    }

    func discard() {
        reset()
    }

    private func reset() {
        currentDeviceID = nil
        currentDevicePrefix = "VS-"
        currentFilenameSuffix = ""
        currentSessionID = nil
        currentStartedAt = nil
        currentAudio.removeAll(keepingCapacity: false)
    }

    private func fileName() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let timestamp = formatter.string(from: currentStartedAt ?? Date())

        let device = currentDeviceID.map { "\(currentDevicePrefix)\($0)" } ?? "unknown-device"
        let session = currentSessionID.map(String.init) ?? "unknown"
        return "\(timestamp)-\(device)-session-\(session)\(currentFilenameSuffix).ogg"
    }
}
