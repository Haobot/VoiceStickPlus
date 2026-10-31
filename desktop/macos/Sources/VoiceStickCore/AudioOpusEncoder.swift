import COpus
import Foundation

/// Opus 编码器封装：把小米遥控器链路解码后的 PCM 重新编码为 Opus，
/// 产出与固件相同规格的帧，下游 Ogg mux/ASR/字幕/wechat 零改动。
/// 参数严格对齐固件 audio_pipeline 与 Windows AudioOpusEncoder：16 kHz 单声道、
/// OPUS_APPLICATION_VOIP、VBR 关、bitrate 32000、DTX 关、complexity 1、OPUS_SIGNAL_VOICE；
/// 帧长 40ms = 640 采样。
public final class AudioOpusEncoder {
    public static let sampleRate = 16000
    public static let channels = 1
    public static let frameSamples = 640  // 40 ms
    public static let bitrate = 32000

    /// 32kbps × 40ms ≈ 160B，余量充足（与 Windows 侧缓冲一致）。
    private static let maxEncodedBytes = 1500

    public enum EncoderError: Error {
        /// opus_encoder_create 失败，code 为 Opus 错误码。
        case createFailed(code: Int32)
        /// 逐项设置编码参数失败，code 为 Opus 错误码。
        case configureFailed(code: Int32)
    }

    private var encoder: UnsafeMutableRawPointer?

    public init() throws {
        var error: Int32 = 0
        let handle = vs_opus_encoder_create_16k(&error)
        guard let handle, error == 0 else {
            throw EncoderError.createFailed(code: error)
        }
        encoder = handle
        // 与固件 audio_pipeline 的编码参数逐项对齐。
        for code in [
            vs_opus_set_vbr(handle, 0),
            vs_opus_set_bitrate(handle, Int32(Self.bitrate)),
            vs_opus_set_dtx(handle, 0),
            vs_opus_set_complexity(handle, 1),
            vs_opus_set_signal_voice(handle),
        ] where code != 0 {
            vs_opus_encoder_destroy(handle)
            encoder = nil
            throw EncoderError.configureFailed(code: code)
        }
    }

    deinit {
        if let encoder {
            vs_opus_encoder_destroy(encoder)
        }
    }

    /// 编码单个 PCM 帧。pcm 必须是 640 采样（40ms）；错误（opus_encode 返回 <0）
    /// 返回 nil。0 字节为合法空 payload 帧（对齐 Windows：只把 <0 当错误；实际
    /// 参数下不可达，纯语义对齐），返回空 Data 不丢帧。
    public func encode(_ pcm: [Int16]) -> Data? {
        guard let encoder, pcm.count == Self.frameSamples else { return nil }
        let capacity = Self.maxEncodedBytes
        var out = [UInt8](repeating: 0, count: capacity)
        let encoded = pcm.withUnsafeBufferPointer { pcmPtr in
            out.withUnsafeMutableBufferPointer { outPtr in
                vs_opus_encode(encoder, pcmPtr.baseAddress, Int32(pcm.count),
                               outPtr.baseAddress, Int32(capacity))
            }
        }
        guard encoded >= 0 else { return nil }
        return Data(out[0..<Int(encoded)])
    }

    /// 重置编码器内部状态（切换会话时）。
    public func reset() {
        guard let encoder else { return }
        vs_opus_reset_state(encoder)
    }
}

/// PCM 组帧器：把任意长度的 16 kHz 单声道 PCM 累积切成 640 采样（40ms）帧，
/// 与固件 AUDIO_FRAME_MS 40 对齐；攒满即吐，余量跨调用保留。
public final class OpusFrameSlicer {
    private let frameSamples: Int
    private var buffer: [Int16] = []

    public init(frameSamples: Int = AudioOpusEncoder.frameSamples) {
        self.frameSamples = frameSamples
        buffer.reserveCapacity(frameSamples)
    }

    /// 不足一帧的余量长度（会话结束时由调用方决定补零编码或丢弃）。
    public var remainderCount: Int { buffer.count }

    public func reset() {
        buffer.removeAll(keepingCapacity: true)
    }

    /// 追加 PCM，返回本次凑满的所有完整帧。
    public func append(_ pcm: [Int16]) -> [[Int16]] {
        var frames: [[Int16]] = []
        guard frameSamples > 0 else { return frames }
        buffer.append(contentsOf: pcm)
        while buffer.count >= frameSamples {
            frames.append(Array(buffer[0..<frameSamples]))
            buffer.removeFirst(frameSamples)
        }
        return frames
    }

    /// 取出余量并清空（避免重复取）。
    public func takeRemainder() -> [Int16] {
        let out = buffer
        buffer.removeAll(keepingCapacity: true)
        return out
    }
}
