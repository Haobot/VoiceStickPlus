// N1 第四刀：配置域类型 + KeySpec + 纯转换器下迁。displayName（tr 本地化）留 App
// 侧 extension（DomainDisplayNames.swift）——本地化是 UI 层关注点；核心纯数据+解析。

// ---- 配置域类型（原 AppConfig.swift 103 块，displayName 已剥离） ----

public enum OutputTarget: String, CaseIterable {
    case focusedApp = "focused_app"
    case subtitle

}

public enum TextTransform: String, CaseIterable {
    case original
    case translate

}

public struct OutputProfile: Equatable {
    public init(target: OutputTarget, transform: TextTransform, translationTarget: String) {
        self.target = target
        self.transform = transform
        self.translationTarget = translationTarget
    }

    public var target: OutputTarget
    public var transform: TextTransform
    public var translationTarget: String

    public static let `default` = OutputProfile(
        target: .focusedApp,
        transform: .original,
        translationTarget: "en"
    )

    public var usesSubtitleASR: Bool {
        target == .subtitle
    }
}

// N1：PairedDeviceEntry / XiaomiSettings 已下沉 VoiceStickCore（ConfigParsing.swift），
// 本文件经既有 import VoiceStickCore 可见，调用点零改动。

/// IMU 唤醒灵敏度（对齐 Windows ImuWakeSensitivity）：low/medium/high。
/// 阈值映射注意是反向的：灵敏度越高阈值越低（low=800 / medium=500 / high=250 lsb）。
public enum ImuWakeSensitivity: String, CaseIterable {
    case low
    case medium
    case high

    public var thresholdLsb: Int {
        switch self {
        case .low: return 800
        case .medium: return 500
        case .high: return 250
        }
    }

}

/// 设备交互设置（对齐 Windows InteractionSettings；[device.<id>.interaction] 覆盖表
/// 键名与顶层一致，加载时以全局默认填平）。体感鼠标 X/Y 灵敏度保留字段（跨端配置
/// 兼容），macOS 暂无体感鼠标消费侧。
public struct InteractionSettings: Equatable {
    public init() {}

    public var imuWakeSensitivity: ImuWakeSensitivity = .low
    public var tapToArrow = false
    /// 1...10；加载时越界值回落 5（对齐 Windows TapSensitivityClamp）。
    public var tapSensitivity = 5
    public var airMouseSensitivityX = 5
    public var airMouseSensitivityY = 5

    public static let `default` = InteractionSettings()

    /// 对齐 Windows TapSensitivityClamp/AirMouseSensitivityClamp：不在 1...10 回落 5。
    public static func clampedSensitivity(_ value: Int) -> Int {
        (1...10).contains(value) ? value : 5
    }
}

/// 编码器录音灯颜色（对齐 Windows encoder_led_color 8 色枚举，顺序固定）。
public enum EncoderLedColor: String, CaseIterable {
    case red
    case green
    case blue
    case yellow
    case purple
    case cyan
    case white
    case off

}

/// 编码器单击/双击动作：recording=录音语义（press 路由主键 / double-click 远程起停），
/// key=自定义按键注入（对齐 Windows EncoderSettings press_action/double_click_action）。
public enum EncoderButtonAction: String, CaseIterable {
    case recording
    case key

}

/// 编码器设置（对齐 Windows EncoderSettings；[device.<id>.encoder] 覆盖表键名去掉
/// encoder_ 前缀，加载时以全局默认填平）。按键字段为 key_spec 语法字符串
/// （见 KeySpec.parse）；加载校验失败的字段保留 fallback。
public struct EncoderSettings: Equatable {
    public init() {}

    /// 旋转注入总开关（false 时旋转事件整段忽略）。默认开。
    public var toArrow = true
    /// 旋转方向翻转（cw/ccw 键互换）。默认关。
    public var rotationInvert = false
    public var rotateCwKey = "down"
    public var rotateCcwKey = "up"
    /// 快慢分档阈值（格/秒）：EWMA 估计速度 >= 阈值走快速键；<=0 关闭分档。
    /// 加载要求 >0（存 0 下次加载回默认）。默认 200。
    public var rotateFastThreshold = 200
    public var rotateCwFastKey = "pagedown"
    public var rotateCcwFastKey = "pageup"
    /// 慢速判定窗口（ms）：0=立即注入（旧行为）；加载要求 >=0。默认 80。
    public var rotateDecideWindowMs = 80
    public var ledColor: EncoderLedColor = .red
    public var pressAction: EncoderButtonAction = .recording
    /// press_action=key 时的注入键；唯一显式允许为空的按键字段。
    public var pressKey = ""
    public var doubleClickAction: EncoderButtonAction = .key
    public var doubleClickKey = "enter"

