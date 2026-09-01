#pragma once

#include "app_config.h" // InteractionMode
#include "audio_opus_encoder.h"
#include "ble_protocol.h" // AudioFrame / StateEvent
#include "ima_adpcm_decoder.h"
#include "pcm_postprocessor.h"
#include "xiaomi_atvv_protocol.h"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace voicestick {

// 会话输出动作（调用方/BLE 层消费）：写 TX 字节 / 合成 StateEvent / 合成 AudioFrame / 错误。
struct XiaomiAtvvWriteTx {
    ByteVector bytes;
};
struct XiaomiAtvvStateEvent {
    StateEvent event;
};
struct XiaomiAtvvAudioFrame {
    AudioFrame frame;
};
struct XiaomiAtvvError {
    std::string code; // "unsupported_codec" / "caps_timeout"
};
using XiaomiAtvvAction = std::variant<XiaomiAtvvWriteTx, XiaomiAtvvStateEvent,
                                      XiaomiAtvvAudioFrame, XiaomiAtvvError>;

// F5 抑制判定（Windows voice_f5_suppressor 消费）：小米语音键按下（ATVV MIC_OPEN）
// 时遥控器固件会向 OS 多发一个 F5 键；仅「最近 80ms 内有 MIC_OPEN」时吞掉 F5。
// 纯谓词放 core 供单测；时间戳为 steady_clock epoch 毫秒（0 = 从未开麦，不吞）。
inline constexpr std::int64_t kF5SuppressWindowMs = 80;
inline bool ShouldSuppressF5(std::int64_t now_ms, std::int64_t last_mic_open_ms, bool enabled) {
    if (!enabled || last_mic_open_ms <= 0) return false;
    const std::int64_t age_ms = now_ms - last_mic_open_ms;
    return age_ms >= 0 && age_ms <= kF5SuppressWindowMs;
}

enum class XiaomiAtvvSessionState {
    kIdle,
    kCapsRequested,
    kReady,
    kTapPending,     // 短击判定中：MIC_OPEN 已应答，等待 300ms 长按阈值或 STOP
    kStreaming,      // button_down 已发出，音频流出中
    kDraining,       // STOP 已收到，150ms 尾包宽限内
    kWaitSecondTap,  // 短击已释放，双击窗内等第二次按下
    kError,
};

// 小米遥控器 ATVV 会话纯状态机（不碰 WinRT）：输入 Control opcode / Audio 字节 /
// 注入时钟 now_ms，输出动作列表。按键语义镜像固件双击设计
//（Doc/Plan/primary-button-double-click.md）：
// - hold_to_talk：MIC_OPEN 先缓冲音频不发事件；按住 ≥300ms 确认长按发 button_down
//   （缓冲音频不丢）；<300ms 松开为短击，丢弃缓冲，进入双击窗；窗内第二次按下合成
//   button_double_click（不录音）；窗超时补发 button_click（协调器侧为无害 no-op）。
// - click_to_talk：MIC_OPEN 立即发 button_down（协调器按 down/up 处理），第一次点击
//   即已开录、松开即停，双击窗超时不再补发 button_click（否则协调器空闲态会把
//   无 duration_ms 的 click 当启动，产生永远收不到音频的幽灵会话）；仅保留窗内
//   第二击合成 button_double_click。
// - 重开拒绝窗：经过 kStreaming 的键程 STOP 后 300ms 内忽略 MIC_OPEN（不回 ACK、
//   不开新会话）；双击路径走 kWaitSecondTap 不受影响。
// 音频路径：ADPCM 累积切帧 → IMA 解码 → 三点平滑+增益 → Opus（640 采样/帧）→
// AudioFrame（session_id 每录音会话自增，与 button_down 一致；flags bit0 首帧 bit1 末帧）。
// 线程契约：全部公开入口（Start/Stop/HandleControlCommand/HandleAudioData/Tick）须在
// 单一线程串行调用（集成层负责 marshal 到 UI 线程），本类无内部锁。
// 构造函数可抛 std::runtime_error（opus_encoder_create 失败时，来自 AudioOpusEncoder）。
class XiaomiAtvvSession {
public:
    struct Options {
        InteractionMode interaction_mode = InteractionMode::kHoldToTalk;
        double gain_db = 12.0;              // ADPCM 解码后增益（±24dB 限幅）
        int double_click_window_ms = 350;   // [device.<id>.xiaomi] double_click_ms；
                                            // 有意小于固件 500ms 窗（更快确认单击），勿对齐
    };

