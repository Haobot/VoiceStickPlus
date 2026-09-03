import AppKit
import Foundation
import TOMLKit
import VoiceStickCore

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

enum OverlayThemeColor: String, CaseIterable {
    case white
    case pink
    case green
    case yellow
    case blue
    case purple

    var displayName: String {
        switch self {
        case .white:
            return "White"
        case .pink:
            return "Pink"
        case .green:
            return "Green"
        case .yellow:
            return "Yellow"
        case .blue:
            return "Blue"
        case .purple:
            return "Purple"
        }
    }
}

enum OverlayPosition: String, CaseIterable {
    case center
    case topLeft = "top_left"
    case topRight = "top_right"
    case bottomLeft = "bottom_left"
    case bottomRight = "bottom_right"

    var displayName: String {
        switch self {
        case .center:
            return "Center"
        case .topLeft:
            return "Top Left"
        case .topRight:
            return "Top Right"
        case .bottomLeft:
            return "Bottom Left"
        case .bottomRight:
            return "Bottom Right"
        }
    }
}

enum OutputTarget: String, CaseIterable {
    case focusedApp = "focused_app"
    case subtitle

    var displayName: String {
        switch self {
        case .focusedApp:
            return "Focused App"
        case .subtitle:
            return "Subtitle"
        }
    }
}

enum TextTransform: String, CaseIterable {
    case original
    case translate

    var displayName: String {
        switch self {
        case .original:
            return "Original"
        case .translate:
            return "Translate"
        }
    }
}

struct OutputProfile: Equatable {
    var target: OutputTarget
    var transform: TextTransform
    var translationTarget: String

    static let `default` = OutputProfile(
        target: .focusedApp,
        transform: .original,
        translationTarget: "en"
    )

    var usesSubtitleASR: Bool {
        target == .subtitle
    }
}

/// 配对设备条目（对齐 Windows PairedDeviceEntry，CSV 持久化格式
/// `id,addr,address_kind,name[,hardware,firmware_version]`）。
/// macOS 读不到蓝牙 MAC：addr 存 CBPeripheral.identifier.uuidString（大写），
/// addressKind 恒为 "uuid"；Windows 的 12 位 hex MAC + "0"/"1"/"2" 原样透传不解释。
struct PairedDeviceEntry: Equatable {
    var deviceID: String
    var address: String
    var addressKind: String
    var name: String
    var hardware: String
    var firmwareVersion: String

    /// hardware 段标识（对齐 Windows kHardwareXiaomiRemote2Pro）。
    static let hardwareXiaomiRemote2Pro = "xiaomi_remote_2_pro"
}

/// 小米蓝牙遥控器 2 Pro 设置（对齐 Windows XiaomiSettings）：全局默认即结构默认值，
/// [device.<id>.xiaomi] 按设备覆盖（加载时已用默认填平所有字段）。
struct XiaomiSettings: Equatable {
    /// ADPCM 解码后增益（dB），消费侧 ±24 限幅。默认 12.0。
    var gainDb = 12.0
    /// 语音键双击时序窗（ms）：第一次短击释放后等待第二次按下的最大窗口。默认 350。
    var doubleClickMs = 350

    static let `default` = XiaomiSettings()
}

struct AppConfig {
    var asrProvider: ASRProvider
    var voiceStickAPIKey: String
    var voiceStickCloudURL: String
    var volcengineAPIKey: String
    var llmBaseURL: String
    var llmAPIKey: String
    var llmModel: String
    var interactionMode: InteractionMode
    var resourceID: String
    var asrHotwords: [String]
    var pairedDeviceIDs: [String]
    var deviceThemeColors: [String: OverlayThemeColor]
    var deviceOverlayPositions: [String: OverlayPosition]
    var defaultOutputProfile: OutputProfile
    var deviceOutputProfiles: [String: OutputProfile]
    var autoEnter: Bool
    var debugAudioCache: Bool
    var debugAudioDirectory: URL
    /// 配对设备条目表（paired_device CSV 数组持久化），与 pairedDeviceIDs 同步维护。
    var pairedDevices: [PairedDeviceEntry]
    /// [device.<id>.xiaomi] 按设备覆盖（键为归一化 4 位大写 hex ID）。
    var deviceXiaomiSettings: [String: XiaomiSettings]
    /// 小米语音键按下时遥控器固件会多发一个 F5 键：是否由事件钩子吞掉（默认开）。
    var xiaomiSuppressF5: Bool

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
    static let websiteURL = URL(string: "https://haobot.github.io/VoiceStickPlus/")!
    static let firmwareManifestURL = URL(
        string: "https://github.com/Haobot/VoiceStickPlus/releases/latest/download/manifest.json"
    )!
    static let minimumCompatibleFirmwareVersion = "0.3.0"

