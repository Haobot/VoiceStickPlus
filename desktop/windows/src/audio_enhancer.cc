// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "audio_enhancer.h"

#include <rnnoise.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "audio_opus_decoder.h"
#include "audio_opus_encoder.h"

namespace voicestick {

namespace {

// 48 tap Blackman-sinc 低通（fc=6.5kHz @48kHz），系数离线计算并验证频响，
// 与 macOS AudioEnhancer.swift 逐字一致。
constexpr std::array<float, 48> kFirTaps = {
    -0.00000000f, 0.00000665f, -0.00005158f, -0.00023423f, -0.00035639f,
    -0.00002598f, 0.00092566f, 0.00191413f, 0.00163463f, -0.00090202f,
    -0.00483830f, -0.00694808f, -0.00347941f, 0.00611010f, 0.01638567f,
    0.01761786f, 0.00272613f, -0.02441944f, -0.04620439f, -0.03863295f,
    0.01353547f, 0.10326436f, 0.19973249f, 0.26223963f, 0.26223963f,
    0.19973249f, 0.10326436f, 0.01353547f, -0.03863295f, -0.04620439f,
    -0.02441944f, 0.00272613f, 0.01761786f, 0.01638567f, 0.00611010f,
    -0.00347941f, -0.00694808f, -0.00483830f, -0.00090202f, 0.00163463f,
    0.00191413f, 0.00092566f, -0.00002598f, -0.00035639f, -0.00023423f,
    -0.00005158f, 0.00000665f, -0.00000000f,
};

// 重编码输出缓冲：40ms @16kHz/32kbps CBR ≈ 160B，取 Opus 包上限附近的宽裕值。
constexpr std::size_t kOpusOutCapacity = 512;

}  // namespace

struct AudioEnhancer::Impl {
  // 48kHz 解码器：16k 流经解码器内部重采样上采样到 48k。
  AudioOpusDecoder decoder{48000, 1};
  AudioOpusEncoder encoder;
  DenoiseState* rnnoise = nullptr;
  // FIR 历史窗（最近 48 个 48kHz 输入样本，含当前）。
  std::array<float, kFirTaps.size()> fir_hist{};
  // 距下一个 3:1 抽取输出点还差几个 48kHz 样本（0 表示当前样本即输出点）。
  int decimate_countdown = 0;

  ~Impl() {
    if (rnnoise != nullptr) {
      rnnoise_destroy(rnnoise);
      rnnoise = nullptr;
    }
  }
};

AudioEnhancer::AudioEnhancer() : impl_(std::make_unique<Impl>()) {}

AudioEnhancer::~AudioEnhancer() = default;

std::unique_ptr<AudioEnhancer> AudioEnhancer::Create() {
  std::unique_ptr<AudioEnhancer> enhancer;
  try {
    // AudioOpusEncoder 构造失败抛 std::runtime_error，统一归为创建失败。
    enhancer = std::unique_ptr<AudioEnhancer>(new AudioEnhancer());
  } catch (...) {
    return nullptr;
  }
  enhancer->impl_->rnnoise = rnnoise_create(nullptr);
  if (enhancer->impl_->rnnoise == nullptr) {
    return nullptr;
  }
  return enhancer;
}

std::optional<ByteVector> AudioEnhancer::ProcessFrame(const ByteVector& opus_payload) {
  if (impl_ == nullptr || impl_->rnnoise == nullptr || opus_payload.empty()) {
    return std::nullopt;
  }

  // 1. Opus 解码 @48k（40ms → 1920 样本）。
  std::array<std::int16_t, kDecodedFrameSamples> pcm48{};
  const auto dec = impl_->decoder.Decode(opus_payload.data(), opus_payload.size(),
                                         pcm48.data(), pcm48.size());
  if (dec.opus_error != 0 || dec.decoded_samples != kDecodedFrameSamples) {
    return std::nullopt;
  }

  // 2. RNNoise 4×480（float 为 int16 量纲）。
  std::array<float, kDecodedFrameSamples> enhanced{};
  std::array<float, kRnnoiseFrameSamples> in_buf{};
  std::array<float, kRnnoiseFrameSamples> out_buf{};
  for (int f = 0; f < kDecodedFrameSamples / kRnnoiseFrameSamples; ++f) {
    const int base = f * kRnnoiseFrameSamples;
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      in_buf[i] = static_cast<float>(pcm48[base + i]);
    }
    rnnoise_process_frame(impl_->rnnoise, out_buf.data(), in_buf.data());
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      enhanced[base + i] = out_buf[i];
    }
  }

  // 3. FIR 低通 + 3:1 抽取回 16k，量化为 int16。
  std::vector<std::int16_t> out16;
  out16.reserve(kInputFrameSamples);
  for (const float sample : enhanced) {
    // 喂一个 48kHz 样本到 FIR，到达抽取相位时产出一个 16kHz 样本。
    auto& hist = impl_->fir_hist;
    std::move(hist.begin() + 1, hist.end(), hist.begin());
    hist.back() = sample;
    if (impl_->decimate_countdown > 0) {
      --impl_->decimate_countdown;
      continue;
    }
    impl_->decimate_countdown = 2;
    float y = 0.0f;
    // y = Σ h[k]·x[n-k]；hist.back() 是当前样本。
    for (std::size_t k = 0; k < kFirTaps.size(); ++k) {
      y += kFirTaps[k] * hist[kFirTaps.size() - 1 - k];
    }
    y = std::clamp(y, -32768.0f, 32767.0f);
    out16.push_back(static_cast<std::int16_t>(std::lround(y)));
  }
  if (out16.size() != kInputFrameSamples) {
    return std::nullopt;
  }

  // 4. 重新编码（AudioOpusEncoder 参数与固件一致）。
  std::array<std::uint8_t, kOpusOutCapacity> opus_out{};
  const auto enc = impl_->encoder.Encode(out16.data(), out16.size(),
                                         opus_out.data(), opus_out.size());
  if (enc.opus_error != 0 || enc.encoded_bytes <= 0) {
    return std::nullopt;
  }
  return ByteVector(opus_out.begin(), opus_out.begin() + enc.encoded_bytes);
}

}  // namespace voicestick
