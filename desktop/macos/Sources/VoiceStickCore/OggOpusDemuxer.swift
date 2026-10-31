import Foundation

/// OggOpus 反序列化：把 OggOpus 字节流解析为 Opus packet 序列 + OpusHead 元信息。
/// 与 `OggOpusMuxer` 互为逆操作，供本地 ASR（SenseVoice 吃 PCM）把 BLE 链路的
/// Ogg 字节流还原为音频样本。
///
/// 与 Windows `ogg_opus_demuxer.cc` 逐分支对齐：标准 OggS 页结构 +
/// OpusHead/OpusTags 头页 + 音频页；lacing 按通用规则解析（segment_table 累加，
/// <255 结束一个 packet，255 续段，支持跨页 packet），兼容自家 muxer 产的
/// 「每页一 packet」简单格式与标准工具产物。
public struct OggOpusStream {
    public var sampleRate = 16000
    public var channels = 1
    /// OpusHead preskip（48kHz 域需丢弃的前置样本数）。与 Windows 同口径：
    /// 解码路径不消费该值（生产 muxer preskip=0），仅透出元信息。
    public var preskip = 0
    public var gain = 0
    /// 按顺序的 Opus packet。
    public var packets: [Data] = []

    public init() {}
}

public enum OggOpusDemuxer {

    /// 解析整个 OggOpus 字节流。
    /// 成功返回 true 并填充 out；遇格式错误（bad magic、截断、OpusHead 缺失）
    /// 返回 false。部分解析结果仍写入 out（已解析的 packets 保留）。
    public static func parse(_ data: Data) -> Bool {
        var out = OggOpusStream()
        let ok = parse(data, into: &out)
        return ok
    }

    public static func parse(_ data: Data, into out: inout OggOpusStream) -> Bool {
        out = OggOpusStream()
        let bytes = [UInt8](data)
        let size = bytes.count
        var offset = 0
        var headerParsed = false
        var error = false
        var pending = [UInt8]()   // 跨页累积的进行中 packet（lacing 255 续段）

        while offset < size {
            // Ogg 页头固定 27 字节：magic(4)+version(1)+header_type(1)+granule(8)+
            // serial(4)+sequence(4)+crc(4)+page_segments(1)。
            guard offset + 27 <= size,
                  bytes[offset] == 0x4F, bytes[offset + 1] == 0x67,
                  bytes[offset + 2] == 0x67, bytes[offset + 3] == 0x53 else {
                error = true
                break
            }
            let segTableOff = offset + 27
            let pageSegments = Int(bytes[offset + 26])
            guard segTableOff + pageSegments <= size else {
                error = true
                break
            }

            var dataOff = segTableOff + pageSegments
            for segI in 0..<pageSegments {
                let seg = bytes[segTableOff + segI]
                guard dataOff + Int(seg) <= size else {
                    error = true
                    break
                }
                pending.append(contentsOf: bytes[dataOff..<(dataOff + Int(seg))])
                dataOff += Int(seg)
                if seg < 255 {
                    if !headerParsed {
                        // 第一个完成的 packet 必须是 OpusHead。
                        guard parseOpusHead(pending, into: &out) else {
                            error = true
                            break
                        }
                        headerParsed = true
                    } else if !isOpusTags(pending) {
                        out.packets.append(Data(pending))
                    }
                    pending.removeAll(keepingCapacity: true)
                }
            }
            if error { break }
            offset = dataOff
        }

        return headerParsed && !error
    }

    /// OpusHead 布局：[0..7]="OpusHead" [8]=version [9]=channels [10..11]=preskip(LE16)
    /// [12..15]=input_sample_rate(LE32) [16..17]=gain(LE16) [18]=channel_mapping_family。
    private static func parseOpusHead(_ p: [UInt8], into out: inout OggOpusStream) -> Bool {
        guard p.count >= 19 else { return false }
        guard String(bytes: p[0..<8], encoding: .ascii) == "OpusHead" else { return false }
        out.channels = Int(p[9])
        out.preskip = Int(p[10]) | (Int(p[11]) << 8)
        out.sampleRate = Int(p[12]) | (Int(p[13]) << 8) | (Int(p[14]) << 16) | (Int(p[15]) << 24)
        out.gain = Int(p[16]) | (Int(p[17]) << 8)
        return true
    }

    private static func isOpusTags(_ p: [UInt8]) -> Bool {
        p.count >= 8 && String(bytes: p[0..<8], encoding: .ascii) == "OpusTags"
    }
}