    static var configExists: Bool {
        FileManager.default.fileExists(atPath: configURL.path)
    }

    static var defaults: AppConfig {
        AppConfig(
            asrProvider: .voiceStickCloud,
            voiceStickAPIKey: "",
            voiceStickCloudURL: defaultVoiceStickCloudURL,
            volcengineAPIKey: "",
            llmBaseURL: "https://api.openai.com/v1",
            llmAPIKey: "",
            llmModel: "gpt-5.5",
            interactionMode: .holdToTalk,
            resourceID: supportedResourceIDs[0],
            asrHotwords: [],
            pairedDeviceIDs: [],
            deviceThemeColors: [:],
            deviceOverlayPositions: [:],
            defaultOutputProfile: .default,
            deviceOutputProfiles: [:],
            autoEnter: true,
            debugAudioCache: false,
            debugAudioDirectory: defaultDebugAudioDirectory,
            pairedDevices: [],
            deviceXiaomiSettings: [:],
            xiaomiSuppressF5: true
        )
    }

    static func load() -> AppConfig {
        let defaults = Self.defaults

        guard let text = try? String(contentsOf: configURL) else {
            return defaults
        }

        return parse(text: text, defaults: defaults)
    }

    /// 从 TOML 文本解析配置（纯函数，不触碰磁盘；单测直接喂字符串）。
    /// TOML 解码失败回退 legacy 逐行解析（兼容古早配置），与原 load() 行为一致。
    static func parse(text: String, defaults: AppConfig = Self.defaults) -> AppConfig {
        guard let file = try? TOMLDecoder().decode(ConfigFile.self, from: TOMLTable(string: text)) else {
            return loadLegacy(text: text, defaults: defaults)
        }

        return AppConfig(
            asrProvider: asrProviderValue(file.asr_provider, default: defaults.asrProvider),
            voiceStickAPIKey: file.voicestick_api_key ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: file.voicestick_cloud_url ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: file.volcengine_api_key ?? file.api_key ?? defaults.volcengineAPIKey,
            llmBaseURL: file.llm_base_url ?? defaults.llmBaseURL,
            llmAPIKey: file.llm_api_key ?? defaults.llmAPIKey,
            llmModel: file.llm_model ?? defaults.llmModel,
            interactionMode: interactionModeValue(file.interaction_mode, default: defaults.interactionMode),
            resourceID: resourceIDValue(file.resource_id, default: defaults.resourceID),
            asrHotwords: hotwordList(file.asr_hotwords ?? ""),
            pairedDeviceIDs: deviceIDList(file.paired_device_ids ?? ""),
            deviceThemeColors: deviceThemeColorMap(file.device_theme_colors ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(file.device_overlay_positions ?? ""),
            defaultOutputProfile: outputProfile(
                target: file.output?.target ?? file.output_target,
                transform: file.output?.transform ?? file.text_transform,
                translationTarget: file.output?.translation_target ?? file.translation_target,
                default: defaults.defaultOutputProfile
            ),
            deviceOutputProfiles: deviceOutputProfileMap(
                file.device,
                defaultProfile: outputProfile(
                    target: file.output?.target ?? file.output_target,
                    transform: file.output?.transform ?? file.text_transform,
                    translationTarget: file.output?.translation_target ?? file.translation_target,
                    default: defaults.defaultOutputProfile
                )
            ),
            autoEnter: file.auto_enter ?? defaults.autoEnter,
            debugAudioCache: file.debug_audio_cache ?? defaults.debugAudioCache,
            debugAudioDirectory: directoryValue(file.debug_audio_dir, default: defaults.debugAudioDirectory),
            pairedDevices: pairedDeviceEntryList(file.paired_device ?? []),
            deviceXiaomiSettings: deviceXiaomiSettingsMap(file.device),
            xiaomiSuppressF5: file.xiaomi_suppress_f5 ?? defaults.xiaomiSuppressF5
        )
    }

    func save() throws {
        try FileManager.default.createDirectory(at: Self.configDirectory, withIntermediateDirectories: true)
        try serializedText().write(to: Self.configURL, atomically: true, encoding: .utf8)
    }

    /// 序列化为 TOML 文本（纯函数，不触碰磁盘；单测与 parse(text:) 对拍 round-trip）。
    func serializedText() -> String {
        var text = """
        asr_provider = "\(asrProvider.rawValue)"
        voicestick_api_key = "\(voiceStickAPIKey.tomlEscaped)"
        voicestick_cloud_url = "\(voiceStickCloudURL.tomlEscaped)"
        volcengine_api_key = "\(volcengineAPIKey.tomlEscaped)"
        llm_base_url = "\(llmBaseURL.tomlEscaped)"
        llm_api_key = "\(llmAPIKey.tomlEscaped)"
        llm_model = "\(llmModel.tomlEscaped)"
        interaction_mode = "\(interactionMode.rawValue)"
        resource_id = "\(resourceID.tomlEscaped)"
        asr_hotwords = "\(asrHotwords.joined(separator: ",").tomlEscaped)"
        paired_device_ids = "\(pairedDeviceIDs.joined(separator: ",").tomlEscaped)"
        device_theme_colors = "\(deviceThemeColorText.tomlEscaped)"
        device_overlay_positions = "\(deviceOverlayPositionText.tomlEscaped)"
        auto_enter = \(autoEnter.tomlValue)
        xiaomi_suppress_f5 = \(xiaomiSuppressF5.tomlValue)
        debug_audio_cache = \(debugAudioCache.tomlValue)
        debug_audio_dir = "\(debugAudioDirectory.path.tomlEscaped)"
        """
        text += pairedDevicesText
        text += """

        [output]
        target = "\(defaultOutputProfile.target.rawValue)"
        transform = "\(defaultOutputProfile.transform.rawValue)"
        translation_target = "\(defaultOutputProfile.translationTarget.tomlEscaped)"
        """
        return text + deviceOutputProfileText + deviceXiaomiSettingsText
    }

    private static func loadLegacy(text: String, defaults: AppConfig) -> AppConfig {
        var values: [String: String] = [:]
        var pairedDeviceLines: [String] = []
        var inPairedDeviceArray = false
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#") else { continue }
            // paired_device 数组块（现行序列化格式 paired_device = [ ... ]）：逐行收
            // CSV 元素（对齐 Windows legacy 保留 paired_device 的语义），"]" 行收尾。
            if inPairedDeviceArray {
                if line.hasPrefix("]") {
                    inPairedDeviceArray = false
                    continue
                }
                var element = line
                if element.hasSuffix(",") { element.removeLast() }
                element = element.trimmingCharacters(in: CharacterSet(charactersIn: "\""))
                if !element.isEmpty { pairedDeviceLines.append(element) }
                continue
            }
            let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            let key = parts[0].trimmingCharacters(in: .whitespaces)
            let value = parts[1].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
            // 单行 `paired_device = "csv"`（Windows legacy 行式）也收。
            if key == "paired_device" {
                if value == "[" {
                    inPairedDeviceArray = true
                } else if !value.isEmpty {
                    pairedDeviceLines.append(value)
                }
                continue
            }
            values[key] = value
        }

        return AppConfig(
            asrProvider: asrProviderValue(values["asr_provider"], default: defaults.asrProvider),
            voiceStickAPIKey: values["voicestick_api_key"] ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: values["voicestick_cloud_url"] ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: values["volcengine_api_key"] ?? values["api_key"] ?? defaults.volcengineAPIKey,
            llmBaseURL: values["llm_base_url"] ?? defaults.llmBaseURL,
            llmAPIKey: values["llm_api_key"] ?? defaults.llmAPIKey,
            llmModel: values["llm_model"] ?? defaults.llmModel,
            interactionMode: interactionModeValue(values["interaction_mode"], default: defaults.interactionMode),
            resourceID: resourceIDValue(values["resource_id"], default: defaults.resourceID),
            asrHotwords: hotwordList(values["asr_hotwords"] ?? ""),
            pairedDeviceIDs: deviceIDList(values["paired_device_ids"] ?? ""),
            deviceThemeColors: deviceThemeColorMap(values["device_theme_colors"] ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(values["device_overlay_positions"] ?? ""),
            defaultOutputProfile: outputProfile(
                target: values["output_target"],
                transform: values["text_transform"],
                translationTarget: values["translation_target"],
                default: defaults.defaultOutputProfile
            ),
            deviceOutputProfiles: [:],
            autoEnter: boolValue(values["auto_enter"], default: defaults.autoEnter),
            debugAudioCache: boolValue(values["debug_audio_cache"], default: defaults.debugAudioCache),
            debugAudioDirectory: directoryValue(values["debug_audio_dir"], default: defaults.debugAudioDirectory),
            pairedDevices: pairedDeviceEntryList(pairedDeviceLines),
            // [device.<id>.xiaomi] 表放弃解析（表结构超出逐行解析能力；
            // Windows legacy 同样不解析 device 表）。
            deviceXiaomiSettings: [:],
            xiaomiSuppressF5: boolValue(values["xiaomi_suppress_f5"], default: defaults.xiaomiSuppressF5)
        )
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

    private static func interactionModeValue(_ text: String?, default defaultValue: InteractionMode) -> InteractionMode {
        guard let text, let mode = InteractionMode(rawValue: text) else { return defaultValue }
        return mode
    }

    private static func outputTargetValue(_ text: String?, default defaultValue: OutputTarget) -> OutputTarget {
        guard let text, let target = OutputTarget(rawValue: text) else { return defaultValue }
        return target
    }

    private static func textTransformValue(_ text: String?, default defaultValue: TextTransform) -> TextTransform {
        guard let text, let transform = TextTransform(rawValue: text) else { return defaultValue }
        return transform
    }

    private static func outputProfile(
        target: String?,
        transform: String?,
        translationTarget: String?,
        default defaultValue: OutputProfile
    ) -> OutputProfile {
        OutputProfile(
            target: outputTargetValue(target, default: defaultValue.target),
            transform: textTransformValue(transform, default: defaultValue.transform),
            translationTarget: (translationTarget?.trimmingCharacters(in: .whitespacesAndNewlines)).flatMap {
                $0.isEmpty ? nil : $0
            } ?? defaultValue.translationTarget
        )
    }

    private static func resourceIDValue(_ text: String?, default defaultValue: String) -> String {
        guard let text, supportedResourceIDs.contains(text) else { return defaultValue }
        return text
    }

    static func normalizedDeviceID(_ text: String) -> String {
        let upper = text.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        // 对齐 Windows NormalizeDeviceId：VS-/RC- 前缀都剥、截 4 位后必须恰好
        // 4 位 ASCII hex（IsHex4），非法一律返回 ""（校验不再下放调用方）。
        let stripped = (upper.hasPrefix("VS-") || upper.hasPrefix("RC-"))
            ? upper.dropFirst(3).prefix(4)
            : upper.prefix(4)
        guard stripped.count == 4, stripped.allSatisfy({ $0.isASCII && $0.isHexDigit }) else {
            return ""
        }
        return String(stripped)
    }

    static func deviceIDList(_ text: String) -> [String] {
        text.split(separator: ",")
            .map { normalizedDeviceID(String($0)) }
            .filter { $0.count == 4 && $0.allSatisfy({ $0.isASCII && $0.isHexDigit }) }
            .reduce(into: []) { ids, id in
                if !ids.contains(id) {
                    ids.append(id)
                }
            }
    }

    static func hotwordList(_ text: String) -> [String] {
        text.split { character in
            character == "," || character == "\n" || character == "\r"
        }
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
            .reduce(into: []) { hotwords, hotword in
                if !hotwords.contains(hotword) {
                    hotwords.append(hotword)
                }
            }
    }

    static func deviceThemeColorMap(_ text: String) -> [String: OverlayThemeColor] {
        text.split(separator: ",").reduce(into: [:]) { colorsByDeviceID, rawPair in
            let parts = rawPair.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return }
            let deviceID = normalizedDeviceID(parts[0])
            let colorName = parts[1].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard deviceID.count == 4,
                  deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let color = OverlayThemeColor(rawValue: colorName) else { return }
            colorsByDeviceID[deviceID] = color
        }
    }

    func themeColor(for deviceID: String?) -> OverlayThemeColor {
        guard let deviceID else { return .white }
        return deviceThemeColors[Self.normalizedDeviceID(deviceID)] ?? .white
    }

    static func deviceOverlayPositionMap(_ text: String) -> [String: OverlayPosition] {
        text.split(separator: ",").reduce(into: [:]) { positionsByDeviceID, rawPair in
            let parts = rawPair.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return }
            let deviceID = normalizedDeviceID(parts[0])
            let positionName = parts[1].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard deviceID.count == 4,
                  deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let position = OverlayPosition(rawValue: positionName) else { return }
            positionsByDeviceID[deviceID] = position
        }
    }

