#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace voicestick {

// Opus 编码器封装：把桌面端（小米遥控器链路）解码后的 PCM 重新编码为 Opus，
// 产出与固件相同规格的帧，下游 Ogg mux/ASR/字幕/wechat 零改动。
// 参数严格对齐固件 audio_pipeline：16 kHz 单声道、OPUS_APPLICATION_VOIP、
// VBR 关、bitrate 32000、DTX 关、complexity 1、OPUS_SIGNAL_VOICE；帧长 40ms=640 采样。
class AudioOpusEncoder {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kChannels = 1;
    static constexpr int kFrameSamples = 640;  // 40 ms
    static constexpr int kBitrate = 32000;

    struct Result {
        // 实际编码出的字节数。失败时为 0。
        int encoded_bytes = 0;
        // Opus 错误码（OPUS_OK=0 表示成功，负值表示失败）。
        int opus_error = 0;
    };

    AudioOpusEncoder();
    ~AudioOpusEncoder();

    AudioOpusEncoder(const AudioOpusEncoder&) = delete;
    AudioOpusEncoder& operator=(const AudioOpusEncoder&) = delete;

    // 编码单个 PCM 帧。samples 必须是 Opus 合法帧长（本类按 640 使用）。
    Result Encode(const std::int16_t* pcm, std::size_t samples,
                  std::uint8_t* opus_out, std::size_t opus_capacity);
    // 重置编码器内部状态（切换会话时）。
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// PCM 组帧器：把任意长度的 16 kHz 单声道 PCM 累积切成 640 采样（40ms）帧，
// 与固件 AUDIO_FRAME_MS 40 对齐；攒满即吐，余量跨调用保留。
class OpusFrameSlicer {
public:
    explicit OpusFrameSlicer(std::size_t frame_samples = AudioOpusEncoder::kFrameSamples);

    void Reset();
    // 追加 PCM，返回本次凑满的所有完整帧。
    std::vector<std::vector<std::int16_t>> Append(std::span<const std::int16_t> pcm);
    // 不足一帧的余量（只读；会话结束时由调用方决定补零编码或丢弃）。
    const std::vector<std::int16_t>& remainder() const { return buffer_; }
    // 取出余量并清空（避免重复取）。
    std::vector<std::int16_t> TakeRemainder();

private:
    std::size_t frame_samples_;
    std::vector<std::int16_t> buffer_;
};

} // namespace voicestick
