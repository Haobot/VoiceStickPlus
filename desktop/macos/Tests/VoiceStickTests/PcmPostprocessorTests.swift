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

    // ---- 旧路径用例（关 HPF/AGC，守 smooth3+增益 语义）----

    // ---- testThreePointSmoothing ----
    // 三点平滑公式（首尾样本不动），增益 0dB。
    do {
        let flat = PcmPostprocessor(gainDb: 0.0, hpf: false, agc: false)
        let smoothed = flat.process([0, 100, 200, 300, 400])
        // out[1]=(0+200+200)>>2=100, out[2]=(100+400+300)>>2=200, out[3]=(200+600+400)>>2=300
        checkEqual(smoothed, [0, 100, 200, 300, 400], "ThreePointSmoothing")
    }

    // ---- testGain ----
    // 增益 +6.02dB ≈ ×2；-6.02dB ≈ ×0.5（少于 3 样本跳过平滑只作增益）。
    do {
        let boost = PcmPostprocessor(gainDb: 6.020599913279624, hpf: false, agc: false)
        checkEqual(boost.process([1000]), [2000], "Gain.boost6dB")
        let cut = PcmPostprocessor(gainDb: -6.020599913279624, hpf: false, agc: false)
        checkEqual(cut.process([1000]), [500], "Gain.cut6dB")
    }

    // ---- testGainClamping ----
    // 增益钳位 ±24dB。
    do {
        let clamped = PcmPostprocessor(gainDb: 30.0, hpf: false, agc: false)
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
        let clamped = PcmPostprocessor(gainDb: 24.0, hpf: false, agc: false)
        checkEqual(clamped.process([3000]), [32767], "Int16Saturation.positive")
        checkEqual(clamped.process([-3000]), [-32768], "Int16Saturation.negative")
    }

    // ---- 新链用例（HPF/AGC，算法规格见 Doc/Plan/xiaomi-direct-desktop-hpf-agc.md）----

    func makeSine(_ amp: Double, _ freq: Double, _ n: Int) -> [Int16] {
        (0..<n).map { i in
            Int16(clamping: Int((amp * sin(2.0 * .pi * freq * Double(i) / 16000.0)).rounded()))
        }
    }
    func steadyRms(_ samples: [Int16]) -> Double {
        let tail = samples.suffix(8000)
        let sum = tail.reduce(0.0) { $0 + Double($1) * Double($1) }
        return sqrt(sum / Double(tail.count))
    }

    // ---- testHpfDcDecay ----
    // 恒定 5000 输入 1000 样本：HPF 抑制 DC，末 100 样本 |out| < 50。
    do {
        let pp = PcmPostprocessor(gainDb: 0.0, hpf: true, agc: false)
        let out = pp.process([Int16](repeating: 5000, count: 1000))
        let tailPeak = out.suffix(100).reduce(0) { max($0, abs(Int($1))) }
        check(tailPeak < 50, "HpfDcDecay tailPeak=\(tailPeak)")
    }

    // ---- testHpfFrequencyResponse ----
    // 50Hz 对 1kHz 稳态 RMS 比 ≥ 13dB 衰减（120Hz 档位参考频响 50Hz -15.3dB，
    // 留 2.3dB 余量；旧 90Hz 档位 -10.6dB≈0.295 会触红，可防系数回退）。
    do {
        let pp50 = PcmPostprocessor(gainDb: 0.0, hpf: true, agc: false)
        let out50 = pp50.process(makeSine(10000, 50, 16000))
        let pp1k = PcmPostprocessor(gainDb: 0.0, hpf: true, agc: false)
        let out1k = pp1k.process(makeSine(10000, 1000, 16000))
        let ratio = steadyRms(out50) / steadyRms(out1k)
        check(ratio <= pow(10.0, -13.0 / 20.0), "HpfFrequencyResponse ratio=\(ratio)")
    }

    // ---- testAgcPullup ----
    // -40dBFS（幅度 ≈328）1kHz 正弦 2s：AGC 最大增益 +12dB，稳态输出 ≈1312±20%
    //（-40dBFS 到目标 -12dBFS 需 +28dB，超出 MAX_GAIN +12dB，故拉到 -28dBFS 量级；
    // Python 镜像仿真稳态峰值 1291）。
    do {
        let pp = PcmPostprocessor(gainDb: 0.0) // hpf/agc 默认开
        let out = pp.process(makeSine(328, 1000, 32000))
        let peak = out.suffix(4000).reduce(0) { max($0, abs(Int($1))) }
        check(peak >= 1049 && peak <= 1574, "AgcPullup peak=\(peak)")
    }

    // ---- testAgcInstantCeil ----
    // 满幅（±32000）突发起音：0.8FS 瞬时限幅 |out| ≤ 26215（且不削波到 32767）。
    do {
        let pp = PcmPostprocessor(gainDb: 0.0) // hpf/agc 默认开
        let input = (0..<2000).map { i in Int16(i % 2 == 0 ? 32000 : -32000) }
        let out = pp.process(input)
        let peak = out.reduce(0) { max($0, abs(Int($1))) }
        check(peak <= 26215, "AgcInstantCeil peak=\(peak)")
        check(peak >= 26000, "AgcInstantCeil signalLost peak=\(peak)")
    }

    // ---- testResetIdempotent ----
    // reset() 后同一输入输出逐样本一致（整段与 640 样本帧流式两种喂法都验；
    // 注意三点平滑按 process 调用粒度作用，帧边界样本两种喂法本就不同，
    // 故流式对照组用新实例而非整段输出）。
    do {
        let mixed = (0..<16000).map { i -> Int16 in
            let v = 6000.0 * sin(2.0 * .pi * 300.0 * Double(i) / 16000.0)
                + 2000.0 * sin(2.0 * .pi * 1.0 * Double(i) / 16000.0)
            return Int16(clamping: Int(v.rounded()))
        }
        let pp = PcmPostprocessor(gainDb: 0.0) // hpf/agc 默认开
        let full = pp.process(mixed)
        pp.reset()
        checkEqual(pp.process(mixed), full, "ResetIdempotent.afterReset")
        // 流式（生产按 640 样本帧切片喂）：reset 后与全新实例逐样本一致。
        pp.reset()
        var chunked: [Int16] = []
        for start in stride(from: 0, to: mixed.count, by: 640) {
            chunked.append(contentsOf: pp.process(Array(mixed[start..<min(start + 640, mixed.count)])))
        }
        let fresh = PcmPostprocessor(gainDb: 0.0)
        var chunkedFresh: [Int16] = []
        for start in stride(from: 0, to: mixed.count, by: 640) {
            chunkedFresh.append(contentsOf: fresh.process(Array(mixed[start..<min(start + 640, mixed.count)])))
        }
        checkEqual(chunked, chunkedFresh, "ResetIdempotent.chunked")
    }
}