    func overlayPosition(for deviceID: String?) -> OverlayPosition {
        guard let deviceID else { return .center }
        return deviceOverlayPositions[Self.normalizedDeviceID(deviceID)] ?? .center
    }

    func outputProfile(for deviceID: String?) -> OutputProfile {
        guard let deviceID, let deviceProfile = deviceOutputProfiles[Self.normalizedDeviceID(deviceID)] else {
            return defaultOutputProfile
        }
        return OutputProfile(
            target: defaultOutputProfile.target,
            transform: deviceProfile.transform,
            translationTarget: deviceProfile.translationTarget
        )
    }

    // ---- 配对设备条目（paired_device CSV，对齐 Windows Parse/FormatPairedDeviceEntry）----

    /// 解析一行 CSV：`id,addr,address_kind,name[,hardware,firmware_version]`。
    /// 与 Windows next_field 语义一致：只取前 6 段，缺省段补空字符串，多余段丢弃。
    static func parsePairedDeviceEntry(_ line: String) -> PairedDeviceEntry {
        let fields = line.split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        func field(_ index: Int) -> String { index < fields.count ? fields[index] : "" }
        return PairedDeviceEntry(
            deviceID: field(0),
            address: field(1),
            addressKind: field(2),
            name: field(3),
            hardware: field(4),
            firmwareVersion: field(5)
        )
    }

