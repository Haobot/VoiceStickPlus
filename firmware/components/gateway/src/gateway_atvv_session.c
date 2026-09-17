// gateway_atvv_session.c — ATVV 会话状态机实现（纯逻辑，无 ESP-IDF 依赖）
//
// 桌面端 xiaomi_atvv_session.cc 的协议层 C 化裁剪版，裁剪范围与保留事实见
// gateway_atvv_session.h 头注释。交互模式分支（hold/click/双击/wechat）由
// main.c 主键状态机接管，本模块只输出 PRESS_DOWN/PRESS_UP 沿。
#include "gateway_atvv_session.h"

#include <string.h>

// Control 特征 opcode（遥控器 → 固件 notify 首字节）。
#define CTRL_STOP 0x00
#define CTRL_STREAM_START 0x04
#define CTRL_MIC_OPEN 0x08
#define CTRL_AUDIO_SYNC 0x0A
#define CTRL_CAPS 0x0B

// codec 位掩码（CAPS 应答）。
#define CODEC_8KHZ 0x01
#define CODEC_16KHZ 0x02

// ---- 动作产出辅助（容量不足时丢弃，PCM 帧计入 dropped_pcm_frames） ----

static gateway_atvv_action_t *push_action(gateway_atvv_action_t *actions, size_t max,
                                          size_t *count, gateway_atvv_action_kind_t kind) {
    if (*count >= max) {
        return NULL;
    }
    gateway_atvv_action_t *a = &actions[(*count)++];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    return a;
}

// ---- CAPS 解析（对齐桌面端 XiaomiAtvvProtocol::ParseCaps） ----

typedef struct {
    uint16_t version;   // BE16，0x0100 即 v1.0
    uint8_t codecs;
    size_t frame_bytes;
} caps_info_t;

static bool parse_caps(const uint8_t *data, size_t len, caps_info_t *out) {
    if (len < 3 || data[0] != CTRL_CAPS) {
        return false;
    }
    out->version = (uint16_t)((data[1] << 8) | data[2]);
    out->frame_bytes = 0;
    if (out->version >= 0x0100) {
        if (len < 5) {
            return false;
        }
        out->codecs = data[3];
        bool legacy_fallback = false;
        // 兼容「报 v1 但 codecs==0 且 [4]&0x03≠0」的旧版双字节 codec 布局：
        // 帧长字段不读（旧布局未定义），保持默认。
        if (out->codecs == 0 && len >= 9 && (data[4] & 0x03) != 0) {
            out->codecs = data[4];
            legacy_fallback = true;
        }
        if (!legacy_fallback && len >= 7) {
            out->frame_bytes = ((size_t)data[5] << 8) | data[6];
        }
    } else {
        if (len < 9) {
            return false;
        }
        out->codecs = data[4];
    }
    return true;
}

// ---- 解码链硬重置（流开始/重启；蓝本 HardResetStream） ----

static void hard_reset_stream(gateway_atvv_session_t *s) {
    gateway_adpcm_reset(&s->adpcm, 0, 0);
    gateway_frame_accumulator_reset(&s->acc);
    gateway_pcm_slicer_reset(&s->slicer);
}

// ---- 按下/松开处理 ----

static void begin_press(gateway_atvv_session_t *s, int64_t now_ms,
                        gateway_atvv_action_t *actions, size_t max, size_t *count,
                        bool send_ack) {
    (void)now_ms;
    if (send_ack) {
        gateway_atvv_action_t *tx =
            push_action(actions, max, count, GATEWAY_ATVV_ACTION_WRITE_TX);
        if (tx != NULL) {
            // v≥1.0：0C 00；旧版补所选 codec（只接受 16kHz）
            tx->tx[0] = 0x0C;
            tx->tx[1] = 0x00;
            tx->tx_len = 2;
            if (s->legacy_layout) {
                tx->tx[2] = CODEC_16KHZ;
                tx->tx_len = 3;
            }
        }
    }
    s->mic_open_remote = true;
    hard_reset_stream(s);
    if (push_action(actions, max, count, GATEWAY_ATVV_ACTION_PRESS_DOWN) != NULL) {
        // PRESS_DOWN 输出后由调用方接管交互语义
    }
    s->state = GATEWAY_ATVV_STATE_STREAMING;
}