    public static let `default` = EncoderSettings()
}

/// 体感鼠标全局进阶参数（对齐 Windows app_config.h air_mouse_* 顶层键；灵敏度档位
/// 在 InteractionSettings.airMouseSensitivityX/Y，按设备覆盖）。键名与 Windows 一致，
/// 配置文件跨端通用；数值钳位对齐 Windows（越界回落默认值，见 AirMouseKin）。
public struct AirMouseSettings: Equatable {
    public init() {}

    /// 速度环时间常数（秒），[0.02, 0.5]，默认 0.05。
    public var tau: Double = 0.05
    public var invertY: Bool = false
    /// sigmoid 增益曲线特征点（单位=固件缩放角速率 dps×4）。默认 100/333/0.25/4.0（真机标定）。
    public var curveLowThresh: Double = 100.0
    public var curveHighThresh: Double = 333.0
    public var curveLowFactor: Double = 0.25
    public var curveHighFactor: Double = 4.0
    /// 方向锁中立区死区，[1, 10]，默认 3.0。
    public var neutralDeadzone: Double = 3.0
    /// 控制模式："angle"（角速率→速度）/"rate"（飞行摇杆），未知名回落 rate（对齐 Windows）。
    public var controlMode: String = "rate"
    /// rate 模式参数：加速度增益 [10,500]、摩擦 [0,0.5]、速度上限 [500,8000]。
    public var rateGain: Double = 80.0
    public var rateFriction: Double = 0.05
    public var rateMaxSpeed: Double = 4000.0

    public static let `default` = AirMouseSettings()

    /// TOML 序列化数字格式（4 位有效数字，避免科学计数法）。整数值必须带小数点落盘
    ///（`%.4g` 对 3 输出 "3" = TOML 整数）：TOMLKit 解 Double 严格按类型，整数形态
    /// 曾让整表解码失败、静默回落 legacy 丢掉全部 [表] 段（LenientDouble 是读侧双保险，
    /// 此处保证新写出为规范 float 形态）。
    public func tomlNumber(_ value: Double) -> String {
        let text = String(format: "%.4g", value)
        let lowered = text.lowercased()
        if lowered == "inf" || lowered == "-inf" || lowered == "nan" {
            return text
        }
        if text.contains(".") || lowered.contains("e") {
            return text
        }
        return text + ".0"
    }

    /// 解析时钳位（对齐 Windows 加载路径：越界回落默认值）。
    public mutating func applyClamps() {
        tau = AirMouseKin.tauClamp(tau)
        neutralDeadzone = AirMouseKin.neutralDeadzoneClamp(neutralDeadzone)
        rateGain = AirMouseKin.rateGainClamp(rateGain)
        rateFriction = AirMouseKin.rateFrictionClamp(rateFriction)
        rateMaxSpeed = AirMouseKin.rateMaxSpeedClamp(rateMaxSpeed)
        controlMode = AirMouseControlMode.fromName(controlMode).name
    }
}

/// 语音编辑指令配置（[voice_edit] 表，方案 §9.1；字段与默认值双端同名同默认）。
/// 只有 default_chars/max_chars/scope_seconds/verify_mode/unverified_seconds 影响
/// 解析与校验；enabled=false 时完全回到现状（不解析、不拦截）。
public struct VoiceEditSettings: Equatable {
    public init() {}

    /// 总开关（决策点 D2：默认开，开箱即用）。
    public var enabled = true
    /// 单独说「删除」时删几个字（决策点 D1：默认 1，可连说）。
    public var defaultChars = 1
    /// 单条指令允许删除的最大字数；显式数量超限按普通文本上屏。
    public var maxChars = 20
    /// L0 作用域时间窗（秒）：只允许删除最近 N 秒内的语音注入。
    public var scopeSeconds = 120
    /// L1 校验模式（决策点 D8：默认 auto，本机 spike 覆盖良好后可调 strict）。
    public var verifyMode: VoiceEditVerifyMode = .auto
    /// auto 模式下不可验证时的允许窗口（秒）：短窗 + 前台应用一致才允许盲删。
    public var unverifiedSeconds = 8

    public static let `default` = VoiceEditSettings()

    /// 加载钳位范围（方案 §9.1）：与 Windows app_config.h 的 kVoiceEdit*Min/Max 完全
    /// 一致（max_chars 1..50），加载时按区间钳位（不是回落默认），保证同一份
    /// config.toml 跨端行为一致。
    public static let defaultCharsRange = 1...10
    public static let maxCharsRange = 1...50
    public static let scopeSecondsRange = 10...600
    public static let unverifiedSecondsRange = 0...30
}

