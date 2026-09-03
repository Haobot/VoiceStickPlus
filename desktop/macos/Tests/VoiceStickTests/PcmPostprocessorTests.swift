import Foundation
import VoiceStickCore

/// 移植 Windows TestFrameAccumulator + TestPcmPostprocessor。
/// （旧 XCTest PcmPostprocessorTests 逐条转换。）
func runPcmPostprocessorTests() {
    // ---- testFrameAccumulatorAcrossPackets ----
    // 跨包切帧：3 + 5 字节凑出两个 4 字节帧。
    do {
        let accumulator = FrameAccumulator(frameBytes: 4)
        check(accumulator.append(Data([1, 2, 3])).isEmpty, "FrameAccumulator.acrossPackets.firstEmpty")
        checkEqual(accumulator.pendingBytes, 3, "FrameAccumulator.acrossPackets.pending3")
        let frames = accumulator.append(Data([4, 5, 6, 7, 8]))
        checkEqual(frames.count, 2, "FrameAccumulator.acrossPackets.twoFrames")
        if frames.count == 2 {
            checkEqual(frames[0], Data([1, 2, 3, 4]), "FrameAccumulator.acrossPackets.frame0")
            checkEqual(frames[1], Data([5, 6, 7, 8]), "FrameAccumulator.acrossPackets.frame1")
        }
        checkEqual(accumulator.pendingBytes, 0, "FrameAccumulator.acrossPackets.pending0") // 8 字节恰好两帧

        // reset 清空部分累积。
        accumulator.reset()
        checkEqual(accumulator.pendingBytes, 0, "FrameAccumulator.acrossPackets.resetClears")
    }

    // ---- testFrameAccumulatorSetFrameBytesClears ----
    // 自定义协商帧长；setFrameBytes 同时清空已累积字节。
    do {
        let accumulator = FrameAccumulator(frameBytes: 4)
        _ = accumulator.append(Data([1, 2]))
        checkEqual(accumulator.pendingBytes, 2, "FrameAccumulator.setFrameBytes.pending2")
        accumulator.setFrameBytes(3)
        checkEqual(accumulator.frameBytes, 3, "FrameAccumulator.setFrameBytes.frameBytes3")
        checkEqual(accumulator.pendingBytes, 0, "FrameAccumulator.setFrameBytes.clearsPending")
        let custom = accumulator.append(Data([1, 2, 3, 4]))
        checkEqual(custom.count, 1, "FrameAccumulator.setFrameBytes.oneFrame")
        if custom.count == 1 {
            checkEqual(custom[0], Data([1, 2, 3]), "FrameAccumulator.setFrameBytes.frameContent")
        }
        checkEqual(accumulator.pendingBytes, 1, "FrameAccumulator.setFrameBytes.pending1")
    }

    // ---- testThreePointSmoothing ----
    // 三点平滑公式（首尾样本不动），增益 0dB。
    do {
        let flat = PcmPostprocessor(gainDb: 0.0)
        let smoothed = flat.process([0, 100, 200, 300, 400])
        // out[1]=(0+200+200)>>2=100, out[2]=(100+400+300)>>2=200, out[3]=(200+600+400)>>2=300
        checkEqual(smoothed, [0, 100, 200, 300, 400], "ThreePointSmoothing")
    }

    // ---- testGain ----
    // 增益 +6.02dB ≈ ×2；-6.02dB ≈ ×0.5（少于 3 样本跳过平滑只作增益）。
    do {
        let boost = PcmPostprocessor(gainDb: 6.020599913279624)
        checkEqual(boost.process([1000]), [2000], "Gain.boost6dB")
        let cut = PcmPostprocessor(gainDb: -6.020599913279624)
        checkEqual(cut.process([1000]), [500], "Gain.cut6dB")
    }

    // ---- testGainClamping ----
    // 增益钳位 ±24dB。
    do {
        let clamped = PcmPostprocessor(gainDb: 30.0)
        checkEqual(clamped.gainDb, 24.0, "GainClamping.high")
        clamped.setGainDb(-30.0)
        checkEqual(clamped.gainDb, -24.0, "GainClamping.low")
        clamped.setGainDb(24.0)
        // 1000 * 10^(24/20) ≈ 15848.9 → lround 15849。
        checkEqual(clamped.process([1000]), [15849], "GainClamping.lround")
    }

    // ---- testInt16Saturation ----
    // int16 限幅。
    do {
        let clamped = PcmPostprocessor(gainDb: 24.0)
        checkEqual(clamped.process([3000]), [32767], "Int16Saturation.positive")
        checkEqual(clamped.process([-3000]), [-32768], "Int16Saturation.negative")
    }
}