// MIC_OPEN(0x08)/STREAM_START(0x04) 合一的「按下」入口（蓝本 HandlePressFrame 裁剪版）。
// 返回 true 表示已接受按下。
static bool handle_press_frame(gateway_atvv_session_t *s, int64_t now_ms,
                               gateway_atvv_action_t *actions, size_t max, size_t *count,
                               bool send_ack) {
    if ((s->state == GATEWAY_ATVV_STATE_STREAMING ||
         s->state == GATEWAY_ATVV_STATE_DRAINING ||
         s->state == GATEWAY_ATVV_STATE_READY) &&
        now_ms < s->reject_reopen_until_ms) {
        // STOP 后重开拒绝窗：不回 ACK、不发沿、不开新会话
        return false;
    }
    if (s->state != GATEWAY_ATVV_STATE_READY && s->state != GATEWAY_ATVV_STATE_DRAINING &&
        s->state != GATEWAY_ATVV_STATE_STREAMING) {
        return false;  // IDLE/CAPS/ERROR：握手未完成
    }
    // STREAMING 中重开（RC003 会话内 0x04 重启）：不开新沿，硬重置流继续
    if (s->state == GATEWAY_ATVV_STATE_STREAMING) {
        hard_reset_stream(s);
        return true;
    }
    // DRAINING 拒绝窗外重开：直接开新按下（尾包宽限余量随硬重置丢弃）
    begin_press(s, now_ms, actions, max, count, send_ack);
    return true;
}

// ---- 收尾（蓝本 FinalizeStream 裁剪版：余量补零出末帧） ----

static void finalize_stream(gateway_atvv_session_t *s,
                            gateway_atvv_action_t *actions, size_t max, size_t *count) {
    int16_t remainder[GATEWAY_PCM_SLICER_FRAME_SAMPLES];
    size_t n = gateway_pcm_slicer_take_remainder(&s->slicer, remainder,
                                                 GATEWAY_PCM_SLICER_FRAME_SAMPLES);
    if (n > 0) {
        memset(remainder + n, 0, (GATEWAY_PCM_SLICER_FRAME_SAMPLES - n) * sizeof(int16_t));
        gateway_atvv_action_t *pcm =
            push_action(actions, max, count, GATEWAY_ATVV_ACTION_PCM_FRAME);
        if (pcm != NULL) {
            memcpy(s->pcm_frame, remainder, sizeof(remainder));
            pcm->pcm = s->pcm_frame;
            pcm->pcm_len = GATEWAY_PCM_SLICER_FRAME_SAMPLES;
        } else {
            s->dropped_pcm_frames++;
        }
    }
    gateway_frame_accumulator_reset(&s->acc);
    s->state = GATEWAY_ATVV_STATE_READY;
}

// ---- 音频帧处理：累积切帧 → 解码 → 切片 → PCM 动作 ----

static void process_audio_bytes(gateway_atvv_session_t *s, const uint8_t *data, size_t len,
                                gateway_atvv_action_t *actions, size_t max, size_t *count) {
    uint8_t frames[GATEWAY_ADPCM_MAX_FRAME_BYTES * 2];
    size_t fn = gateway_frame_accumulator_append(&s->acc, data, len, frames, sizeof(frames));
    for (size_t f = 0; f < fn; f++) {
        int16_t pcm[GATEWAY_ADPCM_MAX_FRAME_BYTES * 2];
        size_t samples = gateway_adpcm_decode(
            &s->adpcm, frames + f * s->acc.frame_bytes, s->acc.frame_bytes,
            pcm, sizeof(pcm) / sizeof(pcm[0]));
        // 切片器输出直接进 session 单帧缓冲（一次调用至多 1 帧，数学封顶见头注释）
        size_t sliced = gateway_pcm_slicer_append(
            &s->slicer, pcm, samples, s->pcm_frame, GATEWAY_PCM_SLICER_FRAME_SAMPLES);
        for (size_t k = 0; k < sliced; k++) {
            gateway_atvv_action_t *act =
                push_action(actions, max, count, GATEWAY_ATVV_ACTION_PCM_FRAME);
            if (act != NULL) {
                act->pcm = s->pcm_frame;
                act->pcm_len = GATEWAY_PCM_SLICER_FRAME_SAMPLES;
            } else {
                s->dropped_pcm_frames++;
            }
        }
    }
}