    /// 格式化对齐 Windows FormatPairedDeviceEntry：固定写满 6 段。
    static func formatPairedDeviceEntry(_ entry: PairedDeviceEntry) -> String {
        [entry.deviceID, entry.address, entry.addressKind, entry.name,
         entry.hardware, entry.firmwareVersion].joined(separator: ",")
    }

    static func pairedDeviceEntryList(_ lines: [String]) -> [PairedDeviceEntry] {
        lines.map { parsePairedDeviceEntry($0) }.filter { !$0.deviceID.isEmpty }
    }

    func pairedDeviceEntry(forID deviceID: String) -> PairedDeviceEntry? {
        let normalized = Self.normalizedDeviceID(deviceID)
        return pairedDevices.first { $0.deviceID == normalized }
    }

    /// 按 CoreBluetooth 外设 UUID 查找（addr 段存大写 uuidString，大小写不敏感比较）。
    func pairedDeviceEntry(forPeripheralUUID uuidString: String) -> PairedDeviceEntry? {
        let target = uuidString.uppercased()
        return pairedDevices.first { $0.address.uppercased() == target }
    }

    /// 设备 hardware 标识（如 "stick_s3"/"xiaomi_remote_2_pro"）；未配对返回 nil。
    func hardware(forID deviceID: String) -> String? {
        pairedDeviceEntry(forID: deviceID)?.hardware
    }

