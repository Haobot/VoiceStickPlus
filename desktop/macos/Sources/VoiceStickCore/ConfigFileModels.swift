// N1 第三刀：TOML/配置文件模型闭包下沉（纯 Decodable，无需 TOMLKit 入 Core）——
// ConfigFile 根模型 + Output/Device/Buttons/Interaction/Encoder/Xiaomi 子表 +
// toml 转义助手（App 的 load 侧 TOMLDecoder 解码、save 侧字符串构建共用）+
// xiaomi 纯转换器（依赖集合 ⊆ Core：模型/normalizedDeviceID/XiaomiSettings）。
// interaction/encoder/buttons/output 转换器因依赖 App 域类型（KeySpec 等）暂留 AppConfig。

public struct ConfigFile: Decodable {
    public var asr_provider: String?
    public var voicestick_api_key: String?
    public var voicestick_cloud_url: String?
    public var volcengine_api_key: String?
    public var api_key: String?
    public var tencent_secret_id: String?
    public var tencent_secret_key: String?
    public var tencent_appid: String?
    public var tencent_engine_model_type: String?
    public var tencent_hotword_id: String?
    public var llm_base_url: String?
    public var llm_api_key: String?
    public var llm_model: String?
    public var refine_enabled: Bool?
    public var refine_prompt: String?
    public var llm_disable_thinking: Bool?
    public var interaction_mode: String?
    public var ui_language: String?
    public var output_target: String?
    public var text_transform: String?
    public var translation_target: String?
    public var resource_id: String?
    public var asr_hotwords: String?
    public var paired_device_ids: String?
    public var device_theme_colors: String?
    public var device_overlay_positions: String?
    public var device_theme_sizes: String?
    public var auto_enter: Bool?
    public var debug_audio_cache: Bool?
    public var debug_audio_dir: String?
    public var paired_device: [String]?
    public var xiaomi_suppress_f5: Bool?
    public var global_hotkey_enabled: Bool?
    public var global_hotkey: String?
    public var launch_at_login: Bool?
    public var developer_mode: Bool?
    public var show_imu_debug: Bool?
    public var imu_wake_sensitivity: String?
    public var tap_to_arrow: Bool?
    public var tap_sensitivity: Int?
    public var air_mouse_sensitivity_x: Int?
    public var air_mouse_sensitivity_y: Int?
    public var air_mouse_tau: Double?
    public var air_mouse_invert_y: Bool?
    public var air_mouse_curve_low_thresh: Double?
    public var air_mouse_curve_high_thresh: Double?
    public var air_mouse_curve_low_factor: Double?
    public var air_mouse_curve_high_factor: Double?
    public var air_mouse_neutral_deadzone: Double?
    public var air_mouse_control_mode: String?
    public var air_mouse_rate_gain: Double?
    public var air_mouse_rate_friction: Double?
    public var air_mouse_rate_max_speed: Double?
    public var encoder_to_arrow: Bool?
    public var encoder_rotation_invert: Bool?
    public var encoder_rotate_cw_key: String?
    public var encoder_rotate_ccw_key: String?
    public var encoder_rotate_fast_threshold: Int?
    public var encoder_rotate_cw_fast_key: String?
    public var encoder_rotate_ccw_fast_key: String?
    public var encoder_rotate_decide_window_ms: Int?
    public var encoder_led_color: String?
    public var encoder_press_action: String?
    public var encoder_press_key: String?
    public var encoder_double_click_action: String?
    public var encoder_double_click_key: String?
    public var output: OutputConfigFile?
    public var device: [String: DeviceConfigFile]?
}

public struct OutputConfigFile: Decodable {
    public var target: String?
    public var transform: String?
    public var translation_target: String?
}

public struct DeviceConfigFile: Decodable {
    public var output: OutputConfigFile?
    public var xiaomi: XiaomiConfigFile?
    public var interaction: InteractionConfigFile?
    public var encoder: EncoderConfigFile?
    public var buttons: ButtonsConfigFile?

    /// 跨模块构造（测试与转换器侧）；解码走 Decodable init(from:)。
    public init(output: OutputConfigFile? = nil,
                xiaomi: XiaomiConfigFile? = nil,
                interaction: InteractionConfigFile? = nil,
                encoder: EncoderConfigFile? = nil,
                buttons: ButtonsConfigFile? = nil) {
        self.output = output
        self.xiaomi = xiaomi
        self.interaction = interaction
        self.encoder = encoder
        self.buttons = buttons
    }
}

/// [device.<id>.buttons] 表：intercept + keys 子表（键为 RemoteButton rawValue，
/// 值为 KeySpec 文本或 "none"=禁用）。
public struct ButtonsConfigFile: Decodable {
    public var intercept: Bool?
    public var keys: [String: String]?
}

/// [device.<id>.interaction] 表：键名与顶层一致。
public struct InteractionConfigFile: Decodable {
    public var imu_wake_sensitivity: String?
    public var tap_to_arrow: Bool?
    public var tap_sensitivity: Int?
    public var air_mouse_sensitivity_x: Int?
    public var air_mouse_sensitivity_y: Int?
}

/// [device.<id>.encoder] 表：键名去掉顶层 encoder_ 前缀。
public struct EncoderConfigFile: Decodable {
    public var to_arrow: Bool?
    public var rotation_invert: Bool?
    public var rotate_cw_key: String?
    public var rotate_ccw_key: String?
    public var rotate_fast_threshold: Int?
    public var rotate_cw_fast_key: String?
    public var rotate_ccw_fast_key: String?
    public var rotate_decide_window_ms: Int?
    public var led_color: String?
    public var press_action: String?
    public var press_key: String?
    public var double_click_action: String?
    public var double_click_key: String?
}

/// [device.<id>.xiaomi] 表。gain_db 宽容接受 TOML 整数（Windows C++ << 对整数值
/// double 会写出 "gain_db = 18" 这种整数形态），double_click_ms 严格整数。
public struct XiaomiConfigFile: Decodable {
    public var gain_db: Double?
    public var double_click_ms: Int?

    private enum CodingKeys: String, CodingKey {
        case gain_db = "gain_db"
        case double_click_ms = "double_click_ms"
    }

    /// 跨模块构造（测试与转换器侧）；解码走 Decodable init(from:)。
    public init(gain_db: Double? = nil, double_click_ms: Int? = nil) {
        self.gain_db = gain_db
        self.double_click_ms = double_click_ms
    }

    public init(from decoder: Decoder) throws {
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

public extension Bool {
    public var tomlValue: String { self ? "true" : "false" }
}

public extension String {
    public var tomlEscaped: String {
        replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
    }
}
    public func xiaomiSettings(from file: XiaomiConfigFile,
                                       fallback: XiaomiSettings = .default) -> XiaomiSettings {
        var settings = fallback
        if let gainDb = file.gain_db { settings.gainDb = gainDb }
        if let doubleClickMs = file.double_click_ms, doubleClickMs > 0 {
            settings.doubleClickMs = doubleClickMs
        }
        return settings
    }

    public func deviceXiaomiSettingsMap(
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

