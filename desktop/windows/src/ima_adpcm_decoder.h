#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace voicestick {

// IMA/DVI ADPCM 解码器（1992 公开标准算法）：4 bit/采样，每字节高半字节优先，
// 16 kHz 单声道。步长表/索引表为标准常数。遥控器会话内 predictor/step 连续推进，
// 丢包即漂移，直到下次 Reset（流开始硬重置或 AUDIO_SYNC 按值重置）。
class ImaAdpcmDecoder {
public:
    // 重置解码状态。step_index 钳位到 [0, 88]。
    void Reset(std::int16_t predictor = 0, int step_index = 0);
    // 解码一段 ADPCM 字节流，每字节拆高/低两个 nibble 各产出一个 int16 样本。
    std::vector<std::int16_t> Decode(std::span<const std::uint8_t> data);

    std::int16_t predictor() const { return predictor_; }
    int step_index() const { return step_index_; }

private:
    std::int16_t predictor_ = 0;
    int step_index_ = 0;
};

} // namespace voicestick
