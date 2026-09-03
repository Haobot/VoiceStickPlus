import Foundation

/// 小米蓝牙遥控器 2 Pro 的 Google ATVV 协议常量与编解码（纯逻辑，不碰 CoreBluetooth）。
/// 协议事实见 Doc/Plan/xiaomi-remote-2-pro-support.md §3：遥控器主导会话，
/// 主机应答；只接受 16 kHz 档。
public enum XiaomiAtvvProtocol {
    public static let serviceUUID = "AB5E0001-5A21-4F05-BC7D-AF01F617B664"
    public static let txUUID = "AB5E0002-5A21-4F05-BC7D-AF01F617B664"
    public static let audioUUID = "AB5E0003-5A21-4F05-BC7D-AF01F617B664"
    public static let controlUUID = "AB5E0004-5A21-4F05-BC7D-AF01F617B664"

    /// Control 特征 opcode（遥控器 → 主机 notify 的首字节）。
    public static let controlStop: UInt8 = 0x00 // 语音键松开
    public static let controlStreamStart: UInt8 = 0x04 // 音频流开始
    public static let controlMicOpen: UInt8 = 0x08 // 语音键按下（请求开麦）
    public static let controlAudioSync: UInt8 = 0x0A // ADPCM 解码器同步
    public static let controlCaps: UInt8 = 0x0B // GET_CAPS 应答

    /// codec 位掩码（CAPS 应答 bytes[3]，或旧版布局 bytes[4]）。
    public static let codecMask8kHz: UInt8 = 0x01
    public static let codecMask16kHz: UInt8 = 0x02

    /// CAPS 未协商帧长（或协商值 0）时的缺省 ADPCM 帧长：120 字节 = 240 采样。
    public static let defaultFrameBytes = 120

    public struct CapsInfo {
        public var version: UInt16 = 0 // BE16，0x0100 即 v1.0
        public var codecs: UInt8 = 0 // codec 位掩码
        public var interaction: UInt8 = 0
        public var frameBytes = defaultFrameBytes

        public init(version: UInt16 = 0, codecs: UInt8 = 0, interaction: UInt8 = 0,
                    frameBytes: Int = XiaomiAtvvProtocol.defaultFrameBytes) {
            self.version = version
            self.codecs = codecs
            self.interaction = interaction
            self.frameBytes = frameBytes
        }

        public var isV1OrLater: Bool { version >= 0x0100 }
        public var supports16kHz: Bool { codecs & codecMask16kHz != 0 }
    }

    /// 连接后主机写 TX 的能力查询：0A 01 00 00 03 03（GET_CAPS v1.0）。
    public static func getCapsCommand() -> Data {
        Data([0x0A, 0x01, 0x00, 0x00, 0x03, 0x03])
    }

    /// MIC_OPEN 应答：v≥1.0 写 0C 00；旧版补一字节所选 codec（0x02=16kHz）。
    public static func micOpenAckCommand(legacyLayout: Bool) -> Data {
        var data = Data([0x0C, 0x00])
        if legacyLayout { data.append(codecMask16kHz) }
        return data
    }

    /// 退出/断开时写 TX：v≥1.0 为 0D <sessionID>，旧版仅 0D。
    public static func micCloseCommand(legacyLayout: Bool, sessionID: UInt8) -> Data {
        var data = Data([0x0D])
        if !legacyLayout { data.append(sessionID) }
        return data
    }

    /// 解析 CAPS 应答（首字节须为 controlCaps）。布局分支：
    /// - v≥1.0：[3]=codec 掩码、[4]=interaction、[5:7]=帧长 BE16（0 → 默认 120）；
    ///   兼容「报 v1 但用旧版双字节 codec 布局」：codecs==0 且 len≥9 且 [4]&0x03≠0
    ///   时取 codecs=[4]、interaction=0x03；该分支旧版布局未定义帧长字段，
    ///   [5:7] 不读、frameBytes 保持默认 120；
    /// - v<1.0：需 len≥9，codecs=[4]、interaction=0，帧长取默认值。
    /// 短包/缺字段返回 nil；是否支持 16kHz 由调用方按 supports16kHz 判定。
    public static func parseCaps(_ data: Data) -> CapsInfo? {
        // Data 切片 startIndex 不归零（下方 data[0]/[1]... 是绝对下标，切片会越界
        // 崩溃）：入口拷贝归一化（实测 Data(data) 归零 startIndex）。
        let data = Data(data)
        guard data.count >= 3, data[0] == controlCaps else { return nil }

        var caps = CapsInfo()
        caps.version = UInt16(data[1]) << 8 | UInt16(data[2])
        if caps.isV1OrLater {
            // v1.0 布局：[3]=codec 掩码、[4]=interaction、[5:7]=帧长 BE16（可选）。
            guard data.count >= 5 else { return nil }
            caps.codecs = data[3]
            caps.interaction = data[4]
            // 兼容分支：报 v1 但 codecs==0，按旧版双字节 codec 布局重读（[4] 实为 codec）。
            // 旧版布局未定义帧长字段，[5:7] 是垃圾值，不读、保持默认 120
            //（与 scripts/e2e_test/atvv_capture.py 的解析对齐）。
            var legacyLayout = false
            if caps.codecs == 0, data.count >= 9, (data[4] & 0x03) != 0 {
                caps.codecs = data[4]
                caps.interaction = 0x03
                legacyLayout = true
            }
            if !legacyLayout, data.count >= 7 {
                let frameBytes = Int(data[5]) << 8 | Int(data[6])
                if frameBytes > 0 { caps.frameBytes = frameBytes }
            }
        } else {
            // 旧版布局：需 len≥9，codecs=[4]、interaction=0，帧长取默认值。
            guard data.count >= 9 else { return nil }
            caps.codecs = data[4]
            caps.interaction = 0
        }
        return caps
    }
}
