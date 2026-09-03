/* VoiceStick Opus shim：opus_encoder_ctl/opus_decoder_ctl 是可变参数函数，
 * Swift 无法直接调用，这里包一层非可变参数接口，只暴露 opaque 指针。
 * 编码参数与固件 audio_pipeline / Windows audio_opus_encoder 对齐。 */
#ifndef VOICESTICK_OPUS_SHIM_H
#define VOICESTICK_OPUS_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建编码器：16000 Hz / 1 ch / OPUS_APPLICATION_VOIP。
 * 成功返回非 NULL 且 *errorOut == OPUS_OK(0)；失败返回 NULL。 */
void* vs_opus_encoder_create_16k(int* errorOut);

/* 编码一帧 PCM。返回编码字节数（>0），负值为 Opus 错误码。 */
int vs_opus_encode(void* st, const int16_t* pcm, int frame_size,
                   uint8_t* out, int32_t max_bytes);

/* 以下 setter 返回 OPUS_OK(0) 或负值错误码。 */
int vs_opus_set_bitrate(void* st, int32_t bitrate);
int vs_opus_set_vbr(void* st, int vbr);
int vs_opus_set_complexity(void* st, int complexity);
int vs_opus_set_signal_voice(void* st);
int vs_opus_set_dtx(void* st, int dtx);

/* 重置编码器内部状态（切换会话时）。 */
int vs_opus_reset_state(void* st);

void vs_opus_encoder_destroy(void* st);

/* 解码侧：供单测 round-trip 使用。 */
void* vs_opus_decoder_create_16k(int* errorOut);
int vs_opus_decode(void* st, const uint8_t* data, int32_t len,
                   int16_t* out, int frame_size, int decode_fec);
void vs_opus_decoder_destroy(void* st);

#ifdef __cplusplus
}
#endif

#endif /* VOICESTICK_OPUS_SHIM_H */
