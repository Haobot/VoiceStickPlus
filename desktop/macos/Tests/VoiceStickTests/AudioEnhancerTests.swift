import COpus
import Foundation
import VoiceStickCore

/// AudioEnhancer（RNNoise 桌面端降噪 PoC）测试。
/// 固定种子伪随机保证确定性；断言边界留有 ≥2x 余量防平台数值抖动。

/// 16k 解码回环（与 AudioOpusEncoderTests 同款薄封装）。
private final class EnhTestDecoder {
    private var handle: UnsafeMutableRawPointer?

    init?() {
        var error: Int32 = 0
        handle = vs_opus_decoder_create_16k(&error)
        guard handle != nil, error == 0 else { return nil }
    }

    deinit {
        if let handle { vs_opus_decoder_destroy(handle) }
    }

    func decode(_ packet: Data, frameSamples: Int) -> [Int16]? {
        guard let handle, !packet.isEmpty else { return nil }
        var out = [Int16](repeating: 0, count: frameSamples)
        let decoded = packet.withUnsafeBytes { raw -> Int32 in
            out.withUnsafeMutableBufferPointer { outPtr in
                vs_opus_decode(handle,
                               raw.baseAddress?.assumingMemoryBound(to: UInt8.self),
                               Int32(raw.count),
                               outPtr.baseAddress,
                               Int32(frameSamples),
                               0)
            }
        }
        guard decoded > 0 else { return nil }
        return Array(out.prefix(Int(decoded)))
    }
}

/// 确定性伪随机（LCG），避免 arc4random 引入不可复现性。
private struct Lcg {
    var state: UInt64
    mutating func next() -> Double {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return Double((state >> 33) & 0xFFFFFF) / Double(0x800000) - 1.0
    }
}

private func rms(_ samples: [Int16]) -> Double {
    guard !samples.isEmpty else { return 0 }
    let sum = samples.reduce(0.0) { $0 + Double($1) * Double($1) }
    return (sum / Double(samples.count)).squareRoot()
}

/// 把一个 PCM 会话切成 40ms 帧依次编码 → 增强 → 解码，返回增强后整段 PCM。
/// 任一帧失败返回 nil（调用方 check 失败）。
private func enhanceSession(pcm: [Int16], enhancer: AudioEnhancer) -> [Int16]? {
    guard let encoder = try? AudioOpusEncoder(), let decoder = EnhTestDecoder() else {
        return nil
    }
    var out: [Int16] = []
    for start in stride(from: 0, to: pcm.count, by: AudioOpusEncoder.frameSamples) {
        let frame = Array(pcm[start..<min(start + AudioOpusEncoder.frameSamples, pcm.count)])
        guard frame.count == AudioOpusEncoder.frameSamples,
              let packet = encoder.encode(frame),
              let enhancedPacket = enhancer.processFrame(packet),
              let decoded = decoder.decode(enhancedPacket, frameSamples: AudioOpusEncoder.frameSamples)
        else { return nil }
        out.append(contentsOf: decoded)
    }
    return out
}

func runAudioEnhancerTests() {
    // ---- testEnhancerFrameShape ----
    // 单帧 440Hz 正弦：增强后仍是合法 Opus 帧，16k 解码回 640 样本。
    if let enhancer = try? AudioEnhancer(),
       let encoder = try? AudioOpusEncoder(),
       let decoder = EnhTestDecoder() {
        let frame = (0..<AudioOpusEncoder.frameSamples).map {
            Int16(sin(2.0 * .pi * 440.0 * Double($0) / 16000.0) * 10000.0)
        }
        if let packet = unwrap(encoder.encode(frame), "EnhancerFrameShape.encode") {
            let enhanced = enhancer.processFrame(packet)
            check(enhanced != nil && !(enhanced?.isEmpty ?? true), "EnhancerFrameShape.nonEmpty")
            if let enhanced,
               let decoded = unwrap(decoder.decode(enhanced, frameSamples: AudioOpusEncoder.frameSamples),
                                    "EnhancerFrameShape.decode") {
                checkEqual(decoded.count, AudioOpusEncoder.frameSamples, "EnhancerFrameShape.640")
            }
        }
    } else {
        check(false, "EnhancerFrameShape.init")
    }

    // ---- testEnhancerNoiseSuppression ----
    // 2s 平稳白噪声（rms≈1000 ≈ -30dBFS）：RNNoise 收敛后尾部 1s 输出 rms
    // 须压到输入的 30% 以下（典型 -20dB 以上，断言留大余量）。
    do {
        var lcg = Lcg(state: 0x12345678)
        let noise = (0..<32000).map { _ in Int16(clamping: Int(lcg.next() * 1700.0)) }
        let inRms = rms(noise)
        check(inRms > 700 && inRms < 1300, "EnhancerNoise.inputRms \(inRms)")
        if let enhancer = try? AudioEnhancer(),
           let out = unwrap(enhanceSession(pcm: noise, enhancer: enhancer), "EnhancerNoise.session") {
            let outRms = rms(Array(out.suffix(16000)))
            check(outRms < inRms * 0.3,
                  "EnhancerNoise.suppression in=\(inRms) out=\(outRms) ratio=\(outRms / inRms)")
        }
    }

    // ---- testEnhancerSpeechPassthrough ----
    // 类语音信号（200+800Hz 复合音，4Hz 音节级幅度调制，1.52s=38 帧整帧对齐）：
    // 尾部 0.5s 输出 rms 保留 ≥ 45%（RNNoise 不应把持续有声段当噪声吃掉）。
    do {
        let speech = (0..<24320).map { i -> Int16 in
            let t = Double(i) / 16000.0
            let am = 0.55 + 0.45 * sin(2.0 * .pi * 4.0 * t)
            let v = am * (0.6 * sin(2.0 * .pi * 200.0 * t) + 0.4 * sin(2.0 * .pi * 800.0 * t)) * 9000.0
            return Int16(clamping: Int(v.rounded()))
        }
        let inRms = rms(Array(speech.suffix(8000)))
        if let enhancer = try? AudioEnhancer(),
           let out = unwrap(enhanceSession(pcm: speech, enhancer: enhancer), "EnhancerSpeech.session") {
            let outRms = rms(Array(out.suffix(8000)))
            check(outRms > inRms * 0.45,
                  "EnhancerSpeech.passthrough in=\(inRms) out=\(outRms) ratio=\(outRms / inRms)")
        }
    }
}