// ---- KeySpec（原 VoiceStickApp/KeySpec.swift） ----

import CoreGraphics
import Foundation

/// 一次按键注入的规格（对齐 Windows key_spec.h 的 KeySpec/ParseKeySpec）：
/// 修饰键（Ctrl/Alt/Shift/Win 固定序；macOS 上映射 ⌃/⌥/⇧/⌘）+ 主键 + 规范化显示文本
/// （如 "Ctrl+Shift+V"）。供编码器旋转/单击/双击的自定义按键注入使用。
///
/// 语法：单键（up/down/left/right/enter|return/esc|escape/tab/space/backspace/delete/
/// insert/pageup/pagedown/home/end/volumeup/volumedown/volumemute/f1-f24/单字符 A-Z0-9）
/// 或修饰键组合（ctrl|control/alt/shift/win|windows|meta + 单键，"+" 分隔，
/// 大小写与前后空白不敏感）。仅修饰键、未知键名、多个主键均解析失败。
public struct KeySpec: Equatable {
    /// 音量键在 macOS 上没有虚拟键码，走 NX_SYSDEFINED 媒体键事件。
    public enum MediaKey: Equatable {
        case volumeUp
        case volumeDown
        case volumeMute
    }

    public var control = false
    public var option = false   // Alt
    public var shift = false
    public var command = false  // Win
    /// 普通键的 CGKeyCode；媒体键为 nil（看 mediaKey）。
    /// f21-f24 语法合法但 macOS 无对应虚拟键码：解析放行（配置跨端兼容），keyCode 为 nil，注入侧忽略。
    public let keyCode: CGKeyCode?
    public let mediaKey: MediaKey?
    public let displayText: String

    /// 跨模块构造（InputInjector 等注入固定键位用）；解析走 parse。
    public init(control: Bool = false, option: Bool = false, shift: Bool = false,
                command: Bool = false, keyCode: CGKeyCode?, mediaKey: MediaKey?,
                displayText: String) {
        self.control = control
        self.option = option
        self.shift = shift
        self.command = command
        self.keyCode = keyCode
        self.mediaKey = mediaKey
        self.displayText = displayText
    }

    public static func parse(_ text: String) -> KeySpec? {
        var modifiers: (ctrl: Bool, alt: Bool, shift: Bool, win: Bool) = (false, false, false, false)
        var main: (keyCode: CGKeyCode?, mediaKey: MediaKey?, display: String)?

        for rawPart in text.split(separator: "+", omittingEmptySubsequences: false) {
            let part = rawPart.trimmingCharacters(in: .whitespaces).lowercased()
            if part.isEmpty { return nil }

            switch part {
            case "ctrl", "control":
                if modifiers.ctrl { return nil }
                modifiers.ctrl = true
            case "alt":
                if modifiers.alt { return nil }
                modifiers.alt = true
            case "shift":
                if modifiers.shift { return nil }
                modifiers.shift = true
            case "win", "windows", "meta":
                if modifiers.win { return nil }
                modifiers.win = true
            default:
                if main != nil { return nil }  // 多个主键
                guard let resolved = resolveMainKey(part) else { return nil }
                main = resolved
            }
        }
        guard let main else { return nil }  // 仅修饰键

        var display = ""
        if modifiers.ctrl { display += "Ctrl+" }
        if modifiers.alt { display += "Alt+" }
        if modifiers.shift { display += "Shift+" }
        if modifiers.win { display += "Win+" }
        display += main.display

        return KeySpec(
            control: modifiers.ctrl,
            option: modifiers.alt,
            shift: modifiers.shift,
            command: modifiers.win,
            keyCode: main.keyCode,
            mediaKey: main.mediaKey,
            displayText: display
        )
    }

    /// 主键名 → 键码/媒体键 + 显示名（对齐 Windows MainKeyVkey + VkeyDisplayName）。
    private static func resolveMainKey(_ lower: String) -> (CGKeyCode?, MediaKey?, String)? {
        if lower.count == 1, let char = lower.uppercased().first,
           let code = alphanumericKeyCodes[char] {
            return (code, nil, String(char))
        }
        switch lower {
        case "volumeup": return (nil, .volumeUp, "VolumeUp")
        case "volumedown": return (nil, .volumeDown, "VolumeDown")
        case "volumemute": return (nil, .volumeMute, "VolumeMute")
        default: break
        }
        if let (code, name) = namedKeys[lower] {
            return (code, nil, name)
        }
        if lower.hasPrefix("f"), lower.count >= 2 {
            let digits = lower.dropFirst()
            guard digits.allSatisfy({ $0.isNumber }), let num = Int(digits), num >= 1, num <= 24 else {
                return nil
            }
            return (functionKeyCodes[num], nil, "F\(num)")
        }
        return nil
    }

