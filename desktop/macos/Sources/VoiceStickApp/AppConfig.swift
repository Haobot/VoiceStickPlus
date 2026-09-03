import AppKit
import Foundation
import TOMLKit
import VoiceStickCore

enum ASRProvider: String {
    case voiceStickCloud = "voicestick_cloud"
    case volcengine
    case tencent

    var displayName: String {
        switch self {
        case .voiceStickCloud:
            return "VoiceStick Cloud"
        case .volcengine:
            return "Volcengine"
        case .tencent:
            return "Tencent Cloud ASR"
        }
    }
}

/// 悬浮窗主题色（对齐 Windows OverlayThemeColor，菜单顺序：自动/白/黑/粉/绿/黄/蓝/紫）。
/// auto 为默认：Windows 按悬浮窗背景亮度采样选白/黑；macOS 无录屏权限无法采样屏幕，
/// 跟随系统深浅外观近似（浅色→黑主题、深色→白主题）。
enum OverlayThemeColor: String, CaseIterable {
    case auto
    case white
    case black
    case pink
    case green
    case yellow
    case blue
    case purple

    var displayName: String {
        switch self {
        case .auto:
            return tr(.themeAuto)
        case .white:
            return tr(.themeWhite)
        case .black:
            return tr(.themeBlack)
        case .pink:
            return tr(.themePink)
        case .green:
            return tr(.themeGreen)
        case .yellow:
            return tr(.themeYellow)
        case .blue:
            return tr(.themeBlue)
        case .purple:
            return tr(.themePurple)
        }
    }
}

/// 悬浮窗主题大小（对齐 Windows OverlayThemeSize，默认 big）。
enum OverlayThemeSize: String, CaseIterable {
    case big
    case medium
    case small

    var displayName: String {
        switch self {
        case .big:
            return tr(.sizeBig)
        case .medium:
            return tr(.sizeMedium)
        case .small:
            return tr(.sizeSmall)
        }
    }
}

/// 悬浮窗位置（对齐 Windows OverlayPosition，默认 bottom_center）。
enum OverlayPosition: String, CaseIterable {
    case center
    case bottomCenter = "bottom_center"
    case topLeft = "top_left"
    case topRight = "top_right"
    case bottomLeft = "bottom_left"
    case bottomRight = "bottom_right"

    var displayName: String {
        switch self {
        case .center:
            return tr(.positionCenter)
        case .bottomCenter:
            return tr(.positionBottomCenter)
        case .topLeft:
            return tr(.positionTopLeft)
        case .topRight:
            return tr(.positionTopRight)
        case .bottomLeft:
            return tr(.positionBottomLeft)
        case .bottomRight:
            return tr(.positionBottomRight)
        }
    }
}

enum OutputTarget: String, CaseIterable {
    case focusedApp = "focused_app"
    case subtitle

    var displayName: String {
        switch self {
        case .focusedApp:
            return tr(.outputFocusedApp)
        case .subtitle:
            return tr(.outputSubtitle)
        }
    }
}

enum TextTransform: String, CaseIterable {
    case original
    case translate

