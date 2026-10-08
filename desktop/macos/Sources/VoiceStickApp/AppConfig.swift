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
    /// 默认开（对齐 Windows 默认），prompt 留空用内置默认）。
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
    /// 体感鼠标全局进阶参数（对齐 Windows 顶层键 air_mouse_*，默认值=真机标定）。
    var airMouse: AirMouseSettings
    /// [device.<id>.buttons] 按设备覆盖（小米遥控器按键映射；键为归一化 4 位大写 hex ID）。
    var deviceButtonsSettings: [String: ButtonsSettings]

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

    // ---- 内置凭据回退（对齐 Windows Active*()：配置值优先，空则回退编译期内置；
    // 不修改 config 字段、不落盘。内测构建经 BuiltinSecrets 预打包，公开构建全空。）----

    var activeVolcengineAPIKey: String {
        resolveActiveString(volcengineAPIKey, builtin: BuiltinSecrets.volcengineAPIKey)
    }

    var activeTencentSecretID: String {
        resolveActiveString(tencentSecretID, builtin: BuiltinSecrets.tencentSecretID)
    }

    var activeTencentSecretKey: String {
        resolveActiveString(tencentSecretKey, builtin: BuiltinSecrets.tencentSecretKey)
    }

    var activeTencentAppid: String {
        resolveActiveString(tencentAppid, builtin: BuiltinSecrets.tencentAppid)
    }

    var activeLlmAPIKey: String {
        resolveActiveString(llmAPIKey, builtin: BuiltinSecrets.llmAPIKey)
    }

    var activeLlmBaseURL: String {
        resolveActiveString(llmBaseURL, builtin: BuiltinSecrets.llmBaseURL)
    }

    var activeLlmModel: String {
        resolveActiveString(llmModel, builtin: BuiltinSecrets.llmModel)
    }

    /// 首启引导（内测包开箱即用，对齐 Windows「内置 key 跳过 onboarding」）：
    /// config.toml 不存在且内置凭据带可用 ASR provider 时，落一份默认配置
    ///（provider 指向内置凭据方；不带任何密钥——Active 层运行时回退），
    /// 使 applicationDidFinishLaunching 走 startApp 而非 onboarding 向导。
    /// 公开构建（无内置凭据）保持原行为不变。
    static func bootstrapBuiltinDefaultsIfNeeded() {
        guard !configExists, let provider = BuiltinSecrets.builtinProvider else { return }
        var defaults = AppConfig.defaults
        defaults.asrProvider = provider
        defaults.resourceID = resourceIDValue(defaults.resourceID, default: defaults.resourceID)
        try? defaults.save()
        NSLog("BuiltinSecrets: bootstrapped default config with provider \(provider.rawValue)")
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
            refineEnabled: true,
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
            deviceEncoderSettings: [:],
            airMouse: .default,
            deviceButtonsSettings: [:]
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
            defaultOutputProfile: VoiceStickCore.outputProfile(
                target: file.output?.target ?? file.output_target,
                transform: file.output?.transform ?? file.text_transform,
                translationTarget: file.output?.translation_target ?? file.translation_target,
                default: defaults.defaultOutputProfile
            ),
            deviceOutputProfiles: deviceOutputProfileMap(
                file.device,
                defaultProfile: VoiceStickCore.outputProfile(
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
            deviceEncoderSettings: deviceEncoderSettingsMap(file.device, fallback: encoder),
            airMouse: airMouseSettingsValue(
                tau: file.air_mouse_tau,
                invertY: file.air_mouse_invert_y,
                curveLowThresh: file.air_mouse_curve_low_thresh,
                curveHighThresh: file.air_mouse_curve_high_thresh,
                curveLowFactor: file.air_mouse_curve_low_factor,
                curveHighFactor: file.air_mouse_curve_high_factor,
                neutralDeadzone: file.air_mouse_neutral_deadzone,
                controlMode: file.air_mouse_control_mode,
                rateGain: file.air_mouse_rate_gain,
                rateFriction: file.air_mouse_rate_friction,
                rateMaxSpeed: file.air_mouse_rate_max_speed,
                default: defaults.airMouse
            ),
            deviceButtonsSettings: deviceButtonsSettingsMap(file.device)
        )
        recoverTencentSecretID(&config)
        return config
    }

    /// air_mouse_* 顶层键解析（对齐 Windows：越界/非法回落默认，见 AirMouseSettings.applyClamps）。
    private static func airMouseSettingsValue(
        tau: Double?, invertY: Bool?,
        curveLowThresh: Double?, curveHighThresh: Double?,
        curveLowFactor: Double?, curveHighFactor: Double?,
        neutralDeadzone: Double?, controlMode: String?,
        rateGain: Double?, rateFriction: Double?, rateMaxSpeed: Double?,
        default fallback: AirMouseSettings
    ) -> AirMouseSettings {
        var settings = fallback
        if let value = tau { settings.tau = AirMouseKin.tauClamp(value) }
        if let value = invertY { settings.invertY = value }
        if let value = curveLowThresh { settings.curveLowThresh = value }
        if let value = curveHighThresh { settings.curveHighThresh = value }
        if let value = curveLowFactor { settings.curveLowFactor = value }
        if let value = curveHighFactor { settings.curveHighFactor = value }
        if let value = neutralDeadzone { settings.neutralDeadzone = AirMouseKin.neutralDeadzoneClamp(value) }
        if let value = controlMode { settings.controlMode = AirMouseControlMode.fromName(value).name }
        if let value = rateGain { settings.rateGain = AirMouseKin.rateGainClamp(value) }
        if let value = rateFriction { settings.rateFriction = AirMouseKin.rateFrictionClamp(value) }
        if let value = rateMaxSpeed { settings.rateMaxSpeed = AirMouseKin.rateMaxSpeedClamp(value) }
        return settings
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
        air_mouse_tau = \(airMouse.tomlNumber(airMouse.tau))
        air_mouse_invert_y = \(airMouse.invertY.tomlValue)
        air_mouse_curve_low_thresh = \(airMouse.tomlNumber(airMouse.curveLowThresh))
        air_mouse_curve_high_thresh = \(airMouse.tomlNumber(airMouse.curveHighThresh))
        air_mouse_curve_low_factor = \(airMouse.tomlNumber(airMouse.curveLowFactor))
        air_mouse_curve_high_factor = \(airMouse.tomlNumber(airMouse.curveHighFactor))
        air_mouse_neutral_deadzone = \(airMouse.tomlNumber(airMouse.neutralDeadzone))
        air_mouse_control_mode = "\(airMouse.controlMode.tomlEscaped)"
        air_mouse_rate_gain = \(airMouse.tomlNumber(airMouse.rateGain))
        air_mouse_rate_friction = \(airMouse.tomlNumber(airMouse.rateFriction))
        air_mouse_rate_max_speed = \(airMouse.tomlNumber(airMouse.rateMaxSpeed))
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
            + deviceButtonsSettingsText
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
            defaultOutputProfile: VoiceStickCore.outputProfile(
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
            deviceEncoderSettings: [:],
            airMouse: airMouseSettingsValue(
                tau: values["air_mouse_tau"].flatMap(Double.init),
                invertY: values["air_mouse_invert_y"].map { boolValue($0, default: defaults.airMouse.invertY) },
                curveLowThresh: values["air_mouse_curve_low_thresh"].flatMap(Double.init),
                curveHighThresh: values["air_mouse_curve_high_thresh"].flatMap(Double.init),
                curveLowFactor: values["air_mouse_curve_low_factor"].flatMap(Double.init),
                curveHighFactor: values["air_mouse_curve_high_factor"].flatMap(Double.init),
                neutralDeadzone: values["air_mouse_neutral_deadzone"].flatMap(Double.init),
                controlMode: values["air_mouse_control_mode"],
                rateGain: values["air_mouse_rate_gain"].flatMap(Double.init),
                rateFriction: values["air_mouse_rate_friction"].flatMap(Double.init),
                rateMaxSpeed: values["air_mouse_rate_max_speed"].flatMap(Double.init),
                default: defaults.airMouse
            ),
            deviceButtonsSettings: [:]
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




    private static func resourceIDValue(_ text: String?, default defaultValue: String) -> String {
        guard let text, supportedResourceIDs.contains(text) else { return defaultValue }
        return text
    }

    /// N1：实现已下沉 VoiceStickCore（ConfigParsing.swift）；保留静态门面，
    /// 全仓 AppConfig.normalizedDeviceID / Self.normalizedDeviceID 调用点零改动。
    static func normalizedDeviceID(_ text: String) -> String {
        VoiceStickCore.normalizedDeviceID(text)
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
    // N1：parsePairedDeviceEntry / formatPairedDeviceEntry / pairedDeviceEntryList
    // 已下沉 VoiceStickCore（ConfigParsing.swift），非限定调用经 import 解析。

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
        deviceButtonsSettings.removeValue(forKey: deviceID)
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
    // ---- 小米遥控器 [device.<id>.buttons] 覆盖（按键映射）----

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

    // ---- 小米遥控器按键映射（[device.<id>.buttons]，语义对齐 Windows [xiaomi.keys]）----

    /// 全局默认：拦截关闭、全部原生（映射是设备级概念，无顶层全局覆盖表）。
    var buttonsSettings: ButtonsSettings { ButtonsSettings.default }

    /// 返回设备有效按键映射：有覆盖返回覆盖（加载时已校验回填），否则全局默认。
    func buttonsSettings(for deviceID: String?) -> ButtonsSettings {
        guard let deviceID,
              let settings = deviceButtonsSettings[Self.normalizedDeviceID(deviceID)] else {
            return buttonsSettings
        }
        return settings
    }

    /// 按前台应用合并 app 级覆盖（三期）。v1 无 app 级覆盖存储，等价设备有效值；
    /// AppDelegate 每次前台切换经此 resolve，将来加 app 覆盖表只改这里。
    func effectiveButtonsSettings(for deviceID: String?, activeApp: String) -> ButtonsSettings {
        buttonsSettings(for: deviceID)
    }

    /// 对齐 Windows：按键字段仅当 ParseKeySpec 成功才覆盖 fallback；press_key 唯一允许空。

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
            .map { "  \"\(formatPairedDeviceEntry($0).tomlEscaped)\"," }
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

    /// [device.<id>.buttons] 覆盖段（未配对或与默认一致不写出；只写出非原生条目，
    /// 自含、加载顺序无关）。
    private var deviceButtonsSettingsText: String {
        deviceButtonsSettings
            .filter { pairedDeviceIDs.contains($0.key) && $0.value != .default }
            .sorted { $0.key < $1.key }
            .map { deviceID, settings -> String in
                var text = "\n\n[device.\(deviceID).buttons]"
                if settings.intercept {
                    text += "\nintercept = true"
                }
                let keys = settings.mappings
                    .sorted { $0.key < $1.key }
                    .compactMap { key, mapping -> String? in
                        switch mapping.action {
                        case .native:
                            return nil
                        case .disabled:
                            return "\(key) = \"\(buttonsDisabledSentinel)\""
                        case .key:
                            return "\(key) = \"\(mapping.key.tomlEscaped)\""
                        }
                    }
                if !keys.isEmpty {
                    text += "\n\n[device.\(deviceID).buttons.keys]\n"
                    text += keys.joined(separator: "\n")
                }
                return text
            }
            .joined()
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

