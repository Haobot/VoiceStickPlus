#include "voicestick_opus_shim.h"

#include "opus.h"

#define VS_OPUS_SAMPLE_RATE 16000
#define VS_OPUS_CHANNELS 1

void* vs_opus_encoder_create_16k(int* errorOut) {
    int error = OPUS_OK;
    OpusEncoder* st = opus_encoder_create(VS_OPUS_SAMPLE_RATE, VS_OPUS_CHANNELS,
                                          OPUS_APPLICATION_VOIP, &error);
    if (errorOut) *errorOut = error;
    if (error != OPUS_OK) return NULL;
    return st;
}

int vs_opus_encode(void* st, const int16_t* pcm, int frame_size,
                   uint8_t* out, int32_t max_bytes) {
    if (st == NULL || pcm == NULL || out == NULL) return OPUS_BAD_ARG;
    return opus_encode((OpusEncoder*)st, (const opus_int16*)pcm, frame_size,
                       (unsigned char*)out, (opus_int32)max_bytes);
}

int vs_opus_set_bitrate(void* st, int32_t bitrate) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_SET_BITRATE((opus_int32)bitrate));
}

int vs_opus_set_vbr(void* st, int vbr) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_SET_VBR(vbr));
}

int vs_opus_set_complexity(void* st, int complexity) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_SET_COMPLEXITY(complexity));
}

int vs_opus_set_signal_voice(void* st) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

int vs_opus_set_dtx(void* st, int dtx) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_SET_DTX(dtx));
}

int vs_opus_reset_state(void* st) {
    if (st == NULL) return OPUS_BAD_ARG;
    return opus_encoder_ctl((OpusEncoder*)st, OPUS_RESET_STATE);
}

void vs_opus_encoder_destroy(void* st) {
    if (st == NULL) return;
    opus_encoder_destroy((OpusEncoder*)st);
}

void* vs_opus_decoder_create_16k(int* errorOut) {
    int error = OPUS_OK;
    OpusDecoder* st = opus_decoder_create(VS_OPUS_SAMPLE_RATE, VS_OPUS_CHANNELS, &error);
    if (errorOut) *errorOut = error;
    if (error != OPUS_OK) return NULL;
    return st;
}

int vs_opus_decode(void* st, const uint8_t* data, int32_t len,
                   int16_t* out, int frame_size, int decode_fec) {
    if (st == NULL || out == NULL) return OPUS_BAD_ARG;
    return opus_decode((OpusDecoder*)st, (const unsigned char*)data, (opus_int32)len,
                       (opus_int16*)out, frame_size, decode_fec);
}

void vs_opus_decoder_destroy(void* st) {
    if (st == NULL) return;
    opus_decoder_destroy((OpusDecoder*)st);
}