    /// 更新或追加配对设备条目并触发存盘（对齐 Windows SavePairedDevice：整条目替换，
    /// 同时保证 id 进入 pairedDeviceIDs）。id 归一化、addr 大写化。
    /// 返回存盘是否成功；id 归一化为空（非法）时拒绝落库并返回 false。
    mutating func savePairedDevice(id: String, addr: String, kind: String, name: String,
                                   hardware: String, firmwareVersion: String) -> Bool {
        let deviceID = Self.normalizedDeviceID(id)
        guard !deviceID.isEmpty else { return false }
        upsertPairedDevice(PairedDeviceEntry(
            deviceID: deviceID,
            address: addr.uppercased(),
            addressKind: kind,
            name: name,
            hardware: hardware,
            firmwareVersion: firmwareVersion
        ))
        do {
            try save()
            return true
        } catch {
            return false
        }
    }

    /// 纯内存 upsert（不存盘），savePairedDevice 与单测共用。
    mutating func upsertPairedDevice(_ entry: PairedDeviceEntry) {
        guard !entry.deviceID.isEmpty else { return }
        if let index = pairedDevices.firstIndex(where: { $0.deviceID == entry.deviceID }) {
            pairedDevices[index] = entry
        } else {
            pairedDevices.append(entry)
        }
        if !pairedDeviceIDs.contains(entry.deviceID) {
            pairedDeviceIDs.append(entry.deviceID)
        }
    }

