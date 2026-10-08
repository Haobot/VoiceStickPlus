import Foundation

// N1 第一刀：配置纯逻辑下沉（AppConfig 的身份归一 + 配对 CSV + 小米设置结构）。
// 自 VoiceStickApp/AppConfig.swift 抽出——runner NOTE 曾点名「normalizedDeviceID /
// paired CSV / xiaomi TOML」三件不在覆盖范围；本文件覆盖前两件 + XiaomiSettings
// 结构（xiaomi TOML 映射依赖 DeviceConfigFile 模型闭包，留下一刀）。纯 Foundation，
// 无需改 Package 依赖。

/// 配对设备条目（对齐 Windows PairedDeviceEntry，CSV 持久化格式
/// id,addr,address_kind,name[,hardware,firmware_version]。
/// macOS 读不到蓝牙 MAC：addr 存 CBPeripheral.identifier.uuidString（大写），
/// addressKind 恒为 "uuid"；Windows 的 12 位 hex MAC + "0"/"1"/"2" 原样透传不解释。
public struct PairedDeviceEntry: Equatable {
    public var deviceID: String
    public var address: String
    public var addressKind: String
    public var name: String
    public var hardware: String
    public var firmwareVersion: String

    public init(deviceID: String, address: String, addressKind: String, name: String,
                hardware: String, firmwareVersion: String) {
        self.deviceID = deviceID
        self.address = address
        self.addressKind = addressKind
        self.name = name
        self.hardware = hardware
        self.firmwareVersion = firmwareVersion
    }

    /// hardware 段标识（对齐 Windows kHardwareXiaomiRemote2Pro）。
    public static let hardwareXiaomiRemote2Pro = "xiaomi_remote_2_pro"
}

/// 小米蓝牙遥控器 2 Pro 设置（对齐 Windows XiaomiSettings）：全局默认即结构默认值，
/// [device.<id>.xiaomi] 按设备覆盖（加载时已用默认填平所有字段）。
public struct XiaomiSettings: Equatable {
    /// ADPCM 解码后增益（dB），消费侧 ±24 限幅。默认 12.0。
    public var gainDb = 12.0
    /// 语音键双击时序窗（ms）：第一次短击释放后等待第二次按下的最大窗口。默认 350。
    public var doubleClickMs = 350

    public init(gainDb: Double = 12.0, doubleClickMs: Int = 350) {
        self.gainDb = gainDb
        self.doubleClickMs = doubleClickMs
    }

    public static let `default` = XiaomiSettings()
}

/// 归一化设备 ID（对齐 Windows NormalizeDeviceId）：VS-/RC- 前缀都剥、截 4 位后
/// 必须恰好 4 位 ASCII hex，非法一律返回 ""（校验不再下放调用方）。
public func normalizedDeviceID(_ text: String) -> String {
    let upper = text.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
    let stripped = (upper.hasPrefix("VS-") || upper.hasPrefix("RC-"))
        ? upper.dropFirst(3).prefix(4)
        : upper.prefix(4)
    guard stripped.count == 4, stripped.allSatisfy({ $0.isASCII && $0.isHexDigit }) else {
        return ""
    }
    return String(stripped)
}

/// 逗号列表 → 归一 ID 列表（去重、非法丢弃）。
public func deviceIDList(_ text: String) -> [String] {
    text.split(separator: ",")
        .map { normalizedDeviceID(String($0)) }
        .filter { $0.count == 4 && $0.allSatisfy({ $0.isASCII && $0.isHexDigit }) }
        .reduce(into: []) { ids, id in
            if !ids.contains(id) {
                ids.append(id)
            }
        }
}

/// 解析配对条目 CSV 行（不足 6 段补空串）。
public func parsePairedDeviceEntry(_ line: String) -> PairedDeviceEntry {
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
public func formatPairedDeviceEntry(_ entry: PairedDeviceEntry) -> String {
    [entry.deviceID, entry.address, entry.addressKind, entry.name,
     entry.hardware, entry.firmwareVersion].joined(separator: ",")
}

/// 配对条目行列表（deviceID 为空的行丢弃）。
public func pairedDeviceEntryList(_ lines: [String]) -> [PairedDeviceEntry] {
    lines.map { parsePairedDeviceEntry($0) }.filter { !$0.deviceID.isEmpty }
}
