// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// OggOpus 反序列化：把 OggOpus 字节流解析为 Opus packet 序列 + OpusHead 元信息。
// 与 OggOpusMuxer 互为逆操作，用于离线重放已落盘的调试音频（.ogg）做延迟与流畅度测试。
//
// 支持标准 OggOpus：OggS 页结构 + OpusHead/OpusTags 头页 + 音频页。lacing 按通用规则
// 解析（segment_table 累加，<255 结束一个 packet，255 续段，支持跨页 packet），
// 兼容自家 muxer 产的"每页一 packet"简单格式与标准工具产物。

#ifndef VOICESTICK_OGG_OPUS_DEMUXER_H_
#define VOICESTICK_OGG_OPUS_DEMUXER_H_

#include <cstdint>
#include <span>
#include <vector>

#include "byte_utils.h"

namespace voicestick {

// OggOpus 流解析结果。packets 按播放顺序排列，可直接喂给 AudioOpusDecoder。
struct OggOpusStream {
  // OpusHead 字段。input_sample_rate 为封装声明的原始采样率（与解码无关，
  // Opus 内部恒 48kHz），preskip 为 48kHz 域需丢弃的前置样本数。
  int sample_rate = 16000;
  int channels = 1;
  int preskip = 0;
  int gain = 0;
  // 按顺序的 Opus packet，每帧通常 20ms（960 样本 @48kHz）。
  std::vector<ByteVector> packets;
};

// 解析整个 OggOpus 字节流。
// 成功返回 true 并填充 out；遇格式错误（bad magic、截断、OpusHead 缺失）返回 false。
// 部分解析结果仍写入 out（已解析的 packets 保留），调用方可据 has_error 判断。
bool ParseOggOpus(std::span<const std::uint8_t> data, OggOpusStream& out);

}  // namespace voicestick

#endif  // VOICESTICK_OGG_OPUS_DEMUXER_H_
