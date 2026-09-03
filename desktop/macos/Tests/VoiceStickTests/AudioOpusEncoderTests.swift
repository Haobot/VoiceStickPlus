import COpus
import Foundation
import VoiceStickCore

/// 移植 Windows TestAudioOpusEncoderRoundTrip 的编码器/组帧器部分；
/// 解码侧用 COpus 的 vs_opus_decode 做回环（该 shim 本就为单测暴露）。
/// （旧 XCTest AudioOpusEncoderTests 逐条转换。）

/// 16 kHz、40ms（640 样本）单声道正弦一帧；sampleOffset 让多帧拼接保持相位连续。
private func makeSinePcm(frequencyHz: Double = 440, sampleOffset: Int = 0,
                         sampleRate: Int = 16000) -> [Int16] {
    let frameSize = sampleRate * 40 / 1000
    return (0..<frameSize).map { i in
        let t = Double(sampleOffset + i) / Double(sampleRate)
        return Int16(sin(2.0 * .pi * frequencyHz * t) * 30000.0)
    }
}

/// vs_opus_decode 薄封装（解码器 shim 仅供测试回环使用）。
private final class OpusDecoder {
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

private func makeEncoder(_ name: String) -> AudioOpusEncoder? {
    do {
        return try AudioOpusEncoder()
    } catch {
        check(false, "\(name): AudioOpusEncoder init threw \(error)")
        return nil
    }
}

func runAudioOpusEncoderTests() {
    // ---- testEncodeProducesNonEmptyPacket ----
    if let encoder = unwrap(makeEncoder("EncodeProducesNonEmptyPacket"), "EncodeProducesNonEmptyPacket.encoder"),
       let packet = unwrap(encoder.encode(makeSinePcm()), "EncodeProducesNonEmptyPacket.packet") {
        check(!packet.isEmpty, "EncodeProducesNonEmptyPacket.nonEmpty")
        // 非法帧长返回 nil（对齐 Windows 侧 opus_error != 0）。
        checkNil(encoder.encode(Array(makeSinePcm().prefix(319))),
                 "EncodeProducesNonEmptyPacket.shortFrameNil")
        checkNil(encoder.encode([]), "EncodeProducesNonEmptyPacket.emptyFrameNil")
    }

    // ---- testEncodeDecodeLoopbackSNR ----
    // 10 帧连续 440Hz 正弦做回环（codec 有固有时延，SNR 按最佳滞后对齐后计算）。
    if let encoder = unwrap(makeEncoder("EncodeDecodeLoopbackSNR"), "EncodeDecodeLoopbackSNR.encoder"),
       let decoder = unwrap(OpusDecoder(), "EncodeDecodeLoopbackSNR.decoder") {
        let frameCount = 10
        var pcmIn: [Int16] = []
        var pcmOut: [Int16] = []
        var loopOk = true
        for frameIndex in 0..<frameCount {
            let frame = makeSinePcm(sampleOffset: frameIndex * AudioOpusEncoder.frameSamples)
            guard let packet = unwrap(encoder.encode(frame),
                                      "EncodeDecodeLoopbackSNR.encode frame \(frameIndex)"),
                  let decoded = unwrap(
                    decoder.decode(packet, frameSamples: AudioOpusEncoder.frameSamples),
                    "EncodeDecodeLoopbackSNR.decode frame \(frameIndex)")
            else { loopOk = false; break }
            checkEqual(decoded.count, AudioOpusEncoder.frameSamples,
                       "EncodeDecodeLoopbackSNR.frameSize \(frameIndex)")
            pcmIn.append(contentsOf: frame)
            pcmOut.append(contentsOf: decoded)
        }

        if loopOk {
            // 搜索滞后 L（输出滞后输入），跳过前 1024 样本的起始瞬态。
            var bestSnrDb = -Double.infinity
            let n = min(pcmIn.count, pcmOut.count)
            for lag in 0...960 {
                var signal = 0.0
                var noise = 0.0
                var i = 1024
                while i + lag < n {
                    let x = Double(pcmIn[i])
                    let y = Double(pcmOut[i + lag])
                    signal += x * x
                    noise += (x - y) * (x - y)
                    i += 1
                }
                if noise == 0 {
                    bestSnrDb = .infinity
                    break
                }
                bestSnrDb = max(bestSnrDb, 10 * log10(signal / noise))
            }
            print(String(format: "Opus loopback SNR: %.2f dB (threshold > 15 dB)", bestSnrDb))
            check(bestSnrDb > 15.0, "EncodeDecodeLoopbackSNR.snr \(bestSnrDb) dB")
        }
    }

    // ---- testResetRestoresFirstFrame ----
    // 编码器 reset 后首帧须与全新实例逐字节一致（对齐固件 OPUS_RESET_STATE 语义）。
    if let fresh = unwrap(makeEncoder("ResetRestoresFirstFrame"), "ResetRestoresFirstFrame.fresh"),
       let reused = unwrap(makeEncoder("ResetRestoresFirstFrame"), "ResetRestoresFirstFrame.reused"),
       let freshFirst = unwrap(fresh.encode(makeSinePcm()), "ResetRestoresFirstFrame.freshFirst") {
        checkEqual(unwrap(reused.encode(makeSinePcm()), "ResetRestoresFirstFrame.sameState"),
                   freshFirst, "ResetRestoresFirstFrame.sameState")
        // 改变内部状态后不复位，同输入产出不同（SILK 预测器状态残留）。
        _ = reused.encode(makeSinePcm(frequencyHz: 880))
        _ = reused.encode(makeSinePcm(frequencyHz: 220))
        let dirty = reused.encode(makeSinePcm())
        check(dirty != nil && dirty != freshFirst, "ResetRestoresFirstFrame.dirtyDiffers")
        // reset 后首帧恢复一致。
        reused.reset()
        checkEqual(unwrap(reused.encode(makeSinePcm()), "ResetRestoresFirstFrame.afterReset"),
                   freshFirst, "ResetRestoresFirstFrame.afterReset")
    }

    // ---- testOpusFrameSlicer ----
    // 组帧器：不足 640 采样不吐帧，跨 append 累积，余量正确。
    do {
        let slicer = OpusFrameSlicer()
        check(slicer.append([Int16](repeating: 7, count: 300)).isEmpty, "OpusFrameSlicer.belowFrameSilent")
        checkEqual(slicer.remainderCount, 300, "OpusFrameSlicer.remainder300")
        let frames = slicer.append([Int16](repeating: 9, count: 400))
        checkEqual(frames.count, 1, "OpusFrameSlicer.oneFrame")
        if frames.count == 1 {
            checkEqual(frames[0].count, AudioOpusEncoder.frameSamples, "OpusFrameSlicer.frameSize")
            checkEqual(frames[0][0], 7, "OpusFrameSlicer.headFromFirst")
            checkEqual(frames[0][299], 7, "OpusFrameSlicer.headBoundary")
            checkEqual(frames[0][300], 9, "OpusFrameSlicer.tailFromSecond")
        }
        checkEqual(slicer.remainderCount, 60, "OpusFrameSlicer.remainder60")
        checkEqual(slicer.takeRemainder().count, 60, "OpusFrameSlicer.takeRemainder")
        checkEqual(slicer.remainderCount, 0, "OpusFrameSlicer.takeClears")
        slicer.reset()
        checkEqual(slicer.remainderCount, 0, "OpusFrameSlicer.resetClears")
    }
}
