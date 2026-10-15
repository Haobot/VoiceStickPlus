#include "pcm_postprocessor.h"

#include <algorithm>
#include <cmath>

namespace voicestick {

PcmPostprocessor::PcmPostprocessor(double gain_db, bool hpf, bool agc)
    : gain_db_(ClampGainDb(gain_db)), hpf_enabled_(hpf), agc_enabled_(agc) {}

double PcmPostprocessor::ClampGainDb(double gain_db) {
    return std::clamp(gain_db, -kMaxGainDb, kMaxGainDb);
}

void PcmPostprocessor::Reset() {
    hpf_z1_ = 0.0;
    hpf_z2_ = 0.0;
    agc_env_ = 0.0;
    agc_gain_ = 1.0;
    agc_nf_ = kAgcNfSentinel;
    agc_index_ = 0;
    agc_nf_ready_ = false;
    agc_nf_seen_ = false;
}

std::vector<std::int16_t> PcmPostprocessor::Process(std::span<const std::int16_t> pcm) {
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
        int v = static_cast<int>(smoothed);
        if (hpf_enabled_) v = hpf_process(v);
        const long scaled = std::lround(static_cast<double>(v) * gain);
        v = static_cast<int>(std::clamp<long>(scaled, -32768, 32767));
        if (agc_enabled_) v = agc_process(v);
        out[i] = static_cast<std::int16_t>(v);
    }
    return out;
}

int PcmPostprocessor::hpf_process(int x) {
    const double in = static_cast<double>(x);
    double y = kHpfB0 * in + hpf_z1_;
    hpf_z1_ = kHpfB1 * in - kHpfA1 * y + hpf_z2_;
    hpf_z2_ = kHpfB2 * in - kHpfA2 * y;
    // 软限幅防数值漂移越界（biquad 稳定但极端输入下保险）。
    y = std::clamp(y, -32768.0, 32767.0);
    return static_cast<int>(std::lround(y));
}

int PcmPostprocessor::agc_process(int x) {
    const double ax = std::fabs(static_cast<double>(x));
    // 包络冷启动：首样本直接取 |x|，避免包络从 0 爬升污染本底估计。
    if (agc_index_ == 0) {
        agc_env_ = ax;
    } else {
        const double ec = ax > agc_env_ ? kAgcEnvAttack : kAgcEnvRelease;
        agc_env_ = ec * agc_env_ + (1.0 - ec) * ax;
    }

    if (!agc_nf_ready_) {
        // 冷启动窗：只测本底；本底可信前直通（仅保留 0.8FS 限幅）。
        if (agc_index_ >= kAgcNfSkipSamples) {
            if (agc_env_ < agc_nf_) agc_nf_ = agc_env_;
            if (agc_env_ >= kAgcNfTrustMin) agc_nf_seen_ = true;
        }
        if ((agc_index_ >= kAgcNfWarmSamples && agc_nf_seen_ && agc_nf_ <= kAgcNfMaxTrust) ||
            agc_index_ >= kAgcNfWarmMaxSamples) {
            double nf = agc_nf_seen_ ? agc_nf_ : kAgcNfAbs;
            if (nf < kAgcNfTrustMin) nf = kAgcNfAbs;
            else if (nf > kAgcNfMaxTrust) nf = kAgcNfMaxTrust;
            agc_nf_ = nf;
            agc_nf_ready_ = true;
        }
        ++agc_index_;
        double g0 = 1.0;
        if (ax > 1.0) g0 = std::min(g0, kAgcCeil / ax);
        double y0 = static_cast<double>(x) * g0;
        y0 = std::clamp(y0, -32768.0, 32767.0);
        return static_cast<int>(std::lround(y0));
    }

    // 本底跟踪：瞬时下降；仅 env < 2×nf（噪声段）时缓升；上限 -45dBFS。
    if (agc_env_ < agc_nf_) {
        agc_nf_ = agc_env_;
    } else if (agc_env_ < agc_nf_ * kAgcNfNear) {
        agc_nf_ += (agc_env_ - agc_nf_) * kAgcNfRise;
    }
    if (agc_nf_ > kAgcNfMaxTrust) agc_nf_ = kAgcNfMaxTrust;

    const bool gated = agc_env_ < std::max(kAgcNfAbs, agc_nf_ * kAgcNfRatio);
    double desired = std::clamp(kAgcTarget / std::max(agc_env_, 1.0), kAgcMinGain, kAgcMaxGain);
    if (gated) {
        desired = std::min(desired, kAgcNoiseBudget / std::max(agc_env_, 1.0));
    }
    const double gc = (desired < agc_gain_)
                          ? kAgcGainDown
                          : (desired > kAgcFastRatio * agc_gain_ ? kAgcGainUpFast : kAgcGainUp);
    agc_gain_ = gc * agc_gain_ + (1.0 - gc) * desired;
    // 瞬时峰值限幅：无记忆，不回写 agc_gain_（压住增益挂在高位时的突发起音）。
    double g = agc_gain_;
    if (ax > 1.0) g = std::min(g, kAgcCeil / ax);
    double y = static_cast<double>(x) * g;
    y = std::clamp(y, -32768.0, 32767.0);
    return static_cast<int>(std::lround(y));
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
