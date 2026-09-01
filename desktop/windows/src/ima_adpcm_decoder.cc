#include "ima_adpcm_decoder.h"

#include <algorithm>

namespace voicestick {

namespace {

// IMA/DVI ADPCM 公开标准（1992）常数表。
constexpr int kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
constexpr int kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

} // namespace

void ImaAdpcmDecoder::Reset(std::int16_t predictor, int step_index) {
    predictor_ = predictor;
    step_index_ = std::clamp(step_index, 0, 88);
}

std::vector<std::int16_t> ImaAdpcmDecoder::Decode(std::span<const std::uint8_t> data) {
    std::vector<std::int16_t> pcm;
    pcm.reserve(data.size() * 2);
    for (const std::uint8_t byte : data) {
        // 每字节高半字节优先。
        for (const int shift : {4, 0}) {
            const int nibble = (byte >> shift) & 0x0F;
            const int step = kStepTable[step_index_];
            int diff = step >> 3;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 4) diff += step;
            int predictor = predictor_;
            predictor = (nibble & 8) ? predictor - diff : predictor + diff;
            predictor = std::clamp(predictor, -32768, 32767);
            step_index_ = std::clamp(step_index_ + kIndexTable[nibble & 7], 0, 88);
            predictor_ = static_cast<std::int16_t>(predictor);
            pcm.push_back(predictor_);
        }
    }
    return pcm;
}

} // namespace voicestick
