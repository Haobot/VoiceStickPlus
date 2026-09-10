import Foundation
import VoiceStickCore

/// RemoteButtonHID（小米遥控器按键自定义二期 HID usage 映射与拦截处置）测试：
/// usage→按钮映射表、未映射键（power/mute/未知）返回 nil、intercept 开关与
/// action→decision 处置判定。
func runRemoteButtonHIDTests() {
    // ---- usage → RemoteButton 映射表（对齐方案 §3.1 键码表）----
    let cases: [(UInt32, RemoteButton)] = [
        (0x28, .ok), (0x4A, .home), (0xF1, .back), (0x65, .menu), (0x35, .tv),
        (0x4F, .right), (0x50, .left), (0x51, .down), (0x52, .up),
        (0x80, .volUp), (0x81, .volDown),
    ]
    for (usage, button) in cases {
        checkEqual(RemoteButtonHIDMap.button(forUsage: usage), button,
                   "HidMap.usage_\(String(usage, radix: 16))")
    }

    // ---- 未映射键：power(0x66)、mute(0x7F)、未知(0x99/0x00) 均返回 nil → 处置 native ----
    checkNil(RemoteButtonHIDMap.button(forUsage: 0x66), "HidMap.powerNil")
    checkNil(RemoteButtonHIDMap.button(forUsage: 0x7F), "HidMap.muteNil")
    checkNil(RemoteButtonHIDMap.button(forUsage: 0x99), "HidMap.unknownNil")
    checkNil(RemoteButtonHIDMap.button(forUsage: 0x00), "HidMap.zeroNil")

    // ---- interceptableButtons：11 个 B 级键，排除 voiceDoubleClick ----
    checkEqual(RemoteButtonHIDMap.interceptableButtons.count, 11, "HidMap.interceptableCount11")
    check(!RemoteButtonHIDMap.interceptableButtons.contains(.voiceDoubleClick),
          "HidMap.excludesVoiceDoubleClick")
    for b in RemoteButtonHIDMap.interceptableButtons {
        check(b.requiresIntercept, "HidMap.\(b.rawValue).requiresIntercept")
        check(b != .voiceDoubleClick, "HidMap.\(b.rawValue).isBLevel")
    }

    // ---- decision：intercept=false → 一律 native（无论 action）----
    checkEqual(RemoteButtonHIDMap.decision(intercept: false, mapping: ButtonMapping(action: .native, key: "")),
               .native, "HidDecision.off.native")
    checkEqual(RemoteButtonHIDMap.decision(intercept: false, mapping: ButtonMapping(action: .disabled, key: "")),
               .native, "HidDecision.off.disabled")
    checkEqual(RemoteButtonHIDMap.decision(intercept: false, mapping: ButtonMapping(action: .key, key: "alt+left")),
               .native, "HidDecision.off.key")

    // ---- decision：intercept=true，action 分支 ----
    checkEqual(RemoteButtonHIDMap.decision(intercept: true, mapping: ButtonMapping(action: .native, key: "")),
               .native, "HidDecision.on.native")
    checkEqual(RemoteButtonHIDMap.decision(intercept: true, mapping: ButtonMapping(action: .disabled, key: "")),
               .suppress, "HidDecision.on.disabled")
    checkEqual(RemoteButtonHIDMap.decision(intercept: true, mapping: ButtonMapping(action: .key, key: "alt+left")),
               .inject(key: "alt+left"), "HidDecision.on.key")
    checkEqual(RemoteButtonHIDMap.decision(intercept: true, mapping: ButtonMapping(action: .key, key: "")),
               .inject(key: ""), "HidDecision.on.emptyKey")

    // ---- 端到端：usage → 按钮 → decision 链条（自定义返回键 → inject alt+left）----
    guard let back = unwrap(RemoteButtonHIDMap.button(forUsage: 0xF1), "HidPipeline.backMapping") else { return }
    var settings = ButtonsSettings.default
    settings.intercept = true
    settings.setMapping(ButtonMapping(action: .key, key: "alt+left"), for: .back)
    checkEqual(RemoteButtonHIDMap.decision(intercept: settings.intercept,
                                           mapping: settings.mapping(for: back)),
               .inject(key: "alt+left"), "HidPipeline.backInject")

    // ---- 端到端：intercept 关闭 → 即使配置了 key 也 native ----
    settings.intercept = false
    checkEqual(RemoteButtonHIDMap.decision(intercept: settings.intercept,
                                           mapping: settings.mapping(for: back)),
               .native, "HidPipeline.backNativeWhenOff")

    // ---- usage → macOS keycode 反映射（tap 侧吞除用）----
    let keyCodeCases: [(UInt32, Int64)] = [
        (0x28, 0x24), (0x4A, 0x73),
        (0x4F, 0x7C), (0x50, 0x7B), (0x51, 0x7D), (0x52, 0x7E),
        (0x35, 0x32),
    ]
    for (usage, keyCode) in keyCodeCases {
        checkEqual(RemoteButtonHIDMap.macKeyCode(forUsage: usage), keyCode,
                   "HidKeyCode.usage_\(String(usage, radix: 16))")
    }
    // macOS 不产生 keyDown 的 usage 不入表：back(0xF1)/menu(0x65)/音量(0x80/0x81)/未知
    checkNil(RemoteButtonHIDMap.macKeyCode(forUsage: 0xF1), "HidKeyCode.backNil")
    checkNil(RemoteButtonHIDMap.macKeyCode(forUsage: 0x65), "HidKeyCode.menuNil")
    checkNil(RemoteButtonHIDMap.macKeyCode(forUsage: 0x80), "HidKeyCode.volUpNil")
    checkNil(RemoteButtonHIDMap.macKeyCode(forUsage: 0x81), "HidKeyCode.volDownNil")

    // ---- 音量键 systemDefined 键码 ↔ usage ----
    checkEqual(RemoteButtonHIDMap.volumeSystemKeyToUsage[0], 0x80, "HidVolSys.up")
    checkEqual(RemoteButtonHIDMap.volumeSystemKeyToUsage[1], 0x81, "HidVolSys.down")
    checkNil(RemoteButtonHIDMap.volumeSystemKeyToUsage[7], "HidVolSys.otherNil")
}