    var displayName: String {
        switch self {
        case .original:
            return tr(.textOriginal)
        case .translate:
            return tr(.menuTranslation)
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

/// IMU 唤醒灵敏度（对齐 Windows ImuWakeSensitivity）：low/medium/high。
/// 阈值映射注意是反向的：灵敏度越高阈值越低（low=800 / medium=500 / high=250 lsb）。
enum ImuWakeSensitivity: String, CaseIterable {
    case low
    case medium
    case high

    var thresholdLsb: Int {
        switch self {
        case .low: return 800
        case .medium: return 500
        case .high: return 250
        }
    }

    var displayName: String {
        switch self {
        case .low: return tr(.wakeLow)
        case .medium: return tr(.wakeMedium)
        case .high: return tr(.wakeHigh)
        }
    }
}

/// 设备交互设置（对齐 Windows InteractionSettings；[device.<id>.interaction] 覆盖表
/// 键名与顶层一致，加载时以全局默认填平）。体感鼠标 X/Y 灵敏度保留字段（跨端配置
/// 兼容），macOS 暂无体感鼠标消费侧。
struct InteractionSettings: Equatable {
    var imuWakeSensitivity: ImuWakeSensitivity = .low
    var tapToArrow = false
    /// 1...10；加载时越界值回落 5（对齐 Windows TapSensitivityClamp）。
    var tapSensitivity = 5
    var airMouseSensitivityX = 5
    var airMouseSensitivityY = 5

    static let `default` = InteractionSettings()

    /// 对齐 Windows TapSensitivityClamp/AirMouseSensitivityClamp：不在 1...10 回落 5。
    static func clampedSensitivity(_ value: Int) -> Int {
        (1...10).contains(value) ? value : 5
    }
}

/// 编码器录音灯颜色（对齐 Windows encoder_led_color 8 色枚举，顺序固定）。
enum EncoderLedColor: String, CaseIterable {
    case red
    case green
    case blue
    case yellow
    case purple
    case cyan
    case white
    case off

    var displayName: String {
        switch self {
        case .red: return tr(.ledRed)
        case .green: return tr(.ledGreen)
        case .blue: return tr(.ledBlue)
        case .yellow: return tr(.ledYellow)
        case .purple: return tr(.ledPurple)
        case .cyan: return tr(.ledCyan)
        case .white: return tr(.ledWhite)
        case .off: return tr(.ledOff)
        }
    }
}

/// 编码器单击/双击动作：recording=录音语义（press 路由主键 / double-click 远程起停），
/// key=自定义按键注入（对齐 Windows EncoderSettings press_action/double_click_action）。
enum EncoderButtonAction: String, CaseIterable {
    case recording
    case key

    var displayName: String {
        switch self {
        case .recording: return tr(.actionRecording)
        case .key: return tr(.actionCustomKey)
        }
    }
}

/// 编码器设置（对齐 Windows EncoderSettings；[device.<id>.encoder] 覆盖表键名去掉
/// encoder_ 前缀，加载时以全局默认填平）。按键字段为 key_spec 语法字符串
/// （见 KeySpec.parse）；加载校验失败的字段保留 fallback。
struct EncoderSettings: Equatable {
    /// 旋转注入总开关（false 时旋转事件整段忽略）。默认开。
    var toArrow = true
    /// 旋转方向翻转（cw/ccw 键互换）。默认关。
    var rotationInvert = false
    var rotateCwKey = "down"
    var rotateCcwKey = "up"
    /// 快慢分档阈值（格/秒）：EWMA 估计速度 >= 阈值走快速键；<=0 关闭分档。
    /// 加载要求 >0（存 0 下次加载回默认）。默认 200。
    var rotateFastThreshold = 200
    var rotateCwFastKey = "pagedown"
    var rotateCcwFastKey = "pageup"
    /// 慢速判定窗口（ms）：0=立即注入（旧行为）；加载要求 >=0。默认 80。
    var rotateDecideWindowMs = 80
    var ledColor: EncoderLedColor = .red
    var pressAction: EncoderButtonAction = .recording
    /// press_action=key 时的注入键；唯一显式允许为空的按键字段。
    var pressKey = ""
    var doubleClickAction: EncoderButtonAction = .key
    var doubleClickKey = "enter"

    static let `default` = EncoderSettings()
}

struct AppConfig {
    var asrProvider: ASRProvider
    var voiceStickAPIKey: String
    var voiceStickCloudURL: String
    var volcengineAPIKey: String
    /// 腾讯云实时语音识别凭据（对齐 Windows tencent_*；加载时 Trim，引擎模型默认 16k_zh）。
    var tencentSecretID: String
    var tencentSecretKey: String
    var tencentAppid: String
    var tencentEngineModelType: String
    var tencentHotwordID: String
    var llmBaseURL: String
    var llmAPIKey: String
    var llmModel: String
    /// ASR 文本精修开关与自定义 prompt（对齐 Windows refine_enabled/refine_prompt；
    /// 默认关，prompt 留空用内置默认）。
    var refineEnabled: Bool
    var refinePrompt: String
    /// 向所有 LLM 请求注入 enable_thinking:false 关闭深度思考（对齐 Windows，默认开）。
    var llmDisableThinking: Bool
    var interactionMode: InteractionMode
    /// 界面语言（对齐 Windows ui_language，默认 system 跟随系统）。
    var uiLanguage: UiLanguage
    var resourceID: String
    var asrHotwords: [String]
    var pairedDeviceIDs: [String]
    var deviceThemeColors: [String: OverlayThemeColor]
    var deviceOverlayPositions: [String: OverlayPosition]
    var deviceThemeSizes: [String: OverlayThemeSize]
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
    /// 全局热键开关与绑定（对齐 Windows global_hotkey_enabled / global_hotkey，
    /// 默认开 + "Alt+X"=Option+X；键名语法跨端兼容：Alt→⌥、Win→⌘、Ctrl→⌃、Shift→⇧）。
    var globalHotkeyEnabled: Bool
    var globalHotkey: String
    /// 开机自启动（对齐 Windows launch_at_login，默认开；macOS 13+ 经 SMAppService 生效）。
    var launchAtLogin: Bool
    /// 开发者模式（对齐 Windows developer_mode，默认关）：设置窗口放出 API Key/资源 ID/
    /// LLM 凭据/输出目标/系统区/调试开关等高级行，勾选实时生效。
    var developerMode: Bool
    /// IMU 调试显示开关（对齐 Windows show_imu_debug，默认关）：经 control_rx 下发固件，
    /// 固件在屏幕上渲染 IMU 调试信息。
    var showIMUDebug: Bool
    /// 全局默认设备交互设置（对齐 Windows default_interaction_settings，顶层键
    /// imu_wake_sensitivity/tap_to_arrow/tap_sensitivity/air_mouse_sensitivity_x/y）。
    var interactionSettings: InteractionSettings
    /// [device.<id>.interaction] 按设备覆盖（键为归一化 4 位大写 hex ID，加载时已用
    /// 全局默认填平）。
    var deviceInteractionSettings: [String: InteractionSettings]
    /// 全局默认编码器设置（对齐 Windows default_encoder_settings，顶层键 encoder_*）。
    var encoderSettings: EncoderSettings
    /// [device.<id>.encoder] 按设备覆盖（键名去 encoder_ 前缀，加载时已用全局默认填平）。
    var deviceEncoderSettings: [String: EncoderSettings]

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
            tencentSecretID: "",
            tencentSecretKey: "",
            tencentAppid: "",
            tencentEngineModelType: "16k_zh",
            tencentHotwordID: "",
            llmBaseURL: "https://api.openai.com/v1",
            llmAPIKey: "",
            llmModel: "gpt-5.5",
            refineEnabled: false,
            refinePrompt: "",
            llmDisableThinking: true,
            interactionMode: .holdToTalk,
            uiLanguage: .system,
            resourceID: supportedResourceIDs[0],
            asrHotwords: [],
            pairedDeviceIDs: [],
            deviceThemeColors: [:],
            deviceOverlayPositions: [:],
            deviceThemeSizes: [:],
            defaultOutputProfile: .default,
            deviceOutputProfiles: [:],
            autoEnter: true,
            debugAudioCache: false,
            debugAudioDirectory: defaultDebugAudioDirectory,
            pairedDevices: [],
            deviceXiaomiSettings: [:],
            xiaomiSuppressF5: true,
            globalHotkeyEnabled: true,
            globalHotkey: "Alt+X",
            launchAtLogin: true,
            developerMode: false,
            showIMUDebug: false,
            interactionSettings: .default,
            deviceInteractionSettings: [:],
            encoderSettings: .default,
            deviceEncoderSettings: [:]
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

        let interaction = interactionSettingsValue(file, default: defaults.interactionSettings)
        let encoder = encoderSettingsValue(file, default: defaults.encoderSettings)
        var config = AppConfig(
            asrProvider: asrProviderValue(file.asr_provider, default: defaults.asrProvider),
            voiceStickAPIKey: file.voicestick_api_key ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: file.voicestick_cloud_url ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: file.volcengine_api_key ?? file.api_key ?? defaults.volcengineAPIKey,
            tencentSecretID: trimmed(file.tencent_secret_id, default: defaults.tencentSecretID),
            tencentSecretKey: trimmed(file.tencent_secret_key, default: defaults.tencentSecretKey),
            tencentAppid: trimmed(file.tencent_appid, default: defaults.tencentAppid),
            tencentEngineModelType: file.tencent_engine_model_type ?? defaults.tencentEngineModelType,
            tencentHotwordID: trimmed(file.tencent_hotword_id, default: defaults.tencentHotwordID),
            llmBaseURL: file.llm_base_url ?? defaults.llmBaseURL,
            llmAPIKey: file.llm_api_key ?? defaults.llmAPIKey,
            llmModel: file.llm_model ?? defaults.llmModel,
            refineEnabled: file.refine_enabled ?? defaults.refineEnabled,
            refinePrompt: file.refine_prompt ?? defaults.refinePrompt,
            llmDisableThinking: file.llm_disable_thinking ?? defaults.llmDisableThinking,
            interactionMode: interactionModeValue(file.interaction_mode, default: defaults.interactionMode),
            uiLanguage: uiLanguageValue(file.ui_language, default: defaults.uiLanguage),
            resourceID: resourceIDValue(file.resource_id, default: defaults.resourceID),
            asrHotwords: hotwordList(file.asr_hotwords ?? ""),
            pairedDeviceIDs: deviceIDList(file.paired_device_ids ?? ""),
            deviceThemeColors: deviceThemeColorMap(file.device_theme_colors ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(file.device_overlay_positions ?? ""),
            deviceThemeSizes: deviceThemeSizeMap(file.device_theme_sizes ?? ""),
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
            xiaomiSuppressF5: file.xiaomi_suppress_f5 ?? defaults.xiaomiSuppressF5,
            globalHotkeyEnabled: file.global_hotkey_enabled ?? defaults.globalHotkeyEnabled,
            globalHotkey: (file.global_hotkey?.isEmpty == false) ? file.global_hotkey! : defaults.globalHotkey,
            launchAtLogin: file.launch_at_login ?? defaults.launchAtLogin,
            developerMode: file.developer_mode ?? defaults.developerMode,
            showIMUDebug: file.show_imu_debug ?? defaults.showIMUDebug,
            interactionSettings: interaction,
            deviceInteractionSettings: deviceInteractionSettingsMap(file.device, fallback: interaction),
            encoderSettings: encoder,
            deviceEncoderSettings: deviceEncoderSettingsMap(file.device, fallback: encoder)
        )
        recoverTencentSecretID(&config)
        return config
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
        tencent_secret_id = "\(tencentSecretID.tomlEscaped)"
        tencent_secret_key = "\(tencentSecretKey.tomlEscaped)"
        tencent_appid = "\(tencentAppid.tomlEscaped)"
        tencent_engine_model_type = "\(tencentEngineModelType.tomlEscaped)"
        tencent_hotword_id = "\(tencentHotwordID.tomlEscaped)"
        llm_base_url = "\(llmBaseURL.tomlEscaped)"
        llm_api_key = "\(llmAPIKey.tomlEscaped)"
        llm_model = "\(llmModel.tomlEscaped)"
        refine_enabled = \(refineEnabled.tomlValue)
        refine_prompt = "\(refinePrompt.tomlEscaped)"
        llm_disable_thinking = \(llmDisableThinking.tomlValue)
        interaction_mode = "\(interactionMode.rawValue)"
        ui_language = "\(uiLanguage.rawValue)"
        resource_id = "\(resourceID.tomlEscaped)"
        asr_hotwords = "\(asrHotwords.joined(separator: ",").tomlEscaped)"
        paired_device_ids = "\(pairedDeviceIDs.joined(separator: ",").tomlEscaped)"
        device_theme_colors = "\(deviceThemeColorText.tomlEscaped)"
        device_overlay_positions = "\(deviceOverlayPositionText.tomlEscaped)"
        device_theme_sizes = "\(deviceThemeSizeText.tomlEscaped)"
        auto_enter = \(autoEnter.tomlValue)
        xiaomi_suppress_f5 = \(xiaomiSuppressF5.tomlValue)
        global_hotkey_enabled = \(globalHotkeyEnabled.tomlValue)
        global_hotkey = "\(globalHotkey.tomlEscaped)"
        launch_at_login = \(launchAtLogin.tomlValue)
        developer_mode = \(developerMode.tomlValue)
        show_imu_debug = \(showIMUDebug.tomlValue)
        imu_wake_sensitivity = "\(interactionSettings.imuWakeSensitivity.rawValue)"
        tap_to_arrow = \(interactionSettings.tapToArrow.tomlValue)
        tap_sensitivity = \(interactionSettings.tapSensitivity)
        air_mouse_sensitivity_x = \(interactionSettings.airMouseSensitivityX)
        air_mouse_sensitivity_y = \(interactionSettings.airMouseSensitivityY)
        encoder_to_arrow = \(encoderSettings.toArrow.tomlValue)
        encoder_rotation_invert = \(encoderSettings.rotationInvert.tomlValue)
        encoder_rotate_cw_key = "\(encoderSettings.rotateCwKey.tomlEscaped)"
        encoder_rotate_ccw_key = "\(encoderSettings.rotateCcwKey.tomlEscaped)"
        encoder_rotate_fast_threshold = \(encoderSettings.rotateFastThreshold)
        encoder_rotate_cw_fast_key = "\(encoderSettings.rotateCwFastKey.tomlEscaped)"
        encoder_rotate_ccw_fast_key = "\(encoderSettings.rotateCcwFastKey.tomlEscaped)"
        encoder_rotate_decide_window_ms = \(encoderSettings.rotateDecideWindowMs)
        encoder_led_color = "\(encoderSettings.ledColor.rawValue)"
        encoder_press_action = "\(encoderSettings.pressAction.rawValue)"
        encoder_press_key = "\(encoderSettings.pressKey.tomlEscaped)"
        encoder_double_click_action = "\(encoderSettings.doubleClickAction.rawValue)"
        encoder_double_click_key = "\(encoderSettings.doubleClickKey.tomlEscaped)"
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
            + deviceInteractionSettingsText + deviceEncoderSettingsText
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

        let interaction = interactionSettingsValue(values, default: defaults.interactionSettings)
        let encoder = encoderSettingsValue(values, default: defaults.encoderSettings)
        var config = AppConfig(
            asrProvider: asrProviderValue(values["asr_provider"], default: defaults.asrProvider),
            voiceStickAPIKey: values["voicestick_api_key"] ?? defaults.voiceStickAPIKey,
            voiceStickCloudURL: values["voicestick_cloud_url"] ?? defaults.voiceStickCloudURL,
            volcengineAPIKey: values["volcengine_api_key"] ?? values["api_key"] ?? defaults.volcengineAPIKey,
            tencentSecretID: trimmed(values["tencent_secret_id"], default: defaults.tencentSecretID),
            tencentSecretKey: trimmed(values["tencent_secret_key"], default: defaults.tencentSecretKey),
            tencentAppid: trimmed(values["tencent_appid"], default: defaults.tencentAppid),
            tencentEngineModelType: values["tencent_engine_model_type"] ?? defaults.tencentEngineModelType,
            tencentHotwordID: trimmed(values["tencent_hotword_id"], default: defaults.tencentHotwordID),
            llmBaseURL: values["llm_base_url"] ?? defaults.llmBaseURL,
            llmAPIKey: values["llm_api_key"] ?? defaults.llmAPIKey,
            llmModel: values["llm_model"] ?? defaults.llmModel,
            refineEnabled: boolValue(values["refine_enabled"], default: defaults.refineEnabled),
            refinePrompt: values["refine_prompt"] ?? defaults.refinePrompt,
            llmDisableThinking: boolValue(values["llm_disable_thinking"], default: defaults.llmDisableThinking),
            interactionMode: interactionModeValue(values["interaction_mode"], default: defaults.interactionMode),
            uiLanguage: uiLanguageValue(values["ui_language"], default: defaults.uiLanguage),
            resourceID: resourceIDValue(values["resource_id"], default: defaults.resourceID),
            asrHotwords: hotwordList(values["asr_hotwords"] ?? ""),
            pairedDeviceIDs: deviceIDList(values["paired_device_ids"] ?? ""),
            deviceThemeColors: deviceThemeColorMap(values["device_theme_colors"] ?? ""),
            deviceOverlayPositions: deviceOverlayPositionMap(values["device_overlay_positions"] ?? ""),
            deviceThemeSizes: deviceThemeSizeMap(values["device_theme_sizes"] ?? ""),
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
            xiaomiSuppressF5: boolValue(values["xiaomi_suppress_f5"], default: defaults.xiaomiSuppressF5),
            globalHotkeyEnabled: boolValue(values["global_hotkey_enabled"], default: defaults.globalHotkeyEnabled),
            globalHotkey: values["global_hotkey"].flatMap { $0.isEmpty ? nil : $0 } ?? defaults.globalHotkey,
            launchAtLogin: boolValue(values["launch_at_login"], default: defaults.launchAtLogin),
            developerMode: boolValue(values["developer_mode"], default: defaults.developerMode),
            showIMUDebug: boolValue(values["show_imu_debug"], default: defaults.showIMUDebug),
            interactionSettings: interaction,
            // [device.<id>.interaction]/[device.<id>.encoder] 表放弃解析（表结构超出
            // 逐行解析能力；Windows legacy 同样不解析 device 表）。
            deviceInteractionSettings: [:],
            encoderSettings: encoder,
            deviceEncoderSettings: [:]
        )
        recoverTencentSecretID(&config)
        return config
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

    /// 对齐 Windows TomlTrimmedString：凭据类字段加载时去首尾空白。
    private static func trimmed(_ text: String?, default defaultValue: String) -> String {
        guard let text else { return defaultValue }
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// 对齐 Windows MaybeRecoverTencentSecretId：修复早期对话框把 Tencent SecretId
    /// 误写进 volcengine_api_key 的字段映射 bug（provider=tencent 且 volcengine key
    /// 形如 AKID… 且 tencent_secret_id 无效时回迁）。Windows 触发后立即重存盘；
    /// macOS parse 是纯函数，只在内存回迁，持久化留给下一次 save()。
    private static func recoverTencentSecretID(_ config: inout AppConfig) {
        guard config.asrProvider == .tencent,
              config.volcengineAPIKey.hasPrefix("AKID"),
              !config.tencentSecretID.hasPrefix("AKID") else { return }
        config.tencentSecretID = config.volcengineAPIKey
        config.volcengineAPIKey = ""
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

    /// 对齐 Windows UiLanguageFromName：zh_CN/zh-CN/zh 兼容，非法值回 system。
    private static func uiLanguageValue(_ text: String?, default defaultValue: UiLanguage) -> UiLanguage {
        guard let text else { return defaultValue }
        return UiLanguage(configValue: text)
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
        guard let deviceID else { return .auto }
        return deviceThemeColors[Self.normalizedDeviceID(deviceID)] ?? .auto
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
        guard let deviceID else { return .bottomCenter }
        return deviceOverlayPositions[Self.normalizedDeviceID(deviceID)] ?? .bottomCenter
    }

    static func deviceThemeSizeMap(_ text: String) -> [String: OverlayThemeSize] {
        text.split(separator: ",").reduce(into: [:]) { sizesByDeviceID, rawPair in
            let parts = rawPair.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return }
            let deviceID = normalizedDeviceID(parts[0])
            let sizeName = parts[1].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard deviceID.count == 4,
                  deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let size = OverlayThemeSize(rawValue: sizeName) else { return }
            sizesByDeviceID[deviceID] = size
        }
    }

    func themeSize(for deviceID: String?) -> OverlayThemeSize {
        guard let deviceID else { return .big }
        return deviceThemeSizes[Self.normalizedDeviceID(deviceID)] ?? .big
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
        deviceThemeSizes.removeValue(forKey: deviceID)
        deviceOutputProfiles.removeValue(forKey: deviceID)
        deviceXiaomiSettings.removeValue(forKey: deviceID)
        deviceInteractionSettings.removeValue(forKey: deviceID)
        deviceEncoderSettings.removeValue(forKey: deviceID)
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

    // ---- 设备交互/编码器设置（对齐 Windows Parse{Interaction,Encoder}Settings）----

    /// 返回设备有效交互设置：有覆盖返回覆盖（加载时已用全局默认填平），否则全局默认。
    func interactionSettings(for deviceID: String?) -> InteractionSettings {
        guard let deviceID,
              let settings = deviceInteractionSettings[Self.normalizedDeviceID(deviceID)] else {
            return interactionSettings
        }
        return settings
    }

    /// 返回设备有效编码器设置：有覆盖返回覆盖（加载时已用全局默认填平），否则全局默认。
    func encoderSettings(for deviceID: String?) -> EncoderSettings {
        guard let deviceID,
              let settings = deviceEncoderSettings[Self.normalizedDeviceID(deviceID)] else {
            return encoderSettings
        }
        return settings
    }

    /// 对齐 Windows：按键字段仅当 ParseKeySpec 成功才覆盖 fallback；press_key 唯一允许空。
    private static func keySpecValue(_ text: String?, fallback: String, allowEmpty: Bool = false) -> String {
        guard let text else { return fallback }
        if text.isEmpty { return allowEmpty ? text : fallback }
        return KeySpec.parse(text) != nil ? text : fallback
    }

    /// 顶层 interaction 键解析（对齐 Windows Load 顶层分支）。imu_wake_sensitivity 例外：
    /// 对齐 ImuWakeSensitivityFromName，非法值回 low 而不是保留 fallback。
    private static func interactionSettingsValue(
        imuWakeSensitivity: String?, tapToArrow: Bool?, tapSensitivity: Int?,
        airMouseSensitivityX: Int?, airMouseSensitivityY: Int?,
        default fallback: InteractionSettings
    ) -> InteractionSettings {
        var settings = fallback
        if let value = imuWakeSensitivity {
            settings.imuWakeSensitivity = ImuWakeSensitivity(rawValue: value) ?? .low
        }
        if let value = tapToArrow { settings.tapToArrow = value }
        if let value = tapSensitivity {
            settings.tapSensitivity = InteractionSettings.clampedSensitivity(value)
        }
        if let value = airMouseSensitivityX {
            settings.airMouseSensitivityX = InteractionSettings.clampedSensitivity(value)
        }
        if let value = airMouseSensitivityY {
            settings.airMouseSensitivityY = InteractionSettings.clampedSensitivity(value)
        }
        return settings
    }

    /// 顶层 encoder 键解析（对齐 Windows Load 顶层分支）：非法值保留 fallback。
    private static func encoderSettingsValue(
        toArrow: Bool?, rotationInvert: Bool?, rotateCwKey: String?, rotateCcwKey: String?,
        rotateFastThreshold: Int?, rotateCwFastKey: String?, rotateCcwFastKey: String?,
        rotateDecideWindowMs: Int?, ledColor: String?, pressAction: String?, pressKey: String?,
        doubleClickAction: String?, doubleClickKey: String?,
        default fallback: EncoderSettings
    ) -> EncoderSettings {
        var settings = fallback
        if let value = toArrow { settings.toArrow = value }
        if let value = rotationInvert { settings.rotationInvert = value }
        settings.rotateCwKey = keySpecValue(rotateCwKey, fallback: settings.rotateCwKey)
        settings.rotateCcwKey = keySpecValue(rotateCcwKey, fallback: settings.rotateCcwKey)
        if let value = rotateFastThreshold, value > 0 { settings.rotateFastThreshold = value }
        settings.rotateCwFastKey = keySpecValue(rotateCwFastKey, fallback: settings.rotateCwFastKey)
        settings.rotateCcwFastKey = keySpecValue(rotateCcwFastKey, fallback: settings.rotateCcwFastKey)
        if let value = rotateDecideWindowMs, value >= 0 { settings.rotateDecideWindowMs = value }
        if let value = ledColor, let color = EncoderLedColor(rawValue: value) {
            settings.ledColor = color
        }
        if let value = pressAction, let action = EncoderButtonAction(rawValue: value) {
            settings.pressAction = action
        }
        settings.pressKey = keySpecValue(pressKey, fallback: settings.pressKey, allowEmpty: true)
        if let value = doubleClickAction, let action = EncoderButtonAction(rawValue: value) {
            settings.doubleClickAction = action
        }
        settings.doubleClickKey = keySpecValue(doubleClickKey, fallback: settings.doubleClickKey)
        return settings
    }

    private static func interactionSettingsValue(
        _ file: ConfigFile, default fallback: InteractionSettings
    ) -> InteractionSettings {
        interactionSettingsValue(
            imuWakeSensitivity: file.imu_wake_sensitivity,
            tapToArrow: file.tap_to_arrow,
            tapSensitivity: file.tap_sensitivity,
            airMouseSensitivityX: file.air_mouse_sensitivity_x,
            airMouseSensitivityY: file.air_mouse_sensitivity_y,
            default: fallback
        )
    }

    private static func encoderSettingsValue(
        _ file: ConfigFile, default fallback: EncoderSettings
    ) -> EncoderSettings {
        encoderSettingsValue(
            toArrow: file.encoder_to_arrow,
            rotationInvert: file.encoder_rotation_invert,
            rotateCwKey: file.encoder_rotate_cw_key,
            rotateCcwKey: file.encoder_rotate_ccw_key,
            rotateFastThreshold: file.encoder_rotate_fast_threshold,
            rotateCwFastKey: file.encoder_rotate_cw_fast_key,
            rotateCcwFastKey: file.encoder_rotate_ccw_fast_key,
            rotateDecideWindowMs: file.encoder_rotate_decide_window_ms,
            ledColor: file.encoder_led_color,
            pressAction: file.encoder_press_action,
            pressKey: file.encoder_press_key,
            doubleClickAction: file.encoder_double_click_action,
            doubleClickKey: file.encoder_double_click_key,
            default: fallback
        )
    }

    /// legacy 逐行解析版：字符串值先转类型（非整数保留 fallback），语义与 TOML 版一致。
    private static func interactionSettingsValue(
        _ values: [String: String], default fallback: InteractionSettings
    ) -> InteractionSettings {
        interactionSettingsValue(
            imuWakeSensitivity: values["imu_wake_sensitivity"],
            tapToArrow: values["tap_to_arrow"].map { boolValue($0, default: fallback.tapToArrow) },
            tapSensitivity: values["tap_sensitivity"].flatMap(Int.init),
            airMouseSensitivityX: values["air_mouse_sensitivity_x"].flatMap(Int.init),
            airMouseSensitivityY: values["air_mouse_sensitivity_y"].flatMap(Int.init),
            default: fallback
        )
    }

    private static func encoderSettingsValue(
        _ values: [String: String], default fallback: EncoderSettings
    ) -> EncoderSettings {
        encoderSettingsValue(
            toArrow: values["encoder_to_arrow"].map { boolValue($0, default: fallback.toArrow) },
            rotationInvert: values["encoder_rotation_invert"].map {
                boolValue($0, default: fallback.rotationInvert)
            },
            rotateCwKey: values["encoder_rotate_cw_key"],
            rotateCcwKey: values["encoder_rotate_ccw_key"],
            rotateFastThreshold: values["encoder_rotate_fast_threshold"].flatMap(Int.init),
            rotateCwFastKey: values["encoder_rotate_cw_fast_key"],
            rotateCcwFastKey: values["encoder_rotate_ccw_fast_key"],
            rotateDecideWindowMs: values["encoder_rotate_decide_window_ms"].flatMap(Int.init),
            ledColor: values["encoder_led_color"],
            pressAction: values["encoder_press_action"],
            pressKey: values["encoder_press_key"],
            doubleClickAction: values["encoder_double_click_action"],
            doubleClickKey: values["encoder_double_click_key"],
            default: fallback
        )
    }

    private static func interactionSettings(
        from file: InteractionConfigFile, fallback: InteractionSettings
    ) -> InteractionSettings {
        interactionSettingsValue(
            imuWakeSensitivity: file.imu_wake_sensitivity,
            tapToArrow: file.tap_to_arrow,
            tapSensitivity: file.tap_sensitivity,
            airMouseSensitivityX: file.air_mouse_sensitivity_x,
            airMouseSensitivityY: file.air_mouse_sensitivity_y,
            default: fallback
        )
    }

    private static func encoderSettings(
        from file: EncoderConfigFile, fallback: EncoderSettings
    ) -> EncoderSettings {
        encoderSettingsValue(
            toArrow: file.to_arrow,
            rotationInvert: file.rotation_invert,
            rotateCwKey: file.rotate_cw_key,
            rotateCcwKey: file.rotate_ccw_key,
            rotateFastThreshold: file.rotate_fast_threshold,
            rotateCwFastKey: file.rotate_cw_fast_key,
            rotateCcwFastKey: file.rotate_ccw_fast_key,
            rotateDecideWindowMs: file.rotate_decide_window_ms,
            ledColor: file.led_color,
            pressAction: file.press_action,
            pressKey: file.press_key,
            doubleClickAction: file.double_click_action,
            doubleClickKey: file.double_click_key,
            default: fallback
        )
    }

    private static func deviceInteractionSettingsMap(
        _ devices: [String: DeviceConfigFile]?, fallback: InteractionSettings
    ) -> [String: InteractionSettings] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { map, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let interaction = pair.value.interaction else {
                return
            }
            map[deviceID] = interactionSettings(from: interaction, fallback: fallback)
        }
    }

    private static func deviceEncoderSettingsMap(
        _ devices: [String: DeviceConfigFile]?, fallback: EncoderSettings
    ) -> [String: EncoderSettings] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { map, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let encoder = pair.value.encoder else {
                return
            }
            map[deviceID] = encoderSettings(from: encoder, fallback: fallback)
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
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .auto }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    private var deviceOverlayPositionText: String {
        deviceOverlayPositions
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .bottomCenter }
            .sorted { $0.key < $1.key }
            .map { "\($0.key):\($0.value.rawValue)" }
            .joined(separator: ",")
    }

    private var deviceThemeSizeText: String {
        deviceThemeSizes
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .big }
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

    /// [device.<id>.interaction] 覆盖段（对齐 Windows Save：未配对或与全局默认一致
    /// 不写出；写出的表全量含 5 个字段，保证自含、加载顺序无关）。
    private var deviceInteractionSettingsText: String {
        deviceInteractionSettings
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != interactionSettings }
            .sorted { $0.key < $1.key }
            .map { deviceID, settings in
                """

                [device.\(deviceID).interaction]
                imu_wake_sensitivity = "\(settings.imuWakeSensitivity.rawValue)"
                tap_to_arrow = \(settings.tapToArrow.tomlValue)
                tap_sensitivity = \(settings.tapSensitivity)
                air_mouse_sensitivity_x = \(settings.airMouseSensitivityX)
                air_mouse_sensitivity_y = \(settings.airMouseSensitivityY)
                """
            }
            .joined(separator: "\n")
    }

    /// [device.<id>.encoder] 覆盖段（对齐 Windows Save：未配对或与全局默认一致
    /// 不写出；写出的表全量含 13 个字段（键名去 encoder_ 前缀），保证自含、加载顺序无关）。
    private var deviceEncoderSettingsText: String {
        deviceEncoderSettings
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != encoderSettings }
            .sorted { $0.key < $1.key }
            .map { deviceID, settings in
                """

                [device.\(deviceID).encoder]
                to_arrow = \(settings.toArrow.tomlValue)
                rotation_invert = \(settings.rotationInvert.tomlValue)
                rotate_cw_key = "\(settings.rotateCwKey.tomlEscaped)"
                rotate_ccw_key = "\(settings.rotateCcwKey.tomlEscaped)"
                rotate_fast_threshold = \(settings.rotateFastThreshold)
                rotate_cw_fast_key = "\(settings.rotateCwFastKey.tomlEscaped)"
                rotate_ccw_fast_key = "\(settings.rotateCcwFastKey.tomlEscaped)"
                rotate_decide_window_ms = \(settings.rotateDecideWindowMs)
                led_color = "\(settings.ledColor.rawValue)"
                press_action = "\(settings.pressAction.rawValue)"
                press_key = "\(settings.pressKey.tomlEscaped)"
                double_click_action = "\(settings.doubleClickAction.rawValue)"
                double_click_key = "\(settings.doubleClickKey.tomlEscaped)"
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
    var tencent_secret_id: String?
    var tencent_secret_key: String?
    var tencent_appid: String?
    var tencent_engine_model_type: String?
    var tencent_hotword_id: String?
    var llm_base_url: String?
    var llm_api_key: String?
    var llm_model: String?
    var refine_enabled: Bool?
    var refine_prompt: String?
    var llm_disable_thinking: Bool?
    var interaction_mode: String?
    var ui_language: String?
    var output_target: String?
    var text_transform: String?
    var translation_target: String?
    var resource_id: String?
    var asr_hotwords: String?
    var paired_device_ids: String?
    var device_theme_colors: String?
    var device_overlay_positions: String?
    var device_theme_sizes: String?
    var auto_enter: Bool?
    var debug_audio_cache: Bool?
    var debug_audio_dir: String?
    var paired_device: [String]?
    var xiaomi_suppress_f5: Bool?
    var global_hotkey_enabled: Bool?
    var global_hotkey: String?
    var launch_at_login: Bool?
    var developer_mode: Bool?
    var show_imu_debug: Bool?
    var imu_wake_sensitivity: String?
    var tap_to_arrow: Bool?
    var tap_sensitivity: Int?
    var air_mouse_sensitivity_x: Int?
    var air_mouse_sensitivity_y: Int?
    var encoder_to_arrow: Bool?
    var encoder_rotation_invert: Bool?
    var encoder_rotate_cw_key: String?
    var encoder_rotate_ccw_key: String?
    var encoder_rotate_fast_threshold: Int?
    var encoder_rotate_cw_fast_key: String?
    var encoder_rotate_ccw_fast_key: String?
    var encoder_rotate_decide_window_ms: Int?
    var encoder_led_color: String?
    var encoder_press_action: String?
    var encoder_press_key: String?
    var encoder_double_click_action: String?
    var encoder_double_click_key: String?
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
    var interaction: InteractionConfigFile?
    var encoder: EncoderConfigFile?
}

/// [device.<id>.interaction] 表：键名与顶层一致。
private struct InteractionConfigFile: Decodable {
    var imu_wake_sensitivity: String?
    var tap_to_arrow: Bool?
    var tap_sensitivity: Int?
    var air_mouse_sensitivity_x: Int?
    var air_mouse_sensitivity_y: Int?
}

/// [device.<id>.encoder] 表：键名去掉顶层 encoder_ 前缀。
private struct EncoderConfigFile: Decodable {
    var to_arrow: Bool?
    var rotation_invert: Bool?
    var rotate_cw_key: String?
    var rotate_ccw_key: String?
    var rotate_fast_threshold: Int?
    var rotate_cw_fast_key: String?
    var rotate_ccw_fast_key: String?
    var rotate_decide_window_ms: Int?
    var led_color: String?
    var press_action: String?
    var press_key: String?
    var double_click_action: String?
    var double_click_key: String?
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
