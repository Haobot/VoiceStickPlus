// gateway_atvv_session.h — 小米遥控器 ATVV 会话状态机（纯逻辑，host 可测）
//
// 桌面端直连实现 xiaomi_atvv_session.cc 的固件 C 化裁剪版（方案
// Doc/Plan/xiaomi-remote-stick-gateway.md §5.4.1）：只保留协议层会话管理，
// 裁掉交互模式分支（hold/click/双击窗/wechat toggle——由固件 main.c 现有
// 主键状态机接管 APP_INPUT_SOURCE_XIAOMI 源）与 Opus 编码（audio_pipeline 统一）。
//
// 保留的协议事实（蓝本 xiaomi_atvv_session.cc + xiaomi_atvv_protocol.cc）：
// - 连接建立后主机写 TX GET_CAPS(0A 01 00 00 03 03)，2s 超时报 caps_timeout；
// - CAPS 只接受 16kHz（0x02 位）；版本 < v1.0 为 legacy 布局（TX 命令格式不同）；
// - 语音键按下两径：RC003 MIC_OPEN(0x08) 须回 ACK(0x0C)，2 Pro 直接 0x04
//   一体帧（按下+开流，无 ACK；byte2 存在时必须为 16kHz codec）；
// - 0x04 到达一律硬重置解码链（RC003 会话重启可能不发 SYNC，否则二次按键
//   DC 饱和）；AUDIO_SYNC(0x0a) 按 predictor/step 值重置；
// - STOP(0x00) 后 150ms 内的音频尾包仍须接收（Audio/Control 双特征异步）；
// - 经过 STREAMING 的键程 STOP 后 300ms 内拒绝重开（防抖动，不回 ACK）；
// - 断开时若遥控器侧 mic 未关，尽力写 MIC_CLOSE(0xD)。
//
// 输出动作语义：
// - PRESS_DOWN/PRESS_UP → 调用方注入主键按下/松开沿（固件交互状态机接管）；
// - PCM_FRAME → 40ms PCM 帧，喂 audio_pipeline 外部音频源；
// - WRITE_TX → 调用方写小米 ATVV TX 特征；
// 线程契约：所有入口须单线程串行调用（NimBLE host 任务），无内部锁。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway_adpcm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_ATVV_TX_MAX_BYTES 8
#define GATEWAY_ATVV_MAX_ACTIONS 4

// 时序常量（毫秒，对齐桌面端 xiaomi_atvv_session.h）：
// 尾包宽限 150ms（协议 §3.2）；CAPS 超时 2s；重开拒绝窗 300ms。
#define GATEWAY_ATVV_AUDIO_TAIL_GRACE_MS 150
#define GATEWAY_ATVV_CAPS_TIMEOUT_MS 2000
#define GATEWAY_ATVV_REOPEN_REJECT_MS 300

typedef enum {
    GATEWAY_ATVV_STATE_IDLE = 0,
    GATEWAY_ATVV_STATE_CAPS_REQUESTED,
    GATEWAY_ATVV_STATE_READY,
    GATEWAY_ATVV_STATE_STREAMING,   // 按下沿已输出，音频流出中
    GATEWAY_ATVV_STATE_DRAINING,    // STOP 已收，尾包宽限内
    GATEWAY_ATVV_STATE_ERROR,
} gateway_atvv_state_t;

typedef enum {
    GATEWAY_ATVV_ACTION_WRITE_TX = 0,
    GATEWAY_ATVV_ACTION_PRESS_DOWN,
    GATEWAY_ATVV_ACTION_PRESS_UP,
    GATEWAY_ATVV_ACTION_PCM_FRAME,
    GATEWAY_ATVV_ACTION_ERROR,
} gateway_atvv_action_kind_t;

typedef struct {
    gateway_atvv_action_kind_t kind;
    // WRITE_TX
    uint8_t tx[GATEWAY_ATVV_TX_MAX_BYTES];
    size_t tx_len;
    // PCM_FRAME：指向 session 内部单帧缓冲。一次调用至多产 1 个 PCM 动作
    // （单包音频 ≤514B → ≤1028 样本 < 2×640），仅在本批动作处理期间有效。
    const int16_t *pcm;
    size_t pcm_len;
    // ERROR：静态字符串 "caps_timeout" / "unsupported_codec"
    const char *error_code;
} gateway_atvv_action_t;

typedef struct {
    gateway_atvv_state_t state;
    bool legacy_layout;      // CAPS 版本 < v1.0，影响 TX 命令格式
    size_t frame_bytes;      // CAPS 协商 ADPCM 帧长
    gateway_adpcm_state_t adpcm;
    gateway_frame_accumulator_t acc;
    gateway_pcm_slicer_t slicer;
    int64_t caps_requested_at_ms;
    int64_t stop_received_at_ms;
    int64_t reject_reopen_until_ms;  // 长按键程 STOP 后的重开拒绝窗截止
    bool mic_open_remote;            // 遥控器侧 mic 打开未 STOP，断开需 MIC_CLOSE
    uint8_t remote_session_id;       // 0x04 byte3（可选），MIC_CLOSE 透传
    // PCM 帧输出缓冲（单帧，动作 pcm 指针指向这里）
    int16_t pcm_frame[GATEWAY_PCM_SLICER_FRAME_SAMPLES];
    // 调试计数：动作槽满被丢的 PCM 帧数（正常恒为 0）
    uint32_t dropped_pcm_frames;
} gateway_atvv_session_t;

// 复位到 IDLE（全状态清零，不清 dropped_pcm_frames 统计语义——统计随生命周期累计）。
void gateway_atvv_session_reset(gateway_atvv_session_t *s);

// 连接建立后调用：进入 CAPS_REQUESTED 并产出 GET_CAPS。仅 IDLE 可启动，
// 其余状态返回 0（重复调用无害）。
size_t gateway_atvv_session_start(gateway_atvv_session_t *s, int64_t now_ms,
                                  gateway_atvv_action_t *actions, size_t max_actions);

// 断开前调用：mic 未关则产出 MIC_CLOSE，复位到 IDLE。
size_t gateway_atvv_session_stop(gateway_atvv_session_t *s,
                                 gateway_atvv_action_t *actions, size_t max_actions);

// 喂 Control 特征 notify。返回产出动作数。
size_t gateway_atvv_session_control(gateway_atvv_session_t *s, const uint8_t *data,
                                    size_t len, int64_t now_ms,
                                    gateway_atvv_action_t *actions, size_t max_actions);

// 喂 Audio 特征 notify（STREAMING/DRAINING 有效；DRAINING 超宽限丢弃）。
size_t gateway_atvv_session_audio(gateway_atvv_session_t *s, const uint8_t *data,
                                  size_t len, int64_t now_ms,
                                  gateway_atvv_action_t *actions, size_t max_actions);

// 周期调用：CAPS 超时、尾包宽限到期收尾（余量补零出末帧）。
size_t gateway_atvv_session_tick(gateway_atvv_session_t *s, int64_t now_ms,
                                 gateway_atvv_action_t *actions, size_t max_actions);

#ifdef __cplusplus
}
#endif
