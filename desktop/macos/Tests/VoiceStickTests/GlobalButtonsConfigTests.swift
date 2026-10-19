import Foundation
import VoiceStickCore

// 网关按键自定义恢复（macOS P1 补口）：根 [buttons] 全局键位表的 load/save 与
// 「无配对 RC 回落全局」链路——Windows ShowGatewayKeymapDialog 编辑 [xiaomi.keys]
// 的 macOS 对应物。此前 macOS 无全局表（buttonsSettings 恒 default 且无处写入），
// 网关模式既无 UI 入口也无可写存储：路由推导恒 passthrough、gateway_key 消费恒
// native 忽略，表现为「除录音键外其他按键都没作用」。
//
// 同时钉死一个先于本功能的静默丢表 bug：tomlNumber 整数值写出 3/80/4000（TOML
// 整数形态）→ TOMLKit decode(Double) 严格 typeMismatch → 整表解码失败静默回落
// loadLegacy（不认任何 [表] 段）→ [device.*] 按键映射等表在重启后全部丢失。
func runGlobalButtonsConfigTests() {
    // load：根 [buttons] → globalButtonsSettings（keys 三态 + intercept + 非法容忍）
    let toml = """
    [buttons]
    intercept = true

    [buttons.keys]
    back = "backspace"
    volume_up = "none"
    up = ""
    zzz_unknown = "ctrl+x"
    home = "shift"
    """
    let cfg = AppConfig.parse(text: toml)
    check(cfg.globalButtonsSettings.intercept, "gb: intercept 解析")
    check(cfg.globalButtonsSettings.mapping(for: .back)
          == ButtonMapping(action: .key, key: "backspace"),
          "gb: KeySpec 文本 → action=key")
    check(cfg.globalButtonsSettings.mapping(for: .volUp).action == .disabled,
          "gb: none 哨兵 → action=disabled")
    check(cfg.globalButtonsSettings.mapping(for: .up).action == .native,
          "gb: 空值回落 native")
    check(cfg.globalButtonsSettings.mapping(for: .tv).action == .native,
          "gb: 未知键名容忍（不进映射表）")
    check(cfg.globalButtonsSettings.mapping(for: .home).action == .native,
          "gb: 非法 KeySpec 条目忽略")

    // 消费回落：无 RC 配对条目 → buttonsSettings(for:) 取全局（协调器
    // activeXiaomiDeviceID=nil 的同款路径）。
    check(cfg.buttonsSettings(for: nil) == cfg.globalButtonsSettings,
          "gb: nil 设备回落全局")
    check(cfg.buttonsSettings(for: "FFFF") == cfg.globalButtonsSettings,
          "gb: 无覆盖设备回落全局")

    // 路由推导与消费同源（2026-10-12 语义修正）：可映射键一律 software——
    // 原生由桌面 naturalSpec 注入（macOS 系统级 HID 会话不可用）；未映射的 power
    // 与无映射面的 volume_mute 保持 passthrough（power 有映射时 → software，见下）。
    let routes = GatewaySupport.routes(for: cfg.buttonsSettings(for: nil))
    check(routes.first { $0.key == "back" }?.route == "software",
          "gb: 映射键推软件路由")
    check(routes.first { $0.key == "up" }?.route == "software",
          "gb: 未映射键（原生）推软件路由（桌面自然键注入）")
    check(routes.first { $0.key == "power" }?.route == "passthrough",
          "gb: 未映射的 power 推直通")

    // power 映射（2026-10-19 开放，macOS 补齐 Windows 12 键全集）：解析/路由/序列化。
    let powerCfg = AppConfig.parse(text: """
    [buttons.keys]
    power = "f1"
    """)
    check(powerCfg.globalButtonsSettings.mapping(for: .power)
          == ButtonMapping(action: .key, key: "f1"),
          "gb: power 键值 → action=key")
    check(GatewaySupport.routes(for: powerCfg.globalButtonsSettings)
          .first { $0.key == "power" }?.route == "software",
          "gb: 有映射的 power 推软件路由（对齐 Windows has_mapping 口径）")
    let powerDisabled = AppConfig.parse(text: """
    [buttons.keys]
    power = "none"
    """)
    check(powerDisabled.globalButtonsSettings.mapping(for: .power).action == .disabled,
          "gb: power none 哨兵 → action=disabled")
    check(GatewaySupport.routes(for: powerDisabled.globalButtonsSettings)
          .first { $0.key == "power" }?.route == "software",
          "gb: 禁用的 power 推软件路由（gateway_key 消费吞键）")
    let powerText = powerCfg.serializedText()
    check(powerText.contains("power = \"f1\""), "gb: 序列化写出 power 映射")
    check(AppConfig.parse(text: powerText).globalButtonsSettings
          == powerCfg.globalButtonsSettings, "gb: power save→load round-trip 恒等")

    // 整数形态 Double 宽容解码（LenientDouble）：%.4g 曾把 3/80/4000 写成 TOML
    // 整数，TOMLKit 严格 typeMismatch → 整表解码失败回落 legacy → 所有 [表] 段丢光。
    check(AppConfig.parse(text: "air_mouse_neutral_deadzone = 3\n\n[buttons]\nintercept = true")
          .globalButtonsSettings.intercept,
          "gb: 整数形态 Double 宽容解码（根 [buttons] 存活）")
    check(AppConfig.parse(
          text: "air_mouse_rate_gain = 80\n\n[device.FFFF.buttons]\nintercept = true")
          .deviceButtonsSettings["FFFF"]?.intercept == true,
          "gb: 整数形态下 [device.*] 表同样存活（历史丢表回归）")

    // 解码主路径探针：完整序列化文本必须走 TOMLDecoder 主路径（legacy 不读 [output]
    // 表只读根 output_target，回读 subtitle 即证明未回落）。
    var probe = cfg
    probe.defaultOutputProfile.target = .subtitle
    let probeText = probe.serializedText()
    check(AppConfig.parse(text: probeText).defaultOutputProfile.target == .subtitle,
          "gb: 序列化全文走解码主路径（非 legacy 回落）")

    // save round-trip：序列化 → 再解析恒等，且序列化幂等（字段无丢失）。
    let text = cfg.serializedText()
    check(text.contains("[buttons.keys]"), "gb: 序列化写出 keys 子表")
    check(text.contains("back = \"backspace\""), "gb: 序列化写出映射")
    check(text.contains("volume_up = \"none\""), "gb: 序列化写出禁用哨兵")
    check(text.contains("intercept = true"), "gb: 序列化写出 intercept")
    let reparsed = AppConfig.parse(text: text)
    check(reparsed.globalButtonsSettings == cfg.globalButtonsSettings,
          "gb: save→load round-trip 恒等")
    check(reparsed.serializedText() == text, "gb: 序列化幂等（解码无字段丢失）")

    // 缺段/默认：回落 .default 且默认不落盘（无脏段）。
    check(AppConfig.parse(text: "").globalButtonsSettings == .default,
          "gb: 缺段回落默认")
    check(!AppConfig.defaults.serializedText().contains("[buttons"),
          "gb: 默认配置不写出 [buttons] 段")

    // 长按连发节拍（[buttons] repeat_*）：解析、越界钳位、非默认 round-trip。
    let repeatToml = """
    [buttons]
    repeat_delay_ms = 450
    repeat_interval_ms = 120

    [buttons.keys]
    back = "delete"
    """
    let repeatCfg = AppConfig.parse(text: repeatToml)
    check(repeatCfg.globalButtonsSettings.repeatDelayMs == 450,
          "gb: repeat_delay_ms 解析")
    check(repeatCfg.globalButtonsSettings.repeatIntervalMs == 120,
          "gb: repeat_interval_ms 解析")
    check(repeatCfg.globalButtonsSettings.mapping(for: .back).key == "delete",
          "gb: repeat_* 与 keys 共存")
    let repeatClamped = AppConfig.parse(text: """
    [buttons]
    repeat_delay_ms = 5
    repeat_interval_ms = 99999
    """)
    check(repeatClamped.globalButtonsSettings.repeatDelayMs == 300
          && repeatClamped.globalButtonsSettings.repeatIntervalMs == 80,
          "gb: repeat_* 越界钳位回落默认")
    let repeatText = repeatCfg.serializedText()
    check(repeatText.contains("repeat_delay_ms = 450")
          && repeatText.contains("repeat_interval_ms = 120"),
          "gb: 非默认节拍落盘")
    let repeatReparsed = AppConfig.parse(text: repeatText)
    check(repeatReparsed.globalButtonsSettings == repeatCfg.globalButtonsSettings,
          "gb: repeat_* save→load round-trip 恒等")
    check(repeatReparsed.serializedText() == repeatText, "gb: repeat_* 序列化幂等")

    // 设备级覆盖表同款节拍字段。
    let deviceRepeat = AppConfig.parse(text: """
    [device.FFFF.buttons]
    repeat_delay_ms = 600
    """)
    check(deviceRepeat.deviceButtonsSettings["FFFF"]?.repeatDelayMs == 600,
          "gb: [device.*.buttons] repeat_delay_ms 解析")
}