    // 时序常量：hold 阈值对齐固件 DOUBLE_CLICK_HOLD_THRESHOLD_MS；尾包宽限见协议 §3.2。
    static constexpr std::int64_t kHoldThresholdMs = 300;
    static constexpr std::int64_t kAudioTailGraceMs = 150;
    static constexpr std::int64_t kCapsTimeoutMs = 2000;
    // STOP（长按键程）后拒绝重开会话的窗口时长。
    static constexpr std::int64_t kReopenRejectMs = 300;

    explicit XiaomiAtvvSession(Options options = {});

    XiaomiAtvvSessionState state() const { return state_; }
    // 当前/最近录音会话 id（未发过 button_down 时为 0）。
    std::uint32_t current_session_id() const { return current_session_id_; }
    // 测试观察用：验证 0x04 硬重置与 0x0A 按值重置。
    const ImaAdpcmDecoder& decoder() const { return decoder_; }
    // 测试观察用：验证 SYNC/流开始时帧累积器清空。
    const FrameAccumulator& accumulator() const { return accumulator_; }

    // 连接建立后调用：进入 CapsRequested 并产出写 TX GET_CAPS。仅 kIdle 可启动。
    std::vector<XiaomiAtvvAction> Start(std::int64_t now_ms);
    // 断开前调用：若遥控器侧 mic 仍打开则尽力发 MIC_CLOSE，并复位到 kIdle。
    std::vector<XiaomiAtvvAction> Stop(std::int64_t now_ms);
    std::vector<XiaomiAtvvAction> HandleControlCommand(std::span<const std::uint8_t> data,
                                                       std::int64_t now_ms);
    std::vector<XiaomiAtvvAction> HandleAudioData(std::span<const std::uint8_t> data,
                                                  std::int64_t now_ms);
    // 周期调用：处理长按阈值确认、尾包宽限到期、双击窗到期、CAPS 超时。
    std::vector<XiaomiAtvvAction> Tick(std::int64_t now_ms);

private:
    // 开麦应答 + 按键按下登记（Ready/WaitSecondTap/Draining 三条入口共用）。
    void BeginPress(std::vector<XiaomiAtvvAction>& actions, std::int64_t now_ms, bool suppressed);
    // 640 采样 PCM 帧 → 后处理 → Opus → AudioFrame 动作（TapPending 中改为暂存）。
    void EmitPcmFrame(std::vector<XiaomiAtvvAction>& actions, std::span<const std::int16_t> pcm);
    // 长按确认：发 button_down 并放出暂存帧，进入 Streaming。
    void ConfirmLongPress(std::vector<XiaomiAtvvAction>& actions);
    // STOP 后的收尾：宽限到期/被新按下打断时补末帧（end flag）并回到 Ready/WaitSecondTap。
    void FinalizeStream(std::vector<XiaomiAtvvAction>& actions);
    // 丢弃当前按下的音频缓冲（短击/被双击消费的按下）。
    void DiscardPressBuffer();

    Options options_;
    XiaomiAtvvSessionState state_ = XiaomiAtvvSessionState::kIdle;
    bool legacy_layout_ = false;          // CAPS 版本 < v1.0，影响 TX 命令格式

    ImaAdpcmDecoder decoder_;
    FrameAccumulator accumulator_;
    PcmPostprocessor postprocessor_;
    AudioOpusEncoder encoder_;
    OpusFrameSlicer slicer_;

    std::int64_t caps_requested_at_ = 0;
    std::int64_t press_started_at_ = 0;   // 当前按下 MIC_OPEN 时刻
    std::int64_t stop_received_at_ = 0;   // 当前/最近 STOP 时刻
    std::int64_t double_click_deadline_ = 0;
    std::int64_t reject_reopen_until_ = 0;  // 长按键程 STOP 后的重开拒绝窗截止时刻
    bool press_suppressed_ = false;       // 当前按下被双击消费：音频丢弃、不发事件
    bool mic_open_remote_ = false;        // 遥控器侧 mic 打开未 STOP，断开需 MIC_CLOSE
    bool stream_active_ = false;          // 已收到 0x04，音频有效
    std::uint8_t remote_session_id_ = 0;  // 0x04 bytes[3]（可选），MIC_CLOSE 透传

    std::uint32_t next_session_id_ = 1;   // 每次发出 button_down 时取用并自增
    std::uint32_t current_session_id_ = 0;
    std::uint32_t next_seq_ = 1;
    bool session_frame_started_ = false;  // 首帧 start flag 是否已打
    // TapPending 期间已编码未确认归属会话的 Opus 帧（确认长按后补 session/seq 发出）。
    std::vector<ByteVector> pending_payloads_;
};

} // namespace voicestick
