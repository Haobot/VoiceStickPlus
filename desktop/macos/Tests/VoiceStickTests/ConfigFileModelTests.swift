import Foundation
import VoiceStickCore

// N1 第三刀：TOML 配置模型闭包下沉 Core 的覆盖（用 JSONDecoder 驱动 Decodable 语义，
// 与 TOMLDecoder 同走 Codable 规约）——XiaomiConfigFile 的 int→double 宽容是
// Windows C++ 端写出 "gain_db = 18" 整数形态的跨端契约；deviceXiaomiSettingsMap
// 的归一键 + 缺表跳过 + 默认填平是 [device.<id>.xiaomi] 覆盖的核心语义。
func runConfigFileModelTests() {
    let decoder = JSONDecoder()

    // 1) gain_db 宽容：TOML 整数形态（Windows 写出）→ Double
    let intGain = try? decoder.decode(
        XiaomiConfigFile.self,
        from: Data(#"{"gain_db":18}"#.utf8))
    check(intGain?.gain_db == 18.0, "N1toml: gain_db 整数 18 → 18.0 宽容")

    // 2) 浮点原样
    let floatGain = try? decoder.decode(
        XiaomiConfigFile.self,
        from: Data(#"{"gain_db":6.5}"#.utf8))
    check(floatGain?.gain_db == 6.5, "N1toml: gain_db 浮点 6.5 原样")

    // 3) 字段缺省 → nil（由转换器决定是否回落默认）
    let empty = try? decoder.decode(
        XiaomiConfigFile.self, from: Data("{}".utf8))
    check(empty?.gain_db == nil && empty?.double_click_ms == nil,
          "N1toml: 空表字段 nil")

    // 4) double_click_ms 严格整数
    let dcm = try? decoder.decode(
        XiaomiConfigFile.self,
        from: Data(#"{"double_click_ms":400}"#.utf8))
    check(dcm?.double_click_ms == 400, "N1toml: double_click_ms=400")

    // 5) DeviceConfigFile 嵌套解码（[device.<id>.xiaomi] 形态）
    let root = try? decoder.decode(
        DeviceConfigFile.self,
        from: Data(#"{"xiaomi":{"gain_db":18,"double_click_ms":400}}"#.utf8))
    check(root?.xiaomi?.gain_db == 18.0, "N1toml: DeviceConfigFile.xiaomi 嵌套解码")

    // 6) deviceXiaomiSettingsMap：归一 + 缺表跳过 + 默认填平
    let devices: [String: DeviceConfigFile]? = [
        "rc-09af": DeviceConfigFile(xiaomi: XiaomiConfigFile(gain_db: 6.0, double_click_ms: 400)),
        "BAD": DeviceConfigFile(xiaomi: XiaomiConfigFile(gain_db: 1.0, double_click_ms: nil)),
        "1234": DeviceConfigFile(),   // 无 xiaomi 表 → 跳过
    ]
    let map = deviceXiaomiSettingsMap(devices)
    check(map.count == 1, "N1toml: 归一命中 1 台（BAD 归一失败丢弃、无表跳过）")
    check(map["09AF"]?.gainDb == 6.0 && map["09AF"]?.doubleClickMs == 400,
          "N1toml: 覆盖值入 map")
    check(map["09AF"] != nil && XiaomiSettings.default.doubleClickMs == 350,
          "N1toml: 默认结构 350 作兜底语义存在")

    // 7) xiaomiSettings(from:)：nil 字段回落 fallback（默认填平）
    let partial = xiaomiSettings(from: XiaomiConfigFile(gain_db: nil, double_click_ms: nil))
    check(partial.gainDb == 12.0 && partial.doubleClickMs == 350,
          "N1toml: 全缺省字段 → fallback 默认填平")
    let zeroRej = xiaomiSettings(
        from: XiaomiConfigFile(gain_db: nil, double_click_ms: 0))
    check(zeroRej.doubleClickMs == 350, "N1toml: double_click_ms=0 视为无效回落 350")

    // 8) toml 助手（save 侧字符串构建契约）
    check("a\\b\"c".tomlEscaped == "a\\\\b\\\"c",
          "N1toml: 反斜杠与引号转义")
    check(true.tomlValue == "true" && false.tomlValue == "false",
          "N1toml: Bool tomlValue")
}