    /// 移除配对设备：连带清 paired_devices 条目与全部按设备覆盖并触发存盘
    ///（对齐 Windows RemovePairedDevice）。
    mutating func removePairedDevice(id: String) {
        let deviceID = Self.normalizedDeviceID(id)
        pairedDevices.removeAll { $0.deviceID == deviceID }
        pairedDeviceIDs.removeAll { $0 == deviceID }
        deviceThemeColors.removeValue(forKey: deviceID)
        deviceOverlayPositions.removeValue(forKey: deviceID)
        deviceOutputProfiles.removeValue(forKey: deviceID)
        deviceXiaomiSettings.removeValue(forKey: deviceID)
        try? save()
    }

    // ---- 小米遥控器 [device.<id>.xiaomi] 覆盖（对齐 Windows XiaomiSettingsForDevice）----

    /// 返回设备有效小米设置：有覆盖返回覆盖（加载时已用默认填平），否则全局默认。
    func xiaomiSettings(for deviceID: String?) -> (gainDb: Double, doubleClickMs: Int) {
        guard let deviceID,
              let settings = deviceXiaomiSettings[Self.normalizedDeviceID(deviceID)] else {
            return (XiaomiSettings.default.gainDb, XiaomiSettings.default.doubleClickMs)
        }
        return (settings.gainDb, settings.doubleClickMs)
    }

    /// 解析 [device.<id>.xiaomi] 表：以默认填平；double_click_ms <= 0 保留默认
    ///（对齐 Windows ParseXiaomiSettings）。gain_db 的 ±24 限幅在消费侧（后处理）完成。
    private static func xiaomiSettings(from file: XiaomiConfigFile,
                                       fallback: XiaomiSettings = .default) -> XiaomiSettings {
        var settings = fallback
        if let gainDb = file.gain_db { settings.gainDb = gainDb }
        if let doubleClickMs = file.double_click_ms, doubleClickMs > 0 {
            settings.doubleClickMs = doubleClickMs
        }
        return settings
    }

