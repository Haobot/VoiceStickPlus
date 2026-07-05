// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "audio_opus_decoder.h"

#include <opus.h>

#include <algorithm>
#include <cassert>

namespace voicestick {

struct AudioOpusDecoder::Impl {
  OpusDecoder* decoder = nullptr;
  int sample_rate = 16000;
  int channels = 1;
};

namespace {

int OpusErrorCode(int opus_api_return_value) {
  // opus_decode 返回样本数（≥0）或错误码（负值），与 OPUS_* 错误码定义一致。
  return opus_api_return_value;
}

}  // namespace

AudioOpusDecoder::AudioOpusDecoder(int sample_rate, int channels)
    : impl_(std::make_unique<Impl>()) {
  impl_->sample_rate = sample_rate;
  impl_->channels = channels;

  int error = OPUS_OK;
  impl_->decoder = opus_decoder_create(sample_rate, channels, &error);
  assert(impl_->decoder != nullptr && error == OPUS_OK);
}

AudioOpusDecoder::~AudioOpusDecoder() {
  if (impl_ && impl_->decoder != nullptr) {
    opus_decoder_destroy(impl_->decoder);
    impl_->decoder = nullptr;
  }
}

AudioOpusDecoder::Result AudioOpusDecoder::Decode(const uint8_t* opus_data,
                                                  std::size_t opus_len,
                                                  int16_t* pcm_out,
                                                  std::size_t pcm_capacity) {
  if (impl_ == nullptr || impl_->decoder == nullptr) {
    return Result{.opus_error = OPUS_INVALID_STATE};
  }
  if (opus_data == nullptr || opus_len == 0) {
    return Result{.opus_error = OPUS_BAD_ARG};
  }
  if (pcm_out == nullptr || pcm_capacity == 0) {
    return Result{.opus_error = OPUS_BUFFER_TOO_SMALL};
  }

  const int decoded_samples = opus_decode(
      impl_->decoder, opus_data, static_cast<opus_int32>(opus_len),
      pcm_out, static_cast<int>(pcm_capacity), /*decode_fec=*/0);

  if (decoded_samples < 0) {
    return Result{.opus_error = OpusErrorCode(decoded_samples)};
  }

  return Result{
      .decoded_samples = decoded_samples,
      .opus_error = OPUS_OK,
  };
}

void AudioOpusDecoder::Reset() {
  if (impl_ == nullptr || impl_->decoder == nullptr) {
    return;
  }
  opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
}

}  // namespace voicestick