// ---- 对外接口 ----

void gateway_atvv_session_reset(gateway_atvv_session_t *s) {
    memset(s, 0, sizeof(*s));
    gateway_frame_accumulator_init(&s->acc);
    gateway_pcm_slicer_init(&s->slicer);
}

size_t gateway_atvv_session_start(gateway_atvv_session_t *s, int64_t now_ms,
                                  gateway_atvv_action_t *actions, size_t max_actions) {
    if (s->state != GATEWAY_ATVV_STATE_IDLE) {
        return 0;
    }
    s->caps_requested_at_ms = now_ms;
    s->state = GATEWAY_ATVV_STATE_CAPS_REQUESTED;
    size_t count = 0;
    gateway_atvv_action_t *tx = push_action(actions, max_actions, &count,
                                            GATEWAY_ATVV_ACTION_WRITE_TX);
    if (tx != NULL) {
        static const uint8_t get_caps[6] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
        memcpy(tx->tx, get_caps, sizeof(get_caps));
        tx->tx_len = sizeof(get_caps);
    }
    return count;
}

size_t gateway_atvv_session_stop(gateway_atvv_session_t *s,
                                 gateway_atvv_action_t *actions, size_t max_actions) {
    size_t count = 0;
    if (s->mic_open_remote) {
        gateway_atvv_action_t *tx = push_action(actions, max_actions, &count,
                                                GATEWAY_ATVV_ACTION_WRITE_TX);
        if (tx != NULL) {
            // v≥1.0：0D <sessionID>；旧版仅 0D
            tx->tx[0] = 0x0D;
            tx->tx_len = 1;
            if (!s->legacy_layout) {
                tx->tx[1] = s->remote_session_id;
                tx->tx_len = 2;
            }
        }
    }
    s->mic_open_remote = false;
    s->state = GATEWAY_ATVV_STATE_IDLE;
    hard_reset_stream(s);
    return count;
}