    private static func deviceXiaomiSettingsMap(
        _ devices: [String: DeviceConfigFile]?
    ) -> [String: XiaomiSettings] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { map, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let xiaomi = pair.value.xiaomi else {
                return
            }
            map[deviceID] = xiaomiSettings(from: xiaomi)
        }
    }

    private static func deviceOutputProfileMap(
        _ devices: [String: DeviceConfigFile]?,
        defaultProfile: OutputProfile
    ) -> [String: OutputProfile] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { profiles, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }), let output = pair.value.output else {
                return
            }
            profiles[deviceID] = outputProfile(
                target: nil,
                transform: output.transform,
                translationTarget: output.translation_target,
                default: defaultProfile
            )
        }
    }

    private var deviceThemeColorText: String {
        deviceThemeColors
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .white }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    private var deviceOverlayPositionText: String {
        deviceOverlayPositions
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .center }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    /// paired_device 数组段（对齐 Windows Save：为空不写出）。
    private var pairedDevicesText: String {
        guard !pairedDevices.isEmpty else { return "" }
        let lines = pairedDevices
            .map { "  \"\(Self.formatPairedDeviceEntry($0).tomlEscaped)\"," }
            .joined(separator: "\n")
        return "\npaired_device = [\n\(lines)\n]\n"
    }

    private var deviceOutputProfileText: String {
        deviceOutputProfiles
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != defaultOutputProfile }
            .sorted { $0.key < $1.key }
            .map { deviceID, profile in
                """

                [device.\(deviceID).output]
                transform = "\(profile.transform.rawValue)"
                translation_target = "\(profile.translationTarget.tomlEscaped)"
                """
            }
            .joined(separator: "\n")
    }

    /// [device.<id>.xiaomi] 覆盖段（对齐 Windows Save：未配对或与默认一致不写出；
    /// 写出的表全量含 2 个字段，保证自含、加载顺序无关）。
    private var deviceXiaomiSettingsText: String {
        deviceXiaomiSettings
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .default }
            .sorted { $0.key < $1.key }
            .map { deviceID, settings in
                """

                [device.\(deviceID).xiaomi]
                gain_db = \(settings.gainDb)
                double_click_ms = \(settings.doubleClickMs)
                """
            }
            .joined(separator: "\n")
    }
}

private struct ConfigFile: Decodable {
    var asr_provider: String?
    var voicestick_api_key: String?
    var voicestick_cloud_url: String?
    var volcengine_api_key: String?
    var api_key: String?
    var llm_base_url: String?
    var llm_api_key: String?
    var llm_model: String?
    var interaction_mode: String?
    var output_target: String?
    var text_transform: String?
    var translation_target: String?
    var resource_id: String?
    var asr_hotwords: String?
    var paired_device_ids: String?
    var device_theme_colors: String?
    var device_overlay_positions: String?
    var auto_enter: Bool?
    var debug_audio_cache: Bool?
    var debug_audio_dir: String?
    var paired_device: [String]?
    var xiaomi_suppress_f5: Bool?
    var output: OutputConfigFile?
    var device: [String: DeviceConfigFile]?
}

private struct OutputConfigFile: Decodable {
    var target: String?
    var transform: String?
    var translation_target: String?
}

private struct DeviceConfigFile: Decodable {
    var output: OutputConfigFile?
    var xiaomi: XiaomiConfigFile?
}

/// [device.<id>.xiaomi] 表。gain_db 宽容接受 TOML 整数（Windows C++ << 对整数值
/// double 会写出 "gain_db = 18" 这种整数形态），double_click_ms 严格整数。
private struct XiaomiConfigFile: Decodable {
    var gain_db: Double?
    var double_click_ms: Int?

    private enum CodingKeys: String, CodingKey {
        case gain_db = "gain_db"
        case double_click_ms = "double_click_ms"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        if let gain = try? container.decode(Double.self, forKey: .gain_db) {
            gain_db = gain
        } else if let gainInt = try? container.decode(Int.self, forKey: .gain_db) {
            gain_db = Double(gainInt)
        } else {
            gain_db = nil
        }
        double_click_ms = try? container.decodeIfPresent(Int.self, forKey: .double_click_ms)
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
