import Foundation
import VoiceStickCore

// N1 第四刀：配置域类型+KeySpec+纯转换器下迁 Core 的覆盖——语义全部先读后写
//（encoder 门限/枚举回落/keySpecValue 三态/parse 五态/clamp 边界）。
func runConfigDomainTests() {
    // KeySpec.parse：五态
    check(KeySpec.parse("down") != nil, "N1dom: 具名键 down 解析成功")
    check(KeySpec.parse("ctrl+down") != nil, "N1dom: 修饰+主键解析成功")
    check(KeySpec.parse("") == nil, "N1dom: 空串 → nil")
    check(KeySpec.parse("shift") == nil, "N1dom: 仅修饰键 → nil")
    check(KeySpec.parse("down+up") == nil, "N1dom: 多主键 → nil")

    // keySpecValue：三态（nil→fallback / 空→按 allowEmpty / 非法→fallback）
    check(keySpecValue(nil, fallback: "up") == "up", "N1dom: keySpec nil → fallback")
    check(keySpecValue("", fallback: "up", allowEmpty: false) == "up",
          "N1dom: 空且禁空 → fallback")
    check(keySpecValue("", fallback: "up", allowEmpty: true) == "",
          "N1dom: 空且允空 → 空串")
    check(keySpecValue("bogus_key_xyz", fallback: "up") == "up",
          "N1dom: 非法键 → fallback")

    // interactionSettingsValue：越界回落 + imu 非法回 low
    check(interactionSettingsValue(
        imuWakeSensitivity: nil, tapToArrow: nil, tapSensitivity: 99,
        airMouseSensitivityX: nil, airMouseSensitivityY: nil,
        default: InteractionSettings.default).tapSensitivity == 5,
        "N1dom: tapSensitivity 99 → 回落 5")
    check(interactionSettingsValue(
        imuWakeSensitivity: "high", tapToArrow: nil, tapSensitivity: 7,
        airMouseSensitivityX: nil, airMouseSensitivityY: nil,
        default: InteractionSettings.default).imuWakeSensitivity == .high,
        "N1dom: imu high 解析")
    check(interactionSettingsValue(
        imuWakeSensitivity: "bogus", tapToArrow: nil, tapSensitivity: nil,
        airMouseSensitivityX: nil, airMouseSensitivityY: nil,
        default: InteractionSettings.default).imuWakeSensitivity == .low,
        "N1dom: imu 非法回 low（不保留 fallback）")

    // clamp 边界
    check(InteractionSettings.clampedSensitivity(0) == 5
          && InteractionSettings.clampedSensitivity(10) == 10,
          "N1dom: clampedSensitivity 边界 0→5 / 10→10")

    // encoderSettingsValue：门限与枚举
    let enc = encoderSettingsValue(
        toArrow: nil, rotationInvert: nil, rotateCwKey: nil, rotateCcwKey: nil,
        rotateFastThreshold: 0, rotateCwFastKey: nil, rotateCcwFastKey: nil,
        rotateDecideWindowMs: -1, ledColor: "magenta", pressAction: nil,
        pressKey: nil, doubleClickAction: nil, doubleClickKey: nil,
        default: EncoderSettings.default)
    check(enc.rotateFastThreshold == 200, "N1dom: 阈值 0 无效保留默认 200")
    check(enc.rotateDecideWindowMs == 80, "N1dom: 窗口 -1 无效保留默认 80")
    check(enc.ledColor == .red, "N1dom: 非法色名保留默认 red")
    let enc2 = encoderSettingsValue(
        toArrow: nil, rotationInvert: nil, rotateCwKey: "up", rotateCcwKey: nil,
        rotateFastThreshold: 333, rotateCwFastKey: nil, rotateCcwFastKey: nil,
        rotateDecideWindowMs: 0, ledColor: "cyan", pressAction: "key",
        pressKey: "", doubleClickAction: nil, doubleClickKey: nil,
        default: EncoderSettings.default)
    check(enc2.rotateFastThreshold == 333 && enc2.rotateDecideWindowMs == 0,
          "N1dom: 门限 333/窗口 0 有效")
    check(enc2.ledColor == .cyan && enc2.pressAction == .key && enc2.pressKey == "",
          "N1dom: cyan+key+空 pressKey 允许")

    // deviceXiaomiSettingsMap 之外：InteractionSettings 默认语义
    check(InteractionSettings.default.tapSensitivity == 5
          && InteractionSettings.default.imuWakeSensitivity == .low,
          "N1dom: InteractionSettings 默认")
    check(EncoderSettings.default.rotateFastThreshold == 200
          && EncoderSettings.default.ledColor == .red,
          "N1dom: EncoderSettings 默认")
    check(OutputProfile.default.target == .focusedApp,
          "N1dom: OutputProfile 默认 focusedApp")

    // ---- [voice_edit] 语音编辑配置（方案 §9.1）----
    check(VoiceEditSettings.default.enabled
          && VoiceEditSettings.default.defaultChars == 1
          && VoiceEditSettings.default.maxChars == 20
          && VoiceEditSettings.default.scopeSeconds == 120
          && VoiceEditSettings.default.verifyMode == .auto
          && VoiceEditSettings.default.unverifiedSeconds == 8,
          "N1dom: VoiceEditSettings 默认值（开启/1 字/20/120/auto/8s）")

    let voiceEditToml = """
    [voice_edit]
    enabled = false
    default_chars = 3
    max_chars = 7
    scope_seconds = 60
    verify_mode = "strict"
    unverified_seconds = 2
    """
    let voiceEditCfg = AppConfig.parse(text: voiceEditToml)
    check(!voiceEditCfg.voiceEdit.enabled, "N1dom: voice_edit enabled=false 解析")
    check(voiceEditCfg.voiceEdit.defaultChars == 3, "N1dom: voice_edit default_chars 解析")
    check(voiceEditCfg.voiceEdit.maxChars == 7, "N1dom: voice_edit max_chars 解析")
    check(voiceEditCfg.voiceEdit.scopeSeconds == 60, "N1dom: voice_edit scope_seconds 解析")
    check(voiceEditCfg.voiceEdit.verifyMode == .strict, "N1dom: voice_edit verify_mode=strict")
    check(voiceEditCfg.voiceEdit.unverifiedSeconds == 2, "N1dom: voice_edit unverified_seconds")

    check(AppConfig.parse(text: "asr_provider = \"volcengine\"").voiceEdit
          == VoiceEditSettings.default,
          "N1dom: voice_edit 缺表回落默认")

    let voiceEditClamped = AppConfig.parse(text: """
    [voice_edit]
    default_chars = 99
    max_chars = 0
    scope_seconds = 5
    verify_mode = "bogus"
    unverified_seconds = 99
    """)
    check(voiceEditClamped.voiceEdit.defaultChars == 10, "N1dom: default_chars 99 钳位 10")
    check(voiceEditClamped.voiceEdit.maxChars == 1, "N1dom: max_chars 0 钳位 1")
    check(voiceEditClamped.voiceEdit.scopeSeconds == 10, "N1dom: scope_seconds 5 钳位 10")
    check(voiceEditClamped.voiceEdit.unverifiedSeconds == 30,
          "N1dom: unverified_seconds 99 钳位 30")
    check(voiceEditClamped.voiceEdit.verifyMode == .auto, "N1dom: verify_mode 非法回落 auto")

    // 跨端一致性：Windows kVoiceEditMaxCharsMax=50（app_config.cc std::clamp），
    // 同一份 config.toml 的 max_chars=51 在两端都必须得到 50。
    let voiceEditMaxBound = AppConfig.parse(text: """
    [voice_edit]
    max_chars = 51
    """)
    check(voiceEditMaxBound.voiceEdit.maxChars == 50,
          "N1dom: max_chars 51 钳位 50（对齐 Windows 上限）")

    // 各字段独立钳位（同 Windows，不跨字段改写）；default_chars > max_chars 时由
    // 解析器按 max_chars 收紧 effective 删除量（两端解析器同口径）。
    let voiceEditTight = AppConfig.parse(text: """
    [voice_edit]
    default_chars = 8
    max_chars = 3
    """)
    check(voiceEditTight.voiceEdit.defaultChars == 8 && voiceEditTight.voiceEdit.maxChars == 3,
          "N1dom: voice_edit 各字段独立钳位（不跨字段改写）")
    checkEqual(
        VoiceEditParser.parse(
            "删除", defaultChars: voiceEditTight.voiceEdit.defaultChars,
            maxChars: voiceEditTight.voiceEdit.maxChars),
        VoiceEditCommand(action: .deleteLastChars(3), source: .defaultCount),
        "N1dom: default_chars>max_chars 时解析器收紧默认删除量（同 Windows）")

    // 序列化往返（[voice_edit] 恒写出 + 幂等）。
    var voiceEditRoundTrip = AppConfig.defaults
    voiceEditRoundTrip.voiceEdit.enabled = false
    voiceEditRoundTrip.voiceEdit.defaultChars = 4
    voiceEditRoundTrip.voiceEdit.maxChars = 30
    voiceEditRoundTrip.voiceEdit.scopeSeconds = 300
    voiceEditRoundTrip.voiceEdit.verifyMode = .strict
    voiceEditRoundTrip.voiceEdit.unverifiedSeconds = 3
    let voiceEditText = voiceEditRoundTrip.serializedText()
    check(voiceEditText.contains("[voice_edit]"), "N1dom: serializedText 含 [voice_edit]")
    let voiceEditReparsed = AppConfig.parse(text: voiceEditText)
    check(voiceEditReparsed.voiceEdit == voiceEditRoundTrip.voiceEdit,
          "N1dom: voice_edit 序列化往返一致")
    check(voiceEditReparsed.serializedText() == voiceEditText, "N1dom: voice_edit 序列化幂等")

    // legacy 逐行解析（表头被丢弃，裸键名读取；历史配置无该表 → 默认）。
    let voiceEditLegacy = voiceEditSettingsValue(
        ["enabled": "false", "default_chars": "2", "verify_mode": "off"],
        default: .default
    )
    check(!voiceEditLegacy.enabled && voiceEditLegacy.defaultChars == 2
          && voiceEditLegacy.verifyMode == .off,
          "N1dom: voice_edit legacy 裸键解析")
    check(voiceEditSettingsValue([:], default: .default) == .default,
          "N1dom: voice_edit legacy 缺键回落默认")

    // config.example.toml 可被解析且 [voice_edit] 段生效（防示例与解析器漂移）。
    let exampleURL = TestSupport.repoRoot
        .appendingPathComponent("desktop/macos/Config/config.example.toml")
    if let exampleText = try? String(contentsOf: exampleURL, encoding: .utf8) {
        let example = AppConfig.parse(text: exampleText)
        check(example.voiceEdit == VoiceEditSettings.default,
              "N1dom: config.example.toml [voice_edit] 解析为默认值")
    } else {
        check(false, "N1dom: config.example.toml 不可读")
    }
}
