#include "xiaomi_atvv_session.h"

#include <optional>
#include <string_view>

namespace voicestick {

namespace {

// 合成物理主键 StateEvent（source 留空，与固件物理键一致）。
StateEvent MakePrimaryButtonEvent(std::string_view event,
                                  std::optional<std::uint32_t> session_id) {
    StateEvent state_event;
    state_event.event = std::string(event);
    state_event.button = "primary";
    state_event.session_id = session_id;
    return state_event;
}

} // namespace

XiaomiAtvvSession::XiaomiAtvvSession(Options options)
    : options_(options), postprocessor_(options.gain_db) {}

std::vector<XiaomiAtvvAction> XiaomiAtvvSession::Start(std::int64_t now_ms) {
    std::vector<XiaomiAtvvAction> actions;
    if (state_ != XiaomiAtvvSessionState::kIdle) return actions;
    caps_requested_at_ = now_ms;
    state_ = XiaomiAtvvSessionState::kCapsRequested;
    actions.push_back(XiaomiAtvvWriteTx{XiaomiAtvvProtocol::GetCapsCommand()});
    return actions;
}

std::vector<XiaomiAtvvAction> XiaomiAtvvSession::Stop(std::int64_t now_ms) {
    (void)now_ms;
    std::vector<XiaomiAtvvAction> actions;
    if (mic_open_remote_) {
        actions.push_back(XiaomiAtvvWriteTx{
            XiaomiAtvvProtocol::MicCloseCommand(legacy_layout_, remote_session_id_)});
    }
    // 复位到 Idle：重连后需重新 Start 握手；session 计数保持递增不复用。
    state_ = XiaomiAtvvSessionState::kIdle;
    mic_open_remote_ = false;
    stream_active_ = false;
    press_suppressed_ = false;
    legacy_layout_ = false;
    reject_reopen_until_ = 0;
    caps_requested_at_ = 0;
    press_started_at_ = 0;
    stop_received_at_ = 0;
    double_click_deadline_ = 0;
    decoder_.Reset(0, 0);
    encoder_.Reset();
    accumulator_.Reset();
    slicer_.Reset();
    pending_payloads_.clear();
    current_session_id_ = 0;
    next_seq_ = 1;
    session_frame_started_ = false;
    return actions;
}

std::vector<XiaomiAtvvAction> XiaomiAtvvSession::HandleControlCommand(
    std::span<const std::uint8_t> data, std::int64_t now_ms) {
    std::vector<XiaomiAtvvAction> actions;
    if (state_ == XiaomiAtvvSessionState::kError || data.empty()) return actions;

    switch (data[0]) {
        case XiaomiAtvvProtocol::control_caps: {
            if (state_ != XiaomiAtvvSessionState::kCapsRequested &&
                state_ != XiaomiAtvvSessionState::kReady) {
                break;
            }
            const auto caps = XiaomiAtvvProtocol::ParseCaps(data);
            if (!caps.has_value()) break;  // 坏包忽略，由 CAPS 超时兜底
            if (!caps->Supports16kHz()) {
                state_ = XiaomiAtvvSessionState::kError;
                actions.push_back(XiaomiAtvvError{"unsupported_codec"});
                break;
            }
            legacy_layout_ = !caps->IsV1OrLater();
            accumulator_.set_frame_bytes(caps->frame_bytes);
            state_ = XiaomiAtvvSessionState::kReady;
            break;
        }
        case XiaomiAtvvProtocol::control_mic_open: {
            if (state_ == XiaomiAtvvSessionState::kWaitSecondTap) {
                if (now_ms <= double_click_deadline_) {
                    // 窗内第二次按下：合成 button_double_click（桌面端注入 Enter），
                    // 本次按下被消费：仍按协议应答，但不录音、不发 down/up。
                    actions.push_back(XiaomiAtvvStateEvent{
                        MakePrimaryButtonEvent("button_double_click", std::nullopt)});
                    BeginPress(actions, now_ms, /*suppressed=*/true);
                } else {
                    // Tick 未跑窗口已过。hold_to_talk 的短击尚未发过任何事件，补发
                    // button_click 表达本次单击；click_to_talk 的第一次点击已由
                    // down/up 完整表达并开录，补发会让协调器空闲态启动幽灵会话。
                    if (options_.interaction_mode == InteractionMode::kHoldToTalk) {
                        actions.push_back(XiaomiAtvvStateEvent{
                            MakePrimaryButtonEvent("button_click", std::nullopt)});
                    }
                    state_ = XiaomiAtvvSessionState::kReady;
                    BeginPress(actions, now_ms, /*suppressed=*/false);
                }
                break;
            }
            // STOP 后 300ms 重开拒绝窗（kDraining 与窗内已回 kReady 均生效）：
            // 不回 0x0C ACK、不开新会话。双击路径走 kWaitSecondTap 分支，不受影响。
            if ((state_ == XiaomiAtvvSessionState::kDraining ||
                 state_ == XiaomiAtvvSessionState::kReady) &&
                now_ms < reject_reopen_until_) {
                break;
            }
            if (state_ == XiaomiAtvvSessionState::kDraining) {
                // 拒绝窗外重开：先收尾当前会话再开新按下。
                FinalizeStream(actions);
                BeginPress(actions, now_ms, /*suppressed=*/false);
                break;
            }
            if (state_ != XiaomiAtvvSessionState::kReady) break;
            BeginPress(actions, now_ms, /*suppressed=*/false);
            break;
        }
        case XiaomiAtvvProtocol::control_stream_start: {
            if (state_ != XiaomiAtvvSessionState::kTapPending &&
                state_ != XiaomiAtvvSessionState::kStreaming &&
                state_ != XiaomiAtvvSessionState::kDraining) {
                break;
            }
            // RC003 坑：遥控器每次会话编码器从 0/0 重启但可能不发 SYNC，
            // 收到 0x04 一律硬重置，否则第二次按键 DC 饱和。
            decoder_.Reset(0, 0);
            accumulator_.Reset();
            slicer_.Reset();
            pending_payloads_.clear();
            stream_active_ = true;
            if (data.size() >= 4) remote_session_id_ = data[3];
            break;
        }
        case XiaomiAtvvProtocol::control_audio_sync: {
            if (data.size() < 7) break;
            if (state_ != XiaomiAtvvSessionState::kTapPending &&
                state_ != XiaomiAtvvSessionState::kStreaming &&
                state_ != XiaomiAtvvSessionState::kDraining) {
                break;
            }
            const auto predictor = static_cast<std::int16_t>((data[4] << 8) | data[5]);
            decoder_.Reset(predictor, data[6]);
            accumulator_.Reset();
            slicer_.Reset();
            pending_payloads_.clear();
            break;
        }
        case XiaomiAtvvProtocol::control_stop: {
            mic_open_remote_ = false;
            // 注意不清 stream_active_：Audio 与 Control 是两条独立特征，
            // STOP 后 150ms 宽限内的音频尾包仍须接收（FinalizeStream 才清）。
            stop_received_at_ = now_ms;
            if (state_ == XiaomiAtvvSessionState::kTapPending) {
                DiscardPressBuffer();
                if (press_suppressed_) {
                    // 被双击消费的第二次按下：松开不再发事件。
                    press_suppressed_ = false;
                    state_ = XiaomiAtvvSessionState::kReady;
                } else {
                    // 短击：进入双击窗，窗超时由 Tick 发 button_click。
                    double_click_deadline_ = now_ms + options_.double_click_window_ms;
                    state_ = XiaomiAtvvSessionState::kWaitSecondTap;
                }
            } else if (state_ == XiaomiAtvvSessionState::kStreaming) {
                actions.push_back(XiaomiAtvvStateEvent{
                    MakePrimaryButtonEvent("button_up", std::nullopt)});
                // 音频尾包可能晚于 STOP 到达：留 150ms 宽限。
                state_ = XiaomiAtvvSessionState::kDraining;
                // 规格：STOP 后 300ms 内拒绝重开会话（防遥控器抖动/急速重开）。
                // 仅长按键程（经过 kStreaming）武装；短击走 kWaitSecondTap 不设窗。
                reject_reopen_until_ = now_ms + kReopenRejectMs;
            }
            break;
        }
        default:
            break;
    }
    return actions;
}

std::vector<XiaomiAtvvAction> XiaomiAtvvSession::HandleAudioData(
    std::span<const std::uint8_t> data, std::int64_t now_ms) {
    std::vector<XiaomiAtvvAction> actions;
    if (state_ != XiaomiAtvvSessionState::kTapPending &&
        state_ != XiaomiAtvvSessionState::kStreaming &&
        state_ != XiaomiAtvvSessionState::kDraining) {
        return actions;
    }
    if (!stream_active_ || press_suppressed_) return actions;
    // 超宽限的尾包丢弃。
    if (state_ == XiaomiAtvvSessionState::kDraining &&
        now_ms - stop_received_at_ > kAudioTailGraceMs) {
        return actions;
    }
    for (const auto& adpcm_frame : accumulator_.Append(data)) {
        const auto pcm = decoder_.Decode(adpcm_frame);
        for (const auto& frame : slicer_.Append(pcm)) {
            EmitPcmFrame(actions, frame);
        }
    }
    return actions;
}

std::vector<XiaomiAtvvAction> XiaomiAtvvSession::Tick(std::int64_t now_ms) {
    std::vector<XiaomiAtvvAction> actions;
    switch (state_) {
        case XiaomiAtvvSessionState::kCapsRequested:
            if (now_ms - caps_requested_at_ >= kCapsTimeoutMs) {
                state_ = XiaomiAtvvSessionState::kError;
                actions.push_back(XiaomiAtvvError{"caps_timeout"});
            }
            break;
        case XiaomiAtvvSessionState::kTapPending:
            // 按住 ≥300ms 确认长按；被双击消费的按下不确认。
            if (!press_suppressed_ && now_ms - press_started_at_ >= kHoldThresholdMs) {
                ConfirmLongPress(actions);
            }
            break;
        case XiaomiAtvvSessionState::kDraining:
            if (now_ms - stop_received_at_ >= kAudioTailGraceMs) {
                FinalizeStream(actions);
            }
            break;
        case XiaomiAtvvSessionState::kWaitSecondTap:
            if (now_ms >= double_click_deadline_) {
                // 双击窗超时无第二次按下。hold_to_talk：短击未发过事件，补发
                // button_click（协调器侧为无害 no-op）；click_to_talk：第一次点击
                // 已由 down/up 完整表达并开录，补发会让协调器空闲态启动幽灵会话，
                // 故只回 Ready 不发事件。
                if (options_.interaction_mode == InteractionMode::kHoldToTalk) {
                    actions.push_back(XiaomiAtvvStateEvent{
                        MakePrimaryButtonEvent("button_click", std::nullopt)});
                }
                state_ = XiaomiAtvvSessionState::kReady;
            }
            break;
        default:
            break;
    }
    return actions;
}

void XiaomiAtvvSession::BeginPress(std::vector<XiaomiAtvvAction>& actions,
                                   std::int64_t now_ms, bool suppressed) {
    actions.push_back(XiaomiAtvvWriteTx{XiaomiAtvvProtocol::MicOpenAckCommand(legacy_layout_)});
    mic_open_remote_ = true;
    stream_active_ = false;
    press_started_at_ = now_ms;
    press_suppressed_ = suppressed;
    pending_payloads_.clear();
    if (!suppressed) {
        // 对齐固件（audio_pipeline.c：每次录音会话开始 OPUS_RESET_STATE）：
        // 新按下即重置 Opus 编码器，杜绝跨会话状态泄漏。hold_to_talk 的
        // TapPending 暂存帧与 click_to_talk 的立即流都从这里开始编码，
        // ConfirmLongPress 不再重复 Reset（避免暂存帧与确认后流帧之间
        // 编码器状态被人为切断）。
        encoder_.Reset();
    }
    if (!suppressed && options_.interaction_mode == InteractionMode::kClickToTalk) {
        // click_to_talk：MIC_OPEN 立即发 button_down（协调器按 down/up 处理）。
        current_session_id_ = next_session_id_++;
        next_seq_ = 1;
        session_frame_started_ = false;
        actions.push_back(XiaomiAtvvStateEvent{
            MakePrimaryButtonEvent("button_down", current_session_id_)});
        state_ = XiaomiAtvvSessionState::kStreaming;
    } else {
        state_ = XiaomiAtvvSessionState::kTapPending;
    }
}

void XiaomiAtvvSession::EmitPcmFrame(std::vector<XiaomiAtvvAction>& actions,
                                     std::span<const std::int16_t> pcm) {
    const auto processed = postprocessor_.Process(pcm);
    std::uint8_t buffer[1500];  // 32kbps × 40ms ≈ 160B，余量充足
    const auto result = encoder_.Encode(processed.data(), processed.size(), buffer, sizeof(buffer));
    if (result.opus_error != 0) return;
    if (state_ == XiaomiAtvvSessionState::kTapPending) {
        // 长按未确认：暂存（确认后补 session/seq 发出，不丢前 300ms 语音）。
        // 防御性上限：Tick 失能时避免无界增长（300ms 阈值下正常 ≤8 帧）。
        if (pending_payloads_.size() < 100) {
            pending_payloads_.emplace_back(buffer, buffer + result.encoded_bytes);
        }
        return;
    }
    AudioFrame frame;
    frame.session_id = current_session_id_;
    frame.seq = next_seq_++;
    frame.flags = session_frame_started_ ? 0x00 : 0x01;
    session_frame_started_ = true;
    frame.payload.assign(buffer, buffer + result.encoded_bytes);
    actions.push_back(XiaomiAtvvAudioFrame{std::move(frame)});
}

void XiaomiAtvvSession::ConfirmLongPress(std::vector<XiaomiAtvvAction>& actions) {
    // 编码器状态已在 BeginPress 重置，此处不再 Reset（保持暂存帧与后续流帧连续）。
    current_session_id_ = next_session_id_++;
    next_seq_ = 1;
    session_frame_started_ = false;
    actions.push_back(XiaomiAtvvStateEvent{
        MakePrimaryButtonEvent("button_down", current_session_id_)});
    state_ = XiaomiAtvvSessionState::kStreaming;
    // 暂存帧补 session/seq 后按序流出（首帧打 start flag）。
    for (auto& payload : pending_payloads_) {
        AudioFrame frame;
        frame.session_id = current_session_id_;
        frame.seq = next_seq_++;
        frame.flags = session_frame_started_ ? 0x00 : 0x01;
        session_frame_started_ = true;
        frame.payload = std::move(payload);
        actions.push_back(XiaomiAtvvAudioFrame{std::move(frame)});
    }
    pending_payloads_.clear();
}

void XiaomiAtvvSession::FinalizeStream(std::vector<XiaomiAtvvAction>& actions) {
    // 余量补零出末帧（end flag）；无有效音频帧的会话不产生任何 AudioFrame。
    auto remainder = slicer_.TakeRemainder();
    if (!remainder.empty()) {
        remainder.resize(AudioOpusEncoder::kFrameSamples, 0);
        const auto processed = postprocessor_.Process(remainder);
        std::uint8_t buffer[1500];
        const auto result =
            encoder_.Encode(processed.data(), processed.size(), buffer, sizeof(buffer));
        if (result.opus_error == 0) {
            AudioFrame frame;
            frame.session_id = current_session_id_;
            frame.seq = next_seq_++;
            frame.flags = session_frame_started_ ? 0x00 : 0x01;
            frame.flags |= 0x02;
            session_frame_started_ = true;
            frame.payload.assign(buffer, buffer + result.encoded_bytes);
            actions.push_back(XiaomiAtvvAudioFrame{std::move(frame)});
        }
    } else if (session_frame_started_) {
        // 无余量但本会话已有帧：补空 payload end 帧让协调器收尾（既有空 END 路径）。
        AudioFrame frame;
        frame.session_id = current_session_id_;
        frame.seq = next_seq_++;
        frame.flags = 0x02;
        actions.push_back(XiaomiAtvvAudioFrame{std::move(frame)});
    }
    accumulator_.Reset();
    pending_payloads_.clear();
    stream_active_ = false;
    // click_to_talk 的短按（按下即开录、松开即停）武装双击窗；长按直接回 Ready。
    if (options_.interaction_mode == InteractionMode::kClickToTalk &&
        stop_received_at_ - press_started_at_ < kHoldThresholdMs) {
        double_click_deadline_ = stop_received_at_ + options_.double_click_window_ms;
        state_ = XiaomiAtvvSessionState::kWaitSecondTap;
    } else {
        state_ = XiaomiAtvvSessionState::kReady;
    }
}

void XiaomiAtvvSession::DiscardPressBuffer() {
    pending_payloads_.clear();
    slicer_.Reset();
    accumulator_.Reset();
    decoder_.Reset(0, 0);  // 防御性复位；下个会话 0x04 还会硬重置
    stream_active_ = false;
}

} // namespace voicestick
