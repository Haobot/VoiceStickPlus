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
}
