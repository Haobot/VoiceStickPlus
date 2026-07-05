// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// Opus 解码器封装。用于将 BLE 传来的 Opus packet 解码为 16-bit PCM，
// 供 wechat_input_method 输出模式渲染到虚拟麦克风。

#ifndef VOICESTICK_AUDIO_OPUS_DECODER_H_
#define VOICESTICK_AUDIO_OPUS_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace voicestick {

// 将 Opus packet 解码为 PCM。
// 当前仅支持 16 kHz 单声道（与固件端编码参数一致），但接口按通用参数设计，
// 便于后续扩展。
class AudioOpusDecoder {
 public:
  struct Result {
    // 实际解码出的每声道样本数。失败时为 0。
    int decoded_samples = 0;
    // Opus 错误码（OPUS_OK=0 表示成功，负值表示失败）。
    int opus_error = 0;
  };

  explicit AudioOpusDecoder(int sample_rate = 16000, int channels = 1);
  ~AudioOpusDecoder();

  AudioOpusDecoder(const AudioOpusDecoder&) = delete;
  AudioOpusDecoder& operator=(const AudioOpusDecoder&) = delete;

  // 解码单个 Opus packet。
  //
  // Args:
  //   opus_data:     Opus packet 数据。
  //   opus_len:      Opus packet 字节数。
  //   pcm_out:       输出 PCM 缓冲区。
  //   pcm_capacity:  缓冲区可容纳的样本数（每声道）。
  //
  // Returns:
  //   Result{decoded_samples, opus_error}。
  //   decoded_samples 表示实际写入 pcm_out 的每声道样本数。
  Result Decode(const uint8_t* opus_data, std::size_t opus_len,
                int16_t* pcm_out, std::size_t pcm_capacity);

  // 重置解码器内部状态（例如出现错误后或切换 session 时）。
  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voicestick

#endif  // VOICESTICK_AUDIO_OPUS_DECODER_H_
