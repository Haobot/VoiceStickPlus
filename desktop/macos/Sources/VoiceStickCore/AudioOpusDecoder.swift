import COpus
import Foundation

/// Opus 解码器封装：把 Opus packet 解码为 16 kHz 单声道 PCM16，供本地 ASR
/// （SenseVoice）消费 BLE 链路经 Ogg 解复用得到的音频帧。
/// 与 Windows `audio_opus_decoder.cc` 同口径：opus_decode 只把 <0 当错误；
/// preskip 不消费（生产 muxer preskip=0，见 OggOpusDemuxer 注释）。
public final class AudioOpusDecoder {
    public static let sampleRate = 16000
    public static let channels = 1
    /// 最大 Opus 帧长（120ms @16kHz）余量，与 Windows 侧缓冲一致。
    private static let maxFrameSamples = 5760

    public enum DecoderError: Error {
        case createFailed(code: Int32)
    }

    private var decoder: UnsafeMutableRawPointer?

    public init() throws {
        var error: Int32 = 0
        let handle = vs_opus_decoder_create_16k(&error)
        guard let handle, error == 0 else {
            throw DecoderError.createFailed(code: error)
        }
        decoder = handle
    }

    deinit {
        if let decoder {
            vs_opus_decoder_destroy(decoder)
        }
    }

    /// 解码单个 Opus packet；错误（opus_decode 返回 <0）返回 nil。
    public func decode(_ packet: Data) -> [Int16]? {
        guard let decoder, !packet.isEmpty else { return nil }
        var out = [Int16](repeating: 0, count: Self.maxFrameSamples)
        let decoded = packet.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Int32 in
            guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else {
                return -1
            }
            return vs_opus_decode(decoder, base, Int32(packet.count), &out,
                                  Int32(Self.maxFrameSamples), 0)
        }
        guard decoded > 0 else { return nil }
        return Array(out[0..<Int(decoded)])
    }

    /// 复位解码器状态（会话边界丢弃上一会话的解码器内部状态）。
    public func reset() {
        // shim 未暴露 OPUS_RESET_STATE ctl：16k 定参解码无状态泄漏面，丢弃实例
        // 重建等价（调用方在新会话整体重建增量状态时一并重建解码器）。
        if let decoder {
            vs_opus_decoder_destroy(decoder)
        }
        var error: Int32 = 0
        decoder = vs_opus_decoder_create_16k(&error)
    }
}