    // MARK: - 键码表（ANSI 布局）

    private static let alphanumericKeyCodes: [Character: CGKeyCode] = [
        "A": 0, "S": 1, "D": 2, "F": 3, "H": 4, "G": 5, "Z": 6, "X": 7,
        "C": 8, "V": 9, "B": 11, "Q": 12, "W": 13, "E": 14, "R": 15,
        "Y": 16, "T": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
        "5": 23, "9": 25, "7": 26, "8": 28, "0": 29, "O": 31, "U": 32,
        "I": 34, "P": 35, "L": 37, "J": 38, "K": 40, "N": 45, "M": 46
    ]

    private static let namedKeys: [String: (CGKeyCode, String)] = [
        "space": (49, "Space"),
        "enter": (36, "Enter"), "return": (36, "Enter"),
        "esc": (53, "Esc"), "escape": (53, "Esc"),
        "tab": (48, "Tab"),
        "backspace": (51, "Backspace"),
        "delete": (117, "Delete"),   // 前进删除（Windows VK_DELETE 语义）
        "insert": (114, "Insert"),   // kVK_Help，PC 键盘上即 Insert
        "up": (126, "Up"), "down": (125, "Down"),
        "left": (123, "Left"), "right": (124, "Right"),
        "pageup": (116, "PageUp"), "pagedown": (121, "PageDown"),
        "home": (115, "Home"), "end": (119, "End")
    ]

    private static let functionKeyCodes: [Int: CGKeyCode] = [
        1: 122, 2: 120, 3: 99, 4: 118, 5: 96, 6: 97, 7: 98, 8: 100,
        9: 101, 10: 109, 11: 103, 12: 111, 13: 105, 14: 107, 15: 113,
        16: 106, 17: 64, 18: 79, 19: 80, 20: 90
    ]
}

