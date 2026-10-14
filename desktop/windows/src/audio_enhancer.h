// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// ASR 前桌面端人声增强/降噪（RNNoise PoC，镜像 macOS AudioEnhancer.swift）：
// Opus(16k) → 解码上采样 48k → RNNoise（4×10ms 帧）→ 48 tap FIR 低通 +
// 3:1 抽取回 16k → 重新 Opus 编码（参数经 AudioOpusEncoder 与固件逐项对齐）。
//
// RNNoise 固定 48kHz/480 样本（10ms）帧；固件 40ms 帧经 48k 解码得 1920 样本
// = 4×480。48k→16k 抽取用 48 tap Blackman-sinc 低通（fc=6.5kHz：8kHz -25.4dB、
// 9kHz -57.4dB，通带到 6.5kHz 平直），群延迟 23.5 样本 ≈0.5ms。
// 增加总时延 ≈ 10ms（RNNoise 帧长）+ 0.5ms（FIR），对首字延迟无感。
//
// 生命周期：每个录音会话由协调器创建新实例，会话间无状态泄漏。任何一步异常
// （解码样本数异常/编码失败）ProcessFrame 返回 nullopt，调用方回退透传原始
// Opus 帧（增强是增强，不是单点故障）。

#ifndef VOICESTICK_AUDIO_ENHANCER_H_
#define VOICESTICK_AUDIO_ENHANCER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "byte_utils.h"

namespace voicestick {

class AudioEnhancer {
 public:
  // 输入帧：40ms @16kHz = 640 采样（固件 audio_pipeline 帧长）。
  static constexpr int kInputFrameSamples = 640;
  // RNNoise 帧：10ms @48kHz = 480 采样；一个输入帧 = 4 个 RNNoise 帧。
  static constexpr int kRnnoiseFrameSamples = 480;
  // 48kHz 域的 40ms = 1920 采样。
  static constexpr int kDecodedFrameSamples = 1920;

  // 创建实例；Opus 解码器或 RNNoise 状态分配失败时返回 nullptr。
  static std::unique_ptr<AudioEnhancer> Create();

  ~AudioEnhancer();

  AudioEnhancer(const AudioEnhancer&) = delete;
  AudioEnhancer& operator=(const AudioEnhancer&) = delete;

  // 处理一个 40ms Opus 帧，返回增强后的 Opus 帧；失败返回 std::nullopt，
  // 调用方回退原始帧。
  std::optional<ByteVector> ProcessFrame(const ByteVector& opus_payload);

 private:
  AudioEnhancer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voicestick

#endif  // VOICESTICK_AUDIO_ENHANCER_H_
