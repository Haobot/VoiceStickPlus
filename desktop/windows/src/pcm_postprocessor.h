#pragma once

#include "byte_utils.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace voicestick {

// PCM 后处理：三点平滑（首尾样本不动）+ dB 增益（钳位 ±24 dB）+ int16 限幅。
// 平滑与增益一次遍历完成：out[i] = clamp((in[i-1]+2*in[i]+in[i+1])>>2 * gain)。
class PcmPostprocessor {
public:
    // 增益钳位范围（dB）。
    static constexpr double kMaxGainDb = 24.0;

    explicit PcmPostprocessor(double gain_db = 0.0);

    void set_gain_db(double gain_db) { gain_db_ = ClampGainDb(gain_db); }
    double gain_db() const { return gain_db_; }

    // 返回处理后的新缓冲；少于 3 个样本时跳过平滑只做增益。
    std::vector<std::int16_t> Process(std::span<const std::int16_t> pcm) const;

    static double ClampGainDb(double gain_db);

private:
    double gain_db_ = 0.0;
};

// ADPCM 裸字节流跨包累积切帧器：ATVV audio notify 无帧头无序号，
// 按 CAPS 协商帧长（默认 120 字节）累积，攒满一帧吐出一帧。
class FrameAccumulator {
public:
    explicit FrameAccumulator(std::size_t frame_bytes = 120);

    // 调整协商帧长并清空已累积字节（CAPS 到达时调用）。
    void set_frame_bytes(std::size_t frame_bytes);
    std::size_t frame_bytes() const { return frame_bytes_; }
    // 已累积、尚未凑满一帧的字节数。
    std::size_t pending_bytes() const { return buffer_.size(); }

    void Reset();
    // 追加字节，返回本次凑满的所有完整帧（可能为零或多帧）。
    std::vector<ByteVector> Append(std::span<const std::uint8_t> data);

private:
    std::size_t frame_bytes_;
    ByteVector buffer_;
};

} // namespace voicestick
