import Foundation
import VoiceStickCore

// N1 第一刀：配置纯逻辑下沉 VoiceStickCore/ConfigParsing.swift 的覆盖——
// runner 旧 NOTE 点名三缺口（normalizedDeviceID / paired CSV / xiaomi TOML）
// 的前两件 + XiaomiSettings 结构基线；xiaomi TOML 映射随 DeviceConfigFile
// 闭包留下一刀。
func runConfigParsingTests() {
    // normalizedDeviceID（对齐 Windows NormalizeDeviceId：VS-/RC- 都剥、IsHex4）
    check(normalizedDeviceID("VS-09AF") == "09AF", "N1: VS- 前缀剥离")
    check(normalizedDeviceID("rc-09af") == "09AF", "N1: RC- 小写前缀归一")
    check(normalizedDeviceID("09af") == "09AF", "N1: 裸 4 hex 归一")
    check(normalizedDeviceID("  AB12  ") == "AB12", "N1: 空白裁剪")
    check(normalizedDeviceID("AB123") == "AB12", "N1: 超长截 4 位")
    check(normalizedDeviceID("AB1G") == "", "N1: 非 hex → 空")
    check(normalizedDeviceID("AB") == "", "N1: 不足 4 位 → 空")
    check(normalizedDeviceID("VS-XYZ") == "", "N1: 前缀后非 hex → 空")

    // deviceIDList：归一 + 去重 + 非法丢弃
    check(deviceIDList("VS-09AF,09af,zzzz,rc-1234") == ["09AF", "1234"],
          "N1: ID 列表去重与非法丢弃")
    check(deviceIDList("") == [], "N1: 空 ID 列表")

    // 配对 CSV：六段解析 / 短行补空 / 格式化往返 / 空 ID 行丢弃
    let entry = parsePairedDeviceEntry(
        "09AF,UUID-ABC,uuid,Mi Remote,xiaomi_remote_2_pro,1.2.3")
    check(entry.deviceID == "09AF", "N1: CSV 六段解析 deviceID")
    check(entry.hardware == PairedDeviceEntry.hardwareXiaomiRemote2Pro,
          "N1: hardware 常量对齐 kHardwareXiaomiRemote2Pro")
    let short = parsePairedDeviceEntry("09AF,UUID-ABC,uuid")
    check(short.hardware == "" && short.firmwareVersion == "",
          "N1: 短行缺段补空")
    let round = parsePairedDeviceEntry(formatPairedDeviceEntry(entry))
    check(round == entry, "N1: format→parse 往返恒等")
    let list = pairedDeviceEntryList(
        ["09AF,U1,uuid,N1", ",,,", "1234,U2,uuid,N2"])
    check(list.count == 2 && list[0].deviceID == "09AF" && list[1].deviceID == "1234",
          "N1: 空 deviceID 行丢弃")

    // XiaomiSettings 结构基线（xiaomi TOML 下一刀的承重结构）
    let defaults = XiaomiSettings.default
    check(defaults.gainDb == 12.0 && defaults.doubleClickMs == 350,
          "N1: XiaomiSettings 默认 gainDb=12.0 / doubleClickMs=350")
    let custom = XiaomiSettings(gainDb: 6.0, doubleClickMs: 400)
    check(custom.gainDb == 6.0 && custom.doubleClickMs == 400,
          "N1: XiaomiSettings 显式初值")
}