// ---- 纯转换器（buttons/interaction/encoder/output；实例读取方法留 App） ----

    /// keys 值为 "none" 时表示禁用（TOML 无法表达动作枚举，用哨兵值区分
    /// action=disabled 与 action=key；空串/非法值回落 native）。
    public let buttonsDisabledSentinel = "none"

    /// 解析 [device.<id>.buttons] 表：keys 值为 KeySpec 文本（action=key）或
    /// "none"（action=disabled）；未写出的键回落 native；KeySpec 非法的条目忽略。
    public func buttonsSettings(from file: ButtonsConfigFile) -> ButtonsSettings {
        var settings = ButtonsSettings.default
        if let intercept = file.intercept { settings.intercept = intercept }
        // 节拍解析侧钳位：越界回落默认（对齐 AirMouseSettings.applyClamps 语义；
        // init 也有同款防线，此处保证解析路径独立成立）。
        if let delay = file.repeat_delay_ms {
            settings.repeatDelayMs = ButtonsSettings.repeatDelayRange.contains(delay)
                ? delay : ButtonsSettings.defaultRepeatDelayMs
        }
        if let interval = file.repeat_interval_ms {
            settings.repeatIntervalMs = ButtonsSettings.repeatIntervalRange.contains(interval)
                ? interval : ButtonsSettings.defaultRepeatIntervalMs
        }
        for (rawKey, value) in file.keys ?? [:] {
            guard let button = RemoteButton(rawValue: rawKey) else { continue }
            if value == buttonsDisabledSentinel {
                settings.setMapping(ButtonMapping(action: .disabled, key: ""), for: button)
            } else if !value.isEmpty, KeySpec.parse(value) != nil {
                settings.setMapping(ButtonMapping(action: .key, key: value), for: button)
            }
        }
        return settings
    }

    public func deviceButtonsSettingsMap(
        _ devices: [String: DeviceConfigFile]?
    ) -> [String: ButtonsSettings] {
        guard let devices else { return [:] }
        return devices.reduce(into: [:]) { map, pair in
            let deviceID = normalizedDeviceID(pair.key)
            guard deviceID.count == 4, deviceID.allSatisfy({ $0.isASCII && $0.isHexDigit }),
                  let buttons = pair.value.buttons else {
                return
            }
            map[deviceID] = buttonsSettings(from: buttons)
        }
    }

    // ---- 设备交互/编码器设置（对齐 Windows Parse{Interaction,Encoder}Settings）----

    /// 返回设备有效交互设置：有覆盖返回覆盖（加载时已用全局默认填平），否则全局默认。

    public func keySpecValue(_ text: String?, fallback: String, allowEmpty: Bool = false) -> String {
        guard let text else { return fallback }
        if text.isEmpty { return allowEmpty ? text : fallback }
        return KeySpec.parse(text) != nil ? text : fallback
    }

    /// 顶层 interaction 键解析（对齐 Windows Load 顶层分支）。imu_wake_sensitivity 例外：
    /// 对齐 ImuWakeSensitivityFromName，非法值回 low 而不是保留 fallback。
    public func interactionSettingsValue(
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
    public func encoderSettingsValue(
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

    public func interactionSettingsValue(
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

    public func encoderSettingsValue(
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
    public func interactionSettingsValue(
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

    public func encoderSettingsValue(
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

    public func interactionSettings(
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

    public func encoderSettings(
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

    public func deviceInteractionSettingsMap(
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

    public func deviceEncoderSettingsMap(
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

    public func deviceOutputProfileMap(
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

    // ---- 语音编辑配置（[voice_edit] 表，方案 §9.1）----

    /// [voice_edit] 表解析：字段缺省保留 fallback；数值越界按区间**钳位**（与 Windows
    /// app_config.cc 的 std::clamp 同口径——回落默认会让同一份 config.toml 跨端行为
    /// 不同：如 max_chars=51 时 Windows=50、回落默认=20）；verify_mode 非法值回 auto。
    /// 各字段独立钳位、不做跨字段改写（同 Windows；default_chars > max_chars 时由
    /// 解析器按 max_chars 收紧 effective 删除量，两端解析器同口径）。
    public func voiceEditSettingsValue(
        _ file: VoiceEditConfigFile?, default fallback: VoiceEditSettings
    ) -> VoiceEditSettings {
        guard let file else { return fallback }
        var settings = fallback
        if let enabled = file.enabled { settings.enabled = enabled }
        if let value = file.default_chars {
            settings.defaultChars = clampVoiceEditValue(
                value, range: VoiceEditSettings.defaultCharsRange)
        }
        if let value = file.max_chars {
            settings.maxChars = clampVoiceEditValue(value, range: VoiceEditSettings.maxCharsRange)
        }
        if let value = file.scope_seconds {
            settings.scopeSeconds = clampVoiceEditValue(
                value, range: VoiceEditSettings.scopeSecondsRange)
        }
        if let value = file.verify_mode {
            settings.verifyMode = VoiceEditVerifyMode.parse(value)
        }
        if let value = file.unverified_seconds {
            settings.unverifiedSeconds = clampVoiceEditValue(
                value, range: VoiceEditSettings.unverifiedSecondsRange)
        }
        return settings
    }

    /// 整数区间钳位（对齐 Windows std::clamp(value, min, max)）。
    private func clampVoiceEditValue(_ value: Int, range: ClosedRange<Int>) -> Int {
        min(max(value, range.lowerBound), range.upperBound)
    }

    /// legacy 逐行解析版：逐行解析器丢弃 `[voice_edit]` 表头，表内键以裸键名出现；
    /// 历史配置无该表 → 全部回落默认。键名 `enabled` 等与顶层无冲突（顶层无同名字段）。
    public func voiceEditSettingsValue(
        _ values: [String: String], default fallback: VoiceEditSettings
    ) -> VoiceEditSettings {
        var file = VoiceEditConfigFile()
        file.enabled = values["enabled"].map { boolValue($0, default: fallback.enabled) }
        file.default_chars = values["default_chars"].flatMap(Int.init)
        file.max_chars = values["max_chars"].flatMap(Int.init)
        file.scope_seconds = values["scope_seconds"].flatMap(Int.init)
        file.verify_mode = values["verify_mode"]
        file.unverified_seconds = values["unverified_seconds"].flatMap(Int.init)
        return voiceEditSettingsValue(file, default: fallback)
    }

// ---- 追加纯助手（boolValue/outputTargetValue/textTransformValue/outputProfile） ----
    public func boolValue(_ text: String?, default defaultValue: Bool) -> Bool {
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

    public func outputTargetValue(_ text: String?, default defaultValue: OutputTarget) -> OutputTarget {
        guard let text, let target = OutputTarget(rawValue: text) else { return defaultValue }
        return target
    }

    public func textTransformValue(_ text: String?, default defaultValue: TextTransform) -> TextTransform {
        guard let text, let transform = TextTransform(rawValue: text) else { return defaultValue }
        return transform
    }

    public func outputProfile(
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
