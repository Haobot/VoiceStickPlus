// gateway_adpcm.c — ATVV 音频归一化实现（纯逻辑，无 ESP-IDF 依赖）
//
// 算法与桌面端 C++ 实现逐字节对齐（互为金标准，host 单测比对）：
// desktop/windows/src/ima_adpcm_decoder.cc（解码器）、pcm_postprocessor.h
// （FrameAccumulator）、audio_opus_encoder.h（OpusFrameSlicer）。
#include "gateway_adpcm.h"

#include <string.h>

// IMA/DVI ADPCM 公开标准（1992）常数表。
static const int k_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int k_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void gateway_adpcm_reset(gateway_adpcm_state_t *st, int16_t predictor, int step_index) {
    st->predictor = predictor;
    st->step_index = clamp_int(step_index, 0, 88);
}

size_t gateway_adpcm_decode(gateway_adpcm_state_t *st, const uint8_t *data, size_t len,
                            int16_t *out, size_t out_cap) {
    size_t produced = 0;
    for (size_t i = 0; i < len; i++) {
        // 每字节高半字节优先
        for (int shift = 4; shift >= 0; shift -= 4) {
            if (produced >= out_cap) {
                return produced;  // 容量截断：剩余输入丢弃（调用方按 len*2 预备）
            }
            const int nibble = (data[i] >> shift) & 0x0F;
            const int step = k_step_table[st->step_index];
            int diff = step >> 3;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 4) diff += step;
            int predictor = st->predictor;
            predictor = (nibble & 8) ? predictor - diff : predictor + diff;
            predictor = clamp_int(predictor, -32768, 32767);
            st->step_index =
                clamp_int(st->step_index + k_index_table[nibble & 7], 0, 88);
            st->predictor = (int16_t)predictor;
            out[produced++] = (int16_t)predictor;
        }
    }
    return produced;
}

// ---- 帧累积器 ----

void gateway_frame_accumulator_init(gateway_frame_accumulator_t *acc) {
    acc->frame_bytes = GATEWAY_ADPCM_DEFAULT_FRAME_BYTES;
    acc->pending = 0;
}

void gateway_frame_accumulator_set_frame_bytes(gateway_frame_accumulator_t *acc,
                                               size_t frame_bytes) {
    if (frame_bytes == 0) {
        frame_bytes = GATEWAY_ADPCM_DEFAULT_FRAME_BYTES;
    } else if (frame_bytes > GATEWAY_ADPCM_MAX_FRAME_BYTES) {
        frame_bytes = GATEWAY_ADPCM_MAX_FRAME_BYTES;
    }
    acc->frame_bytes = frame_bytes;
    acc->pending = 0;  // 协商即重置：半帧残量对新帧长无意义
}

void gateway_frame_accumulator_reset(gateway_frame_accumulator_t *acc) {
    acc->pending = 0;
}

size_t gateway_frame_accumulator_pending(const gateway_frame_accumulator_t *acc) {
    return acc->pending;
}

size_t gateway_frame_accumulator_append(gateway_frame_accumulator_t *acc,
                                        const uint8_t *data, size_t len,
                                        uint8_t *out, size_t out_cap_bytes) {
    size_t frames = 0;
    // 上次容量不足滞留的完整帧先尝试吐出
    if (acc->pending == acc->frame_bytes) {
        if (acc->frame_bytes <= out_cap_bytes) {
            memcpy(out, acc->buf, acc->frame_bytes);
            frames = 1;
            acc->pending = 0;
        } else {
            return 0;  // 连一帧都容不下：滞留帧保留，本次输入丢弃
        }
    }
    for (size_t i = 0; i < len; i++) {
        acc->buf[acc->pending++] = data[i];
        if (acc->pending == acc->frame_bytes) {
            if ((frames + 1) * acc->frame_bytes > out_cap_bytes) {
                break;  // 容量不足：完整帧滞留暂存，本次后续输入丢弃（不切半帧）
            }
            memcpy(out + frames * acc->frame_bytes, acc->buf, acc->frame_bytes);
            frames++;
            acc->pending = 0;
        }
    }
    return frames;
}

// ---- PCM 切片器 ----

void gateway_pcm_slicer_init(gateway_pcm_slicer_t *slicer) {
    slicer->pending = 0;
}

void gateway_pcm_slicer_reset(gateway_pcm_slicer_t *slicer) {
    slicer->pending = 0;
}

size_t gateway_pcm_slicer_pending(const gateway_pcm_slicer_t *slicer) {
    return slicer->pending;
}

size_t gateway_pcm_slicer_append(gateway_pcm_slicer_t *slicer,
                                 const int16_t *pcm, size_t n,
                                 int16_t *out, size_t out_cap_samples) {
    size_t frames = 0;
    if (slicer->pending == GATEWAY_PCM_SLICER_FRAME_SAMPLES) {
        if (GATEWAY_PCM_SLICER_FRAME_SAMPLES <= out_cap_samples) {
            memcpy(out, slicer->buf, GATEWAY_PCM_SLICER_FRAME_SAMPLES * sizeof(int16_t));
            frames = 1;
            slicer->pending = 0;
        } else {
            return 0;
        }
    }
    for (size_t i = 0; i < n; i++) {
        slicer->buf[slicer->pending++] = pcm[i];
        if (slicer->pending == GATEWAY_PCM_SLICER_FRAME_SAMPLES) {
            if ((frames + 1) * GATEWAY_PCM_SLICER_FRAME_SAMPLES > out_cap_samples) {
                break;  // 完整帧滞留，本次后续输入丢弃
            }
            memcpy(out + frames * GATEWAY_PCM_SLICER_FRAME_SAMPLES, slicer->buf,
                   GATEWAY_PCM_SLICER_FRAME_SAMPLES * sizeof(int16_t));
            frames++;
            slicer->pending = 0;
        }
    }
    return frames;
}

size_t gateway_pcm_slicer_take_remainder(gateway_pcm_slicer_t *slicer,
                                         int16_t *out, size_t out_cap_samples) {
    size_t n = slicer->pending;
    if (n > out_cap_samples) {
        n = out_cap_samples;
    }
    memcpy(out, slicer->buf, n * sizeof(int16_t));
    slicer->pending = 0;
    return n;
}
