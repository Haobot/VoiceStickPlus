import Foundation

/// IMA/DVI ADPCM 解码器（1992 公开标准算法）：4 bit/采样，每字节高半字节优先，
/// 16 kHz 单声道。步长表/索引表为标准常数。遥控器会话内 predictor/step 连续推进，
/// 丢包即漂移，直到下次 reset（流开始硬重置或 AUDIO_SYNC 按值重置）。
public final class ImaAdpcmDecoder {
    // IMA/DVI ADPCM 公开标准（1992）常数表。
    private static let stepTable: [Int] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767,
    ]
    private static let indexTable: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8]

    public private(set) var predictor: Int16 = 0
    public private(set) var stepIndex: Int = 0

    public init() {}

    /// 重置解码状态。stepIndex 钳位到 [0, 88]。
    public func reset(predictor: Int16 = 0, stepIndex: Int = 0) {
        self.predictor = predictor
        self.stepIndex = min(max(stepIndex, 0), 88)
    }

    /// 解码一段 ADPCM 字节流，每字节拆高/低两个 nibble 各产出一个 int16 样本。
    public func decode(_ data: Data) -> [Int16] {
        var pcm: [Int16] = []
        pcm.reserveCapacity(data.count * 2)
        for byte in data {
            // 每字节高半字节优先。
            for shift in [4, 0] {
                let nibble = Int(byte >> UInt8(shift)) & 0x0F
                let step = Self.stepTable[stepIndex]
                var diff = step >> 3
                if nibble & 1 != 0 { diff += step >> 2 }
                if nibble & 2 != 0 { diff += step >> 1 }
                if nibble & 4 != 0 { diff += step }
                var value = Int(predictor)
                value = nibble & 8 != 0 ? value - diff : value + diff
                value = min(max(value, -32768), 32767)
                stepIndex = min(max(stepIndex + Self.indexTable[nibble & 7], 0), 88)
                predictor = Int16(value)
                pcm.append(predictor)
            }
        }
        return pcm
    }
}