size_t gateway_atvv_session_control(gateway_atvv_session_t *s, const uint8_t *data,
                                    size_t len, int64_t now_ms,
                                    gateway_atvv_action_t *actions, size_t max_actions) {
    size_t count = 0;
    if (len == 0 || s->state == GATEWAY_ATVV_STATE_ERROR) {
        return 0;
    }
    switch (data[0]) {
        case CTRL_CAPS: {
            if (s->state != GATEWAY_ATVV_STATE_CAPS_REQUESTED &&
                s->state != GATEWAY_ATVV_STATE_READY) {
                break;
            }
            caps_info_t caps;
            if (!parse_caps(data, len, &caps)) {
                break;  // 坏包忽略，由 CAPS 超时兜底
            }
            if ((caps.codecs & CODEC_16KHZ) == 0) {
                s->state = GATEWAY_ATVV_STATE_ERROR;
                gateway_atvv_action_t *err =
                    push_action(actions, max_actions, &count, GATEWAY_ATVV_ACTION_ERROR);
                if (err != NULL) {
                    err->error_code = "unsupported_codec";
                }
                break;
            }
            s->legacy_layout = caps.version < 0x0100;
            gateway_frame_accumulator_set_frame_bytes(&s->acc, caps.frame_bytes);
            s->frame_bytes = s->acc.frame_bytes;
            s->state = GATEWAY_ATVV_STATE_READY;
            break;
        }
        case CTRL_MIC_OPEN: {
            handle_press_frame(s, now_ms, actions, max_actions, &count, /*send_ack=*/true);
            break;
        }
        case CTRL_STREAM_START: {
            if (s->state == GATEWAY_ATVV_STATE_STREAMING) {
                // 会话内流重启：硬重置（编码器从 0/0 重启但可能不发 SYNC）
                hard_reset_stream(s);
                if (len >= 4) {
                    s->remote_session_id = data[3];
                }
                break;
            }
            if (s->state == GATEWAY_ATVV_STATE_DRAINING || s->state == GATEWAY_ATVV_STATE_READY) {
                // 2 Pro：0x04 即「按下+开流」一体帧；byte2=codec 存在时必须 16kHz
                if (len >= 3 && data[2] != CODEC_16KHZ) {
                    s->state = GATEWAY_ATVV_STATE_ERROR;
                    gateway_atvv_action_t *err =
                        push_action(actions, max_actions, &count, GATEWAY_ATVV_ACTION_ERROR);
                    if (err != NULL) {
                        err->error_code = "unsupported_codec";
                    }
                    break;
                }
                if (handle_press_frame(s, now_ms, actions, max_actions, &count,
                                       /*send_ack=*/false)) {
                    if (len >= 4) {
                        s->remote_session_id = data[3];
                    }
                }
            }
            break;  // IDLE/CAPS：握手未完成忽略
        }
        case CTRL_AUDIO_SYNC: {
            if (len < 7) {
                break;
            }
            if (s->state != GATEWAY_ATVV_STATE_STREAMING &&
                s->state != GATEWAY_ATVV_STATE_DRAINING) {
                break;
            }
            int16_t predictor = (int16_t)((data[4] << 8) | data[5]);
            gateway_adpcm_reset(&s->adpcm, predictor, data[6]);
            gateway_frame_accumulator_reset(&s->acc);
            gateway_pcm_slicer_reset(&s->slicer);
            break;
        }
        case CTRL_STOP: {
            s->mic_open_remote = false;
            s->stop_received_at_ms = now_ms;
            if (s->state == GATEWAY_ATVV_STATE_STREAMING) {
                if (push_action(actions, max_actions, &count,
                                GATEWAY_ATVV_ACTION_PRESS_UP) != NULL) {
                    // 尾包 150ms 宽限后才收尾（Tick 驱动）
                }
                s->state = GATEWAY_ATVV_STATE_DRAINING;
                s->reject_reopen_until_ms = now_ms + GATEWAY_ATVV_REOPEN_REJECT_MS;
            } else if (s->state == GATEWAY_ATVV_STATE_DRAINING) {
                // 宽限期内重复 STOP：刷新宽限与拒绝窗起点
                s->reject_reopen_until_ms = now_ms + GATEWAY_ATVV_REOPEN_REJECT_MS;
            }
            break;
        }
        default:
            break;
    }
    return count;
}

size_t gateway_atvv_session_audio(gateway_atvv_session_t *s, const uint8_t *data,
                                  size_t len, int64_t now_ms,
                                  gateway_atvv_action_t *actions, size_t max_actions) {
    if (s->state != GATEWAY_ATVV_STATE_STREAMING &&
        s->state != GATEWAY_ATVV_STATE_DRAINING) {
        return 0;
    }
    // 超宽限的尾包丢弃
    if (s->state == GATEWAY_ATVV_STATE_DRAINING &&
        now_ms - s->stop_received_at_ms > GATEWAY_ATVV_AUDIO_TAIL_GRACE_MS) {
        return 0;
    }
    size_t count = 0;
    process_audio_bytes(s, data, len, actions, max_actions, &count);
    return count;
}

size_t gateway_atvv_session_tick(gateway_atvv_session_t *s, int64_t now_ms,
                                 gateway_atvv_action_t *actions, size_t max_actions) {
    size_t count = 0;
    switch (s->state) {
        case GATEWAY_ATVV_STATE_CAPS_REQUESTED:
            if (now_ms - s->caps_requested_at_ms >= GATEWAY_ATVV_CAPS_TIMEOUT_MS) {
                s->state = GATEWAY_ATVV_STATE_ERROR;
                gateway_atvv_action_t *err =
                    push_action(actions, max_actions, &count, GATEWAY_ATVV_ACTION_ERROR);
                if (err != NULL) {
                    err->error_code = "caps_timeout";
                }
            }
            break;
        case GATEWAY_ATVV_STATE_DRAINING:
            if (now_ms - s->stop_received_at_ms >= GATEWAY_ATVV_AUDIO_TAIL_GRACE_MS) {
                finalize_stream(s, actions, max_actions, &count);
            }
            break;
        default:
            break;
    }
    return count;
}
