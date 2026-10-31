// gateway_adpcm.h — ATVV 音频归一化纯逻辑件（无 ESP-IDF 依赖，host 可测）
//
// 对应桌面端 C++ 三件套（IMA ADPCM 解码器 / FrameAccumulator / OpusFrameSlicer），
// 算法逐字节对齐互为金标准：Doc/Plan/xiaomi-remote-stick-gateway.md §5.4.1。
// 桌面端蓝本：desktop/windows/src/{ima_adpcm_decoder.cc, pcm_postprocessor.h,
// audio_opus_encoder.h}。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- IMA/DVI ADPCM 解码器（1992 公开标准） ----
// 4 bit/采样，每字节高半字节优先；predictor/step 跨包连续推进，丢包即漂移，
// 直到 Reset（流开始硬重置或 AUDIO_SYNC 按值重置）。
typedef struct {
    int16_t predictor;
    int step_index;  // 钳位 [0, 88]
} gateway_adpcm_state_t;

void gateway_adpcm_reset(gateway_adpcm_state_t *st, int16_t predictor, int step_index);

// 解码一段 ADPCM 字节流：每字节产 2 个样本写入 out。
// 返回产出样本数（len*2）；out 容量不足时截断（正常调用方按 len*2 预备）。
size_t gateway_adpcm_decode(gateway_adpcm_state_t *st, const uint8_t *data, size_t len,
                            int16_t *out, size_t out_cap);

// ---- ADPCM 裸字节流帧累积器 ----
// ATVV audio notify 无帧头无序号，按 CAPS 协商帧长（默认 120 字节）累积，
// 攒满一帧吐一帧。帧长协商值防御钳位 [1, 上限]。
#define GATEWAY_ADPCM_DEFAULT_FRAME_BYTES 120
#define GATEWAY_ADPCM_MAX_FRAME_BYTES 256

typedef struct {
    size_t frame_bytes;
    uint8_t buf[GATEWAY_ADPCM_MAX_FRAME_BYTES];
    size_t pending;
} gateway_frame_accumulator_t;

void gateway_frame_accumulator_init(gateway_frame_accumulator_t *acc);
// CAPS 到达时调整协商帧长并清空已累积字节；0 用默认值，超上限钳位。
void gateway_frame_accumulator_set_frame_bytes(gateway_frame_accumulator_t *acc,
                                               size_t frame_bytes);
void gateway_frame_accumulator_reset(gateway_frame_accumulator_t *acc);
size_t gateway_frame_accumulator_pending(const gateway_frame_accumulator_t *acc);

// 追加字节，凑满的完整帧平铺写入 out（每帧 frame_bytes 长）。
// 返回本次凑满的完整帧数；out 容量不足时完整帧滞留暂存（不切半帧）、
// 本次后续输入丢弃，下次调用容量足够时先吐滞留帧。
size_t gateway_frame_accumulator_append(gateway_frame_accumulator_t *acc,
                                        const uint8_t *data, size_t len,
                                        uint8_t *out, size_t out_cap_bytes);

// ---- PCM 组帧器 ----
// 把任意长度 16 kHz 单声道 PCM 累积切成 40ms（640 采样）帧，与固件
// audio_pipeline AUDIO_FRAME_MS=40 对齐；攒满即吐，余量跨调用保留。
#define GATEWAY_PCM_SLICER_FRAME_SAMPLES 640

typedef struct {
    int16_t buf[GATEWAY_PCM_SLICER_FRAME_SAMPLES];
    size_t pending;
} gateway_pcm_slicer_t;

void gateway_pcm_slicer_init(gateway_pcm_slicer_t *slicer);
void gateway_pcm_slicer_reset(gateway_pcm_slicer_t *slicer);
size_t gateway_pcm_slicer_pending(const gateway_pcm_slicer_t *slicer);

// 追加 PCM，凑满的完整帧平铺写入 out。返回完整帧数；容量不足时完整帧
// 滞留暂存、本次后续输入丢弃（语义同帧累积器）。
size_t gateway_pcm_slicer_append(gateway_pcm_slicer_t *slicer,
                                 const int16_t *pcm, size_t n,
                                 int16_t *out, size_t out_cap_samples);

// 取出不足一帧的余量（取出后清空，会话收尾补零出末帧用）。返回样本数。
size_t gateway_pcm_slicer_take_remainder(gateway_pcm_slicer_t *slicer,
                                         int16_t *out, size_t out_cap_samples);

#ifdef __cplusplus
}
#endif
