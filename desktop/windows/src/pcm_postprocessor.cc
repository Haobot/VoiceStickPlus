#include "pcm_postprocessor.h"

#include <algorithm>
#include <cmath>

namespace voicestick {

PcmPostprocessor::PcmPostprocessor(double gain_db) : gain_db_(ClampGainDb(gain_db)) {}

double PcmPostprocessor::ClampGainDb(double gain_db) {
    return std::clamp(gain_db, -kMaxGainDb, kMaxGainDb);
}

std::vector<std::int16_t> PcmPostprocessor::Process(std::span<const std::int16_t> pcm) const {
    const double gain = std::pow(10.0, gain_db_ / 20.0);
    std::vector<std::int16_t> out(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        // 三点平滑（首尾样本不动）：out[i]=(in[i-1]+2*in[i]+in[i+1])>>2。
        std::int32_t smoothed = pcm[i];
        if (pcm.size() >= 3 && i > 0 && i + 1 < pcm.size()) {
            smoothed = (static_cast<std::int32_t>(pcm[i - 1]) +
                        2 * static_cast<std::int32_t>(pcm[i]) +
                        static_cast<std::int32_t>(pcm[i + 1])) >> 2;
        }
        const long scaled = std::lround(smoothed * gain);
        out[i] = static_cast<std::int16_t>(std::clamp<long>(scaled, -32768, 32767));
    }
    return out;
}

FrameAccumulator::FrameAccumulator(std::size_t frame_bytes) : frame_bytes_(frame_bytes) {
    buffer_.reserve(frame_bytes);
}

void FrameAccumulator::set_frame_bytes(std::size_t frame_bytes) {
    frame_bytes_ = frame_bytes;
    Reset();
}

void FrameAccumulator::Reset() {
    buffer_.clear();
}

std::vector<ByteVector> FrameAccumulator::Append(std::span<const std::uint8_t> data) {
    std::vector<ByteVector> frames;
    if (frame_bytes_ == 0) return frames;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    while (buffer_.size() >= frame_bytes_) {
        frames.emplace_back(buffer_.begin(),
                            buffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes_));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes_));
    }
    return frames;
}

} // namespace voicestick
