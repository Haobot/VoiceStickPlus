#include "audio_opus_encoder.h"

#include <opus.h>

#include <stdexcept>
#include <string>

namespace voicestick {

struct AudioOpusEncoder::Impl {
    OpusEncoder* encoder = nullptr;
};

AudioOpusEncoder::AudioOpusEncoder() : impl_(std::make_unique<Impl>()) {
    int error = OPUS_OK;
    impl_->encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &error);
    if (impl_->encoder == nullptr || error != OPUS_OK) {
        throw std::runtime_error("opus_encoder_create failed: " + std::to_string(error));
    }
    // 与固件 audio_pipeline 的编码参数逐项对齐。
    opus_encoder_ctl(impl_->encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(1));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

AudioOpusEncoder::~AudioOpusEncoder() {
    if (impl_ && impl_->encoder != nullptr) {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
    }
}

AudioOpusEncoder::Result AudioOpusEncoder::Encode(const std::int16_t* pcm, std::size_t samples,
                                                  std::uint8_t* opus_out,
                                                  std::size_t opus_capacity) {
    if (impl_ == nullptr || impl_->encoder == nullptr) {
        return Result{.opus_error = OPUS_INVALID_STATE};
    }
    if (pcm == nullptr || opus_out == nullptr || samples == 0 || opus_capacity == 0) {
        return Result{.opus_error = OPUS_BAD_ARG};
    }
    const int encoded = opus_encode(impl_->encoder, pcm, static_cast<int>(samples),
                                    opus_out, static_cast<opus_int32>(opus_capacity));
    if (encoded < 0) {
        return Result{.opus_error = encoded};
    }
    return Result{.encoded_bytes = encoded, .opus_error = OPUS_OK};
}

void AudioOpusEncoder::Reset() {
    if (impl_ == nullptr || impl_->encoder == nullptr) return;
    opus_encoder_ctl(impl_->encoder, OPUS_RESET_STATE);
}

OpusFrameSlicer::OpusFrameSlicer(std::size_t frame_samples) : frame_samples_(frame_samples) {
    buffer_.reserve(frame_samples);
}

void OpusFrameSlicer::Reset() {
    buffer_.clear();
}

std::vector<std::vector<std::int16_t>> OpusFrameSlicer::Append(std::span<const std::int16_t> pcm) {
    std::vector<std::vector<std::int16_t>> frames;
    if (frame_samples_ == 0) return frames;
    buffer_.insert(buffer_.end(), pcm.begin(), pcm.end());
    while (buffer_.size() >= frame_samples_) {
        frames.emplace_back(buffer_.begin(),
                            buffer_.begin() + static_cast<std::ptrdiff_t>(frame_samples_));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(frame_samples_));
    }
    return frames;
}

std::vector<std::int16_t> OpusFrameSlicer::TakeRemainder() {
    std::vector<std::int16_t> out;
    out.swap(buffer_);
    return out;
}

} // namespace voicestick
