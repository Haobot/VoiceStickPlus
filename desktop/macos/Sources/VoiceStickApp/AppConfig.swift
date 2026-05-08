import AppKit
import Foundation

enum ASRProvider: String {
    case voiceStickCloud = "voicestick_cloud"
    case volcengine

    var displayName: String {
        switch self {
        case .voiceStickCloud:
            return "VoiceStick Cloud"
        case .volcengine:
            return "Volcengine"
        }
    }
}

struct AppConfig {
    var asrProvider: ASRProvider
    var voiceStickAPIKey: String
    var voiceStickCloudURL: String
    var volcengineAPIKey: String
    var resourceID: String
    var pairedDeviceIDs: [String]
    var autoEnter: Bool
    var debugAudioCache: Bool
    var debugAudioDirectory: URL

    static var configDirectory: URL {
        FileManager.default
            .homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VoiceStick", isDirectory: true)
    }

    static var configURL: URL {
        configDirectory.appendingPathComponent("config.toml")
    }

    static var defaultDebugAudioDirectory: URL {
        configDirectory.appendingPathComponent("DebugAudio", isDirectory: true)
    }

    static let supportedResourceIDs = [
        "volc.seedasr.sauc.duration",
        "volc.seedasr.sauc.concurrent",
        "volc.bigasr.sauc.duration",
        "volc.bigasr.sauc.concurrent"
    ]

    static let defaultVoiceStickCloudURL = "wss://api.xiaozhi.me/voicestick/asr/"
    static let volcengineWebSocketURL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
    static let websiteURL = URL(string: "https://78.github.io/voicestick/")!
    static let firmwareManifestURL = URL(
        string: "https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json"
    )!
    static let minimumCompatibleFirmwareVersion = "0.2.6"

    static var configExists: Bool {
        FileManager.default.fileExists(atPath: configURL.path)
    }

    static var defaults: AppConfig {
        AppConfig(
            asrProvider: .volcengine,
            voiceStickAPIKey: "",
            voiceStickCloudURL: defaultVoiceStickCloudURL,
            volcengineAPIKey: "",
            resourceID: supportedResourceIDs[0],
            pairedDeviceIDs: [],
            autoEnter: true,
            debugAudioCache: false,
            debugAudioDirectory: defaultDebugAudioDirectory
        )
    }

    static func load() -> AppConfig {
        let defaults = Self.defaults

        guard let text = try? String(contentsOf: configURL) else {
            return defaults
        }

        var values: [String: String] = [:]
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#") else { continue }
            let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            values[parts[0].trimmingCharacters(in: .whitespaces)] =
                parts[1].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
        }

        return AppConfig(
            asrProvider: asrProviderValue(values["asr_provider"], default: defaults.asrProvider),
            voiceStickAPIKey: values["voicestick_api_key"] ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: values["voicestick_cloud_url"] ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: values["volcengine_api_key"] ?? values["api_key"] ?? defaults.volcengineAPIKey,
            resourceID: resourceIDValue(values["resource_id"], default: defaults.resourceID),
            pairedDeviceIDs: deviceIDList(values["paired_device_ids"] ?? ""),
            autoEnter: boolValue(values["auto_enter"], default: defaults.autoEnter),
            debugAudioCache: boolValue(values["debug_audio_cache"], default: defaults.debugAudioCache),
            debugAudioDirectory: directoryValue(values["debug_audio_dir"], default: defaults.debugAudioDirectory)
        )
    }

    func save() throws {
        try FileManager.default.createDirectory(at: Self.configDirectory, withIntermediateDirectories: true)
        let text = """
        asr_provider = "\(asrProvider.rawValue)"
        voicestick_api_key = "\(voiceStickAPIKey.tomlEscaped)"
        voicestick_cloud_url = "\(voiceStickCloudURL.tomlEscaped)"
        volcengine_api_key = "\(volcengineAPIKey.tomlEscaped)"
        resource_id = "\(resourceID.tomlEscaped)"
        paired_device_ids = "\(pairedDeviceIDs.joined(separator: ",").tomlEscaped)"
        auto_enter = \(autoEnter.tomlValue)
        debug_audio_cache = \(debugAudioCache.tomlValue)
        debug_audio_dir = "\(debugAudioDirectory.path.tomlEscaped)"
        """
        try text.write(to: Self.configURL, atomically: true, encoding: .utf8)
    }

    static func openConfigDirectory() {
        try? FileManager.default.createDirectory(at: configDirectory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(configDirectory)
    }

    static func openDebugAudioDirectory(_ directory: URL = defaultDebugAudioDirectory) {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        NSWorkspace.shared.open(directory)
    }

    private static func boolValue(_ text: String?, default defaultValue: Bool) -> Bool {
        guard let text else { return defaultValue }
        switch text.lowercased() {
        case "true", "yes", "1", "on":
            return true
        case "false", "no", "0", "off":
            return false
        default:
            return defaultValue
        }
    }

    private static func directoryValue(_ text: String?, default defaultValue: URL) -> URL {
        guard let text, !text.isEmpty else { return defaultValue }
        let expanded = (text as NSString).expandingTildeInPath
        return URL(fileURLWithPath: expanded, isDirectory: true)
    }

    private static func asrProviderValue(_ text: String?, default defaultValue: ASRProvider) -> ASRProvider {
        guard let text, let provider = ASRProvider(rawValue: text) else { return defaultValue }
        return provider
    }

    private static func resourceIDValue(_ text: String?, default defaultValue: String) -> String {
        guard let text, supportedResourceIDs.contains(text) else { return defaultValue }
        return text
    }

    static func normalizedDeviceID(_ text: String) -> String {
        let upper = text.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if upper.hasPrefix("VS-") {
            return String(upper.dropFirst(3).prefix(4))
        }
        return String(upper.prefix(4))
    }

    static func deviceIDList(_ text: String) -> [String] {
        text.split(separator: ",")
            .map { normalizedDeviceID(String($0)) }
            .filter { $0.count == 4 && $0.allSatisfy(\.isHexDigit) }
            .reduce(into: []) { ids, id in
                if !ids.contains(id) {
                    ids.append(id)
                }
            }
    }
}

private extension Bool {
    var tomlValue: String { self ? "true" : "false" }
}

private extension String {
    var tomlEscaped: String {
        replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
    }
}
