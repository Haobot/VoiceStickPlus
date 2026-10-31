import COpus
import CRnnoise
import Foundation

/// ASR 前桌面端人声增强/降噪：Opus(16k) → 解码上采样 48k → RNNoise（4×10ms 帧）
/// → 48 tap FIR 低通 + 3:1 抽取回 16k → 重新 Opus 编码（参数经 AudioOpusEncoder
/// 与固件逐项对齐：VOIP/CBR 32k/complexity 1/40ms）。
///
/// RNNoise 固定 48kHz/480 样本（10ms）帧；固件 40ms 帧经 48k 解码得 1920 样本
/// = 4×480。48k→16k 抽取用 48 tap Blackman-sinc 低通（fc=6.5kHz：8kHz -25.4dB、
/// 9kHz -57.4dB，通带到 6.5kHz 平直），群延迟 23.5 样本 ≈0.5ms。
/// 增加总时延 ≈ 10ms（RNNoise 帧长）+ 0.5ms（FIR），对首字延迟无感。
///
/// 生命周期：每个录音会话由协调器创建新实例（deinit 释放 Opus 解码器与 RNNoise
/// 状态），会话间无状态泄漏。处理失败（解码样本数异常/编码失败）返回 nil，
/// 调用方回退透传原始 Opus 帧（增强是增强，不是单点故障）。
public final class AudioEnhancer {
    /// 输入帧：40ms @16kHz = 640 采样（固件 audio_pipeline 帧长）。
    public static let inputFrameSamples = 640
    /// RNNoise 帧：10ms @48kHz = 480 采样；一个输入帧 = 4 个 RNNoise 帧。
    public static let rnnoiseFrameSamples = 480
    /// 48kHz 域的 40ms = 1920 采样。
    public static let decodedFrameSamples = 1920

    public enum EnhancerError: Error {
        /// vs_opus_decoder_create_48k 失败，code 为 Opus 错误码。
        case decoderCreateFailed(code: Int32)
        /// rnnoise_create 失败（内存不足）。
        case rnnoiseCreateFailed
    }

    private var decoder: UnsafeMutableRawPointer?
    private var rnnoise: OpaquePointer?
    private let encoder: AudioOpusEncoder

    /// 48 tap Blackman-sinc 低通（fc=6.5kHz @48kHz），系数离线计算并验证频响。
    private static let firTaps: [Float] = [
        -0.00000000, 0.00000665, -0.00005158, -0.00023423, -0.00035639, -0.00002598,
        0.00092566, 0.00191413, 0.00163463, -0.00090202, -0.00483830, -0.00694808,
        -0.00347941, 0.00611010, 0.01638567, 0.01761786, 0.00272613, -0.02441944,
        -0.04620439, -0.03863295, 0.01353547, 0.10326436, 0.19973249, 0.26223963,
        0.26223963, 0.19973249, 0.10326436, 0.01353547, -0.03863295, -0.04620439,
        -0.02441944, 0.00272613, 0.01761786, 0.01638567, 0.00611010, -0.00347941,
        -0.00694808, -0.00483830, -0.00090202, 0.00163463, 0.00191413, 0.00092566,
        -0.00002598, -0.00035639, -0.00023423, -0.00005158, 0.00000665, -0.00000000,
    ]
    /// FIR 历史窗（最近 48 个 48kHz 输入样本，含当前）。
    private var firHist = [Float](repeating: 0, count: 48)
    /// 距下一个 3:1 抽取输出点还差几个 48kHz 样本（0 表示当前样本即输出点）。
    private var decimateCountdown = 0

    public init() throws {
        var error: Int32 = 0
        let dec = vs_opus_decoder_create_48k(&error)
        guard let dec, error == 0 else {
            throw EnhancerError.decoderCreateFailed(code: error)
        }
        guard let rnn = rnnoise_create(nil) else {
            vs_opus_decoder_destroy(dec)
            throw EnhancerError.rnnoiseCreateFailed
        }
        decoder = dec
        rnnoise = rnn
        encoder = try AudioOpusEncoder()
    }

    deinit {
        if let decoder { vs_opus_decoder_destroy(decoder) }
        if let rnnoise { rnnoise_destroy(rnnoise) }
    }

    /// 处理一个 40ms Opus 帧，返回增强后的 Opus 帧。
    /// 任何一步异常（解码 ≠1920 样本、编码失败）返回 nil，调用方回退原始帧。
    public func processFrame(_ opusPayload: Data) -> Data? {
        guard let decoder, let rnnoise else { return nil }

        // 1. Opus 解码（16k 流经解码器内部重采样上采样到 48k，40ms → 1920 样本）。
        var pcm48 = [Int16](repeating: 0, count: Self.decodedFrameSamples)
        let decoded: Int32 = opusPayload.withUnsafeBytes { ptr in
            guard let base = ptr.baseAddress else { return -1 }
            return vs_opus_decode(decoder, base.assumingMemoryBound(to: UInt8.self),
                                  Int32(opusPayload.count), &pcm48,
                                  Int32(Self.decodedFrameSamples), 0)
        }
        guard decoded == Int32(Self.decodedFrameSamples) else { return nil }

        // 2. RNNoise 4×480（float 为 int16 量纲）。
        var enhanced = [Float](repeating: 0, count: Self.decodedFrameSamples)
        var inBuf = [Float](repeating: 0, count: Self.rnnoiseFrameSamples)
        var outBuf = [Float](repeating: 0, count: Self.rnnoiseFrameSamples)
        for f in 0..<(Self.decodedFrameSamples / Self.rnnoiseFrameSamples) {
            let base = f * Self.rnnoiseFrameSamples
            for i in 0..<Self.rnnoiseFrameSamples {
                inBuf[i] = Float(pcm48[base + i])
            }
            rnnoise_process_frame(rnnoise, &outBuf, &inBuf)
            for i in 0..<Self.rnnoiseFrameSamples {
                enhanced[base + i] = outBuf[i]
            }
        }

        // 3. FIR 低通 + 3:1 抽取回 16k，量化为 int16。
        var out16 = [Int16]()
        out16.reserveCapacity(Self.inputFrameSamples)
        for sample in enhanced {
            if let y = decimatePush(sample) { out16.append(y) }
        }
        guard out16.count == Self.inputFrameSamples else { return nil }

        // 4. 重新编码（AudioOpusEncoder 参数与固件一致）。
        return encoder.encode(out16)
    }

    /// 喂一个 48kHz 样本到 FIR，到达抽取相位时产出一个 16kHz 样本，否则 nil。
    private func decimatePush(_ x: Float) -> Int16? {
        firHist.removeFirst()
        firHist.append(x)
        if decimateCountdown > 0 {
            decimateCountdown -= 1
            return nil
        }
        decimateCountdown = 2
        var y: Float = 0
        // y = Σ h[k]·x[n-k]；firHist[47] 是当前样本。
        for k in 0..<Self.firTaps.count {
            y += Self.firTaps[k] * firHist[Self.firTaps.count - 1 - k]
        }
        let clamped = max(-32768.0, min(32767.0, y))
        return Int16(clamped.rounded())  // .toNearestOrAwayFromZero，对齐 lround
    }
}
