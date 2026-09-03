import Foundation

final class DebugAudioRecorder {
    private let enabled: Bool
    private let directory: URL
    private var currentDeviceID: String?
    private var currentDevicePrefix = "VS-"
    private var currentSessionID: UInt32?
    private var currentStartedAt: Date?
    private var currentAudio = Data()

    init(enabled: Bool, directory: URL) {
        self.enabled = enabled
        self.directory = directory
    }

    /// devicePrefix 按设备类参数化文件名前缀（StickS3 为 "VS-"，小米遥控器为 "RC-"）。
    func start(deviceID: String?, sessionID: UInt32?, devicePrefix: String = "VS-") {
        guard enabled else { return }
        currentDeviceID = deviceID
        currentDevicePrefix = devicePrefix
        currentSessionID = sessionID
        currentStartedAt = Date()
        currentAudio.removeAll(keepingCapacity: true)
    }

    func append(_ data: Data) {
        guard enabled, !data.isEmpty else { return }
        currentAudio.append(data)
    }

    func finish() {
        guard enabled, !currentAudio.isEmpty else {
            reset()
            return
        }

        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let fileURL = directory.appendingPathComponent(fileName(), isDirectory: false)
            try currentAudio.write(to: fileURL, options: .atomic)
            NSLog("Debug audio saved: \(fileURL.path)")
        } catch {
            NSLog("Debug audio save failed: \(error.localizedDescription)")
        }

        reset()
    }

    func discard() {
        reset()
    }

    private func reset() {
        currentDeviceID = nil
        currentDevicePrefix = "VS-"
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
        return "\(timestamp)-\(device)-session-\(session).ogg"
    }
}
