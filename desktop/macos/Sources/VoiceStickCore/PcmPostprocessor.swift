import Foundation

/// PCM 后处理：三点平滑（首尾样本不动）+ dB 增益（钳位 ±24 dB）+ int16 限幅。
/// 平滑与增益一次遍历完成：out[i] = clamp((in[i-1]+2*in[i]+in[i+1])>>2 * gain)。
public final class PcmPostprocessor {
    /// 增益钳位范围（dB）。
    public static let maxGainDb = 24.0

    public private(set) var gainDb: Double

    public init(gainDb: Double = 0.0) {
        self.gainDb = Self.clampGainDb(gainDb)
    }

    public func setGainDb(_ gainDb: Double) {
        self.gainDb = Self.clampGainDb(gainDb)
    }

    public static func clampGainDb(_ gainDb: Double) -> Double {
        min(max(gainDb, -maxGainDb), maxGainDb)
    }

    /// 返回处理后的新数组；少于 3 个样本时跳过平滑只做增益。
    public func process(_ pcm: [Int16]) -> [Int16] {
        let gain = pow(10.0, gainDb / 20.0)
        var out = [Int16](repeating: 0, count: pcm.count)
        for i in 0..<pcm.count {
            // 三点平滑（首尾样本不动）：out[i]=(in[i-1]+2*in[i]+in[i+1])>>2。
            var smoothed = Int32(pcm[i])
            if pcm.count >= 3, i > 0, i + 1 < pcm.count {
                smoothed = (Int32(pcm[i - 1]) + 2 * Int32(pcm[i]) + Int32(pcm[i + 1])) >> 2
            }
            // lround 语义：rounded() 默认 .toNearestOrAwayFromZero。
            let scaled = Int((Double(smoothed) * gain).rounded())
            out[i] = Int16(min(max(scaled, -32768), 32767))
        }
        return out
    }
}

/// ADPCM 裸字节流跨包累积切帧器：ATVV audio notify 无帧头无序号，
/// 按 CAPS 协商帧长（默认 120 字节）累积，攒满一帧吐出一帧。
public final class FrameAccumulator {
    public private(set) var frameBytes: Int
    // 用 [UInt8] 而非 Data：Data.removeFirst 后 startIndex 偏移不归零，
    // 绝对下标 0..<n 会越界；Array 恒为 0 基索引（对齐 Windows ByteVector）。
    private var buffer: [UInt8] = []

    public init(frameBytes: Int = XiaomiAtvvProtocol.defaultFrameBytes) {
        self.frameBytes = frameBytes
        buffer.reserveCapacity(frameBytes)
    }

    /// 调整协商帧长并清空已累积字节（CAPS 到达时调用）。
    public func setFrameBytes(_ frameBytes: Int) {
        self.frameBytes = frameBytes
        reset()
    }

    /// 已累积、尚未凑满一帧的字节数。
    public var pendingBytes: Int { buffer.count }

    public func reset() {
        buffer.removeAll(keepingCapacity: true)
    }

    /// 追加字节，返回本次凑满的所有完整帧（可能为零或多帧）。
    public func append(_ data: Data) -> [Data] {
        var frames: [Data] = []
        guard frameBytes > 0 else { return frames }
        buffer.append(contentsOf: data)
        while buffer.count >= frameBytes {
            frames.append(Data(buffer[0..<frameBytes]))
            buffer.removeFirst(frameBytes)
        }
        return frames
    }
}
