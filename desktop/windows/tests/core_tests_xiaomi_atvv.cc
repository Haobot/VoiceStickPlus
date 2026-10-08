// N8 cut7: xiaomi ATVV-session batch extracted from core_tests.cc (8 tests).
// Depends only on test_support.h; cross-span refs pre-scanned = 0.
#include "test_support.h"

void TestXiaomiAtvvSessionFlow() {
    // CAPS 初始化超时 → Error。
    {
        XiaomiAtvvSession session;
        assert(session.Start(0).size() == 1);
        assert(session.Tick(XiaomiAtvvSession::kCapsTimeoutMs - 1).empty());
        const auto actions = session.Tick(XiaomiAtvvSession::kCapsTimeoutMs);
        assert(HasAtvvError(actions, "caps_timeout"));
        assert(session.state() == XiaomiAtvvSessionState::kError);
    }

    XiaomiAtvvSession session;
    // 未握手时 MIC_OPEN 不应答。
    assert(session.HandleControlCommand(ByteVector{0x08}, 0).empty());

    auto actions = session.Start(0);
    const auto* tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr);
    assert(tx->bytes == (ByteVector{0x0A, 0x01, 0x00, 0x00, 0x03, 0x03}));
    assert(session.state() == XiaomiAtvvSessionState::kCapsRequested);

    actions = session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, 10);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // MIC_OPEN：只回 0C 00，暂不发送 button_down。
    actions = session.HandleControlCommand(ByteVector{0x08}, 1000);
    tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr && tx->bytes == (ByteVector{0x0C, 0x00}));
    assert(FindAtvvEvent(actions, "button_down") == nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);

    // 0x04 流开始（无 SYNC）：硬重置解码器。
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x09}, 1010);
    assert(actions.empty());
    assert(session.decoder().predictor() == 0 && session.decoder().step_index() == 0);

    // 120B ADPCM = 240 采样，不足 640 采样帧，无输出。
    assert(session.HandleAudioData(ByteVector(120, 0x11), 1020).empty());

    // 300ms 长按阈值前不发 button_down；跨过阈值才发（session_id=1）。
    assert(session.Tick(1000 + XiaomiAtvvSession::kHoldThresholdMs - 1).empty());
    actions = session.Tick(1000 + XiaomiAtvvSession::kHoldThresholdMs);
    const auto* down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && down->button == "primary");
    assert(down->session_id.has_value() && *down->session_id == 1);
    assert(session.state() == XiaomiAtvvSessionState::kStreaming);

    // 再喂 240B（480 采样）→ 累计 720 → 出首帧（640 采样，start flag）。
    actions = session.HandleAudioData(ByteVector(240, 0x11), 1320);
    auto frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1);
    assert(frames[0].session_id == 1 && frames[0].seq == 1);
    assert(frames[0].IsStart() && !frames[0].IsEnd());
    assert(!frames[0].payload.empty());

    // STOP → button_up + Draining。
    actions = session.HandleControlCommand(ByteVector{0x00}, 2000);
    const auto* up = FindAtvvEvent(actions, "button_up");
    assert(up != nullptr && up->button == "primary");
    assert(session.state() == XiaomiAtvvSessionState::kDraining);

    // 150ms 宽限内尾包收下：再 480B（960 采样）→ 出一帧。
    actions = session.HandleAudioData(ByteVector(480, 0x11), 2100);
    frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].seq == 2 && !frames[0].IsEnd());

    // 超宽限的尾包丢弃（Tick 尚未收尾，仍处 Draining）。
    assert(session.HandleAudioData(ByteVector(120, 0x11),
                                   2000 + XiaomiAtvvSession::kAudioTailGraceMs + 1).empty());

    // 宽限到期：余量补零出末帧（end flag），回 Ready。
    actions = session.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs + 2);
    frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].seq == 3);
    assert(frames[0].IsEnd() && !frames[0].IsStart());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // STOP 已收到，断开不再发 MIC_CLOSE。
    assert(session.Stop(3000).empty());
    assert(session.state() == XiaomiAtvvSessionState::kIdle);

    // Stop 回到 Idle 后需重新握手（模拟断连重连；session id 计数保持递增）。
    session.Start(3100);
    session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, 3110);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // RC003 坑：第二次会话不发 SYNC，0x04 必须硬重置解码器。
    // 第一次会话把 predictor 推到饱和（0x77 连发）。
    session.HandleControlCommand(ByteVector{0x08}, 4000);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0A}, 4010);
    session.Tick(4000 + XiaomiAtvvSession::kHoldThresholdMs);
    session.HandleAudioData(ByteVector(120, 0x77), 4320);
    assert(session.decoder().predictor() == 32767);
    session.HandleControlCommand(ByteVector{0x00}, 5000);
    session.Tick(5000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 第二次会话 0x04 无 SYNC → predictor/step 归零。
    session.HandleControlCommand(ByteVector{0x08}, 6000);
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0B}, 6010);
    assert(session.decoder().predictor() == 0 && session.decoder().step_index() == 0);

    // 0x0A AUDIO_SYNC：按值重置并清空帧累积器。
    session.HandleAudioData(ByteVector(100, 0x11), 6020);
    assert(session.accumulator().pending_bytes() == 100);
    session.HandleControlCommand(ByteVector{0x0A, 0x00, 0x00, 0x00, 0x01, 0x00, 10}, 6030);
    assert(session.decoder().predictor() == 256 && session.decoder().step_index() == 10);
    assert(session.accumulator().pending_bytes() == 0);
    // predictor 为 BE 有符号：0xFF00 = -256。
    session.HandleControlCommand(ByteVector{0x0A, 0x00, 0x00, 0x00, 0xFF, 0x00, 5}, 6040);
    assert(session.decoder().predictor() == -256 && session.decoder().step_index() == 5);
    // SYNC 后确认长按（session_id=3）并收尾。
    actions = session.Tick(6000 + XiaomiAtvvSession::kHoldThresholdMs);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 3);
    session.HandleControlCommand(ByteVector{0x00}, 7000);
    session.Tick(7000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 8kHz-only → Error，后续输入全部忽略。
    XiaomiAtvvSession unsupported;
    unsupported.Start(0);
    actions = unsupported.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78}, 10);
    assert(HasAtvvError(actions, "unsupported_codec"));
    assert(unsupported.state() == XiaomiAtvvSessionState::kError);
    assert(unsupported.HandleControlCommand(ByteVector{0x08}, 100).empty());
    assert(unsupported.HandleAudioData(ByteVector(120, 0x11), 100).empty());

    // 旧版布局：MIC_OPEN 应答带 codec 字节；MIC_CLOSE 仅 0x0D。
    XiaomiAtvvSession legacy;
    legacy.Start(0);
    legacy.HandleControlCommand(ByteVector{0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00}, 10);
    actions = legacy.HandleControlCommand(ByteVector{0x08}, 100);
    tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr && tx->bytes == (ByteVector{0x0C, 0x00, 0x02}));
    actions = legacy.Stop(200);
    tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr && tx->bytes == (ByteVector{0x0D}));

    // v1 且 mic 未 STOP 时断开：MIC_CLOSE 透传 0x04 的 session id。
    XiaomiAtvvSession closing;
    AtvvHandshakeReady(closing, 0);
    closing.HandleControlCommand(ByteVector{0x08}, 100);
    closing.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x09}, 110);
    actions = closing.Stop(200);
    tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr && tx->bytes == (ByteVector{0x0D, 0x09}));
}

void TestXiaomiAtvvSessionKeyMapping() {
    // 默认 hold_to_talk，双击窗 350ms。
    XiaomiAtvvSession session;
    AtvvHandshakeReady(session, 0);

    // 长按：MIC_OPEN 只应答不发事件；缓冲音频在确认后流出（不丢前 300ms 语音）。
    auto actions = session.HandleControlCommand(ByteVector{0x08}, 100);
    assert(FindAtvvWriteTx(actions) != nullptr);
    assert(FindAtvvEvent(actions, "button_down") == nullptr);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, 110);
    assert(session.HandleAudioData(ByteVector(480, 0x11), 150).empty());  // 960 采样暂存
    assert(session.Tick(399).empty());
    actions = session.Tick(100 + XiaomiAtvvSession::kHoldThresholdMs);
    const auto* down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && down->button == "primary" && *down->session_id == 1);
    auto frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].session_id == 1 && frames[0].IsStart());
    session.HandleControlCommand(ByteVector{0x00}, 2000);
    session.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 短击：缓冲音频丢弃，无 button_down/up；窗超时发 button_click。
    session.HandleControlCommand(ByteVector{0x08}, 3000);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x02}, 3010);
    session.HandleAudioData(ByteVector(240, 0x11), 3020);
    actions = session.HandleControlCommand(ByteVector{0x00}, 3150);  // 150ms < 300ms
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    assert(session.Tick(3150 + 349).empty());
    actions = session.Tick(3150 + 350);
    const auto* click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && click->button == "primary");
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 双击：窗内第二次 MIC_OPEN → button_double_click（仍应答 TX），不录音。
    session.HandleControlCommand(ByteVector{0x08}, 5000);
    session.HandleControlCommand(ByteVector{0x00}, 5100);
    actions = session.HandleControlCommand(ByteVector{0x08}, 5200);  // 5100+350 窗内
    assert(FindAtvvEvent(actions, "button_double_click") != nullptr);
    assert(FindAtvvEvent(actions, "button_down") == nullptr);
    assert(FindAtvvWriteTx(actions) != nullptr);
    // 被双击消费的第二次按下：音频丢弃、跨阈值不发事件、松开不再发事件。
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x03}, 5210);
    assert(session.HandleAudioData(ByteVector(480, 0x11), 5220).empty());
    assert(session.Tick(5600).empty());
    actions = session.HandleControlCommand(ByteVector{0x00}, 5700);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);
    assert(session.Tick(6100).empty());  // 无滞留双击窗

    // 三击：双击窗只合成一次 double_click，第三次按下正常开录（会话 id 自增）。
    session.HandleControlCommand(ByteVector{0x08}, 7000);
    session.HandleControlCommand(ByteVector{0x00}, 7100);
    actions = session.HandleControlCommand(ByteVector{0x08}, 7200);
    assert(FindAtvvEvent(actions, "button_double_click") != nullptr);
    session.HandleControlCommand(ByteVector{0x00}, 7300);
    session.HandleControlCommand(ByteVector{0x08}, 8000);
    actions = session.Tick(8000 + XiaomiAtvvSession::kHoldThresholdMs);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 2);
    session.HandleControlCommand(ByteVector{0x00}, 9000);
    session.Tick(9000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // session_id 在 button_down 与 AudioFrame 间一致且逐会话自增。
    session.HandleControlCommand(ByteVector{0x08}, 10000);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x04}, 10010);
    actions = session.Tick(10000 + XiaomiAtvvSession::kHoldThresholdMs);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 3);
    actions = session.HandleAudioData(ByteVector(480, 0x11), 10310);
    frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].session_id == 3);
    session.HandleControlCommand(ByteVector{0x00}, 11000);
    session.Tick(11000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // click_to_talk：MIC_OPEN 立即发 button_down，STOP 发 button_up；双击仍合成。
    XiaomiAtvvSession::Options click_options;
    click_options.interaction_mode = InteractionMode::kClickToTalk;
    XiaomiAtvvSession click_session(click_options);
    AtvvHandshakeReady(click_session, 0);
    actions = click_session.HandleControlCommand(ByteVector{0x08}, 100);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 1);
    assert(click_session.state() == XiaomiAtvvSessionState::kStreaming);
    actions = click_session.HandleControlCommand(ByteVector{0x00}, 250);  // 短按 150ms
    assert(FindAtvvEvent(actions, "button_up") != nullptr);
    // 尾包宽限收尾后进入双击窗（短按）。
    click_session.Tick(250 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(click_session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = click_session.HandleControlCommand(ByteVector{0x08}, 500);  // 250+350 窗内
    assert(FindAtvvEvent(actions, "button_double_click") != nullptr);
    click_session.HandleControlCommand(ByteVector{0x00}, 600);
    assert(click_session.state() == XiaomiAtvvSessionState::kReady);
    // 窗后按下：立即开录新会话。
    actions = click_session.HandleControlCommand(ByteVector{0x08}, 5000);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 2);
}

void TestXiaomiAtvvSessionClickTapTimeoutSilent() {
    // click_to_talk：短按已由 button_down/up 完整表达并开录，双击窗超时不得再补发
    // button_click（否则协调器 click 分支把空闲态无 duration_ms 的 click 当启动，
    // 产生永远收不到音频的幽灵会话，靠硬超时报错收场）。
    XiaomiAtvvSession::Options click_options;
    click_options.interaction_mode = InteractionMode::kClickToTalk;
    XiaomiAtvvSession session(click_options);
    AtvvHandshakeReady(session, 0);

    auto actions = session.HandleControlCommand(ByteVector{0x08}, 100);
    assert(FindAtvvEvent(actions, "button_down") != nullptr);  // 按下即开录
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, 110);
    session.HandleAudioData(ByteVector(240, 0x11), 120);
    actions = session.HandleControlCommand(ByteVector{0x00}, 200);  // 短按 100ms
    assert(FindAtvvEvent(actions, "button_up") != nullptr);
    // 尾包宽限收尾：click 短按武装双击窗。
    session.Tick(200 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    // 双击窗超时：无任何补发事件，仅回 Ready。
    actions = session.Tick(200 + 350);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // Tick 未跑、窗已过恰好 MIC_OPEN：同样不补发 button_click，直接开新会话。
    session.HandleControlCommand(ByteVector{0x08}, 1000);
    session.HandleControlCommand(ByteVector{0x00}, 1100);
    session.Tick(1100 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = session.HandleControlCommand(ByteVector{0x08}, 1100 + 350 + 10);
    assert(FindAtvvEvent(actions, "button_click") == nullptr);
    const auto* down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr);  // 新按下立即开录
    assert(session.state() == XiaomiAtvvSessionState::kStreaming);
    session.HandleControlCommand(ByteVector{0x00}, 2000);
    session.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);

    // 对照：hold_to_talk 窗超时仍补发 button_click（短击未发过任何事件，
    // 协调器 hold 分支对 click 是无害 no-op）。Tick 未跑窗已过的路径同样补发。
    XiaomiAtvvSession hold_session;
    AtvvHandshakeReady(hold_session, 0);
    hold_session.HandleControlCommand(ByteVector{0x08}, 100);
    hold_session.HandleControlCommand(ByteVector{0x00}, 200);
    assert(hold_session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = hold_session.Tick(200 + 350);
    assert(FindAtvvEvent(actions, "button_click") != nullptr);
    assert(hold_session.state() == XiaomiAtvvSessionState::kReady);

    hold_session.HandleControlCommand(ByteVector{0x08}, 1000);
    hold_session.HandleControlCommand(ByteVector{0x00}, 1100);
    assert(hold_session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = hold_session.HandleControlCommand(ByteVector{0x08}, 1100 + 350 + 10);
    assert(FindAtvvEvent(actions, "button_click") != nullptr);
    assert(hold_session.state() == XiaomiAtvvSessionState::kTapPending);
    hold_session.HandleControlCommand(ByteVector{0x00}, 2000);
}

void TestXiaomiAtvvSessionReopenRejectWindow() {
    // 规格：STOP 后 300ms 内拒绝重开会话（防遥控器抖动/急速重开）。
    XiaomiAtvvSession session;  // hold_to_talk
    AtvvHandshakeReady(session, 0);

    // 长按完整键程：MIC_OPEN → STREAM_START → 长按确认 → STOP → Draining。
    session.HandleControlCommand(ByteVector{0x08}, 1000);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, 1010);
    auto actions = session.Tick(1000 + XiaomiAtvvSession::kHoldThresholdMs);
    assert(FindAtvvEvent(actions, "button_down") != nullptr);
    actions = session.HandleControlCommand(ByteVector{0x00}, 2000);
    assert(FindAtvvEvent(actions, "button_up") != nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kDraining);

    // 拒绝窗内（Draining，STOP 后 100ms）MIC_OPEN：忽略，无 ACK、无事件、状态不变。
    actions = session.HandleControlCommand(ByteVector{0x08}, 2100);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kDraining);

    // 尾包宽限到期收尾回 Ready，但拒绝窗（300ms）仍未满：MIC_OPEN 依旧忽略。
    session.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(session.state() == XiaomiAtvvSessionState::kReady);
    actions = session.HandleControlCommand(ByteVector{0x08}, 2200);  // STOP 后 200ms
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 窗满（STOP 后 ≥300ms）：正常应答并开新按下。
    actions = session.HandleControlCommand(ByteVector{0x08}, 2000 + XiaomiAtvvSession::kReopenRejectMs);
    assert(FindAtvvWriteTx(actions) != nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);
    session.HandleControlCommand(ByteVector{0x00}, 2400);  // 短击收尾
    session.Tick(2400 + 350 + 1);  // 双击窗超时回 Ready

    // 双击路径不受拒绝窗影响：click 短按的 STOP 也武装拒绝窗，
    // 但第二击走 kWaitSecondTap 分支，窗内照常合成 button_double_click。
    XiaomiAtvvSession::Options click_options;
    click_options.interaction_mode = InteractionMode::kClickToTalk;
    XiaomiAtvvSession click_session(click_options);
    AtvvHandshakeReady(click_session, 0);
    click_session.HandleControlCommand(ByteVector{0x08}, 1000);
    click_session.HandleControlCommand(ByteVector{0x00}, 1100);  // 短按 → Draining + 拒绝窗
    click_session.Tick(1100 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(click_session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = click_session.HandleControlCommand(ByteVector{0x08}, 1200);  // STOP 后 100ms，拒绝窗内
    assert(FindAtvvEvent(actions, "button_double_click") != nullptr);
    click_session.HandleControlCommand(ByteVector{0x00}, 1300);

    // Stop 复位清窗：重连握手后立即可开录（不残留拒绝窗）。
    XiaomiAtvvSession restart;
    AtvvHandshakeReady(restart, 0);
    restart.HandleControlCommand(ByteVector{0x08}, 1000);
    restart.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, 1010);
    restart.Tick(1000 + XiaomiAtvvSession::kHoldThresholdMs);
    restart.HandleControlCommand(ByteVector{0x00}, 2000);  // 武装拒绝窗（至 2300）
    restart.Stop(2050);  // 断连复位
    AtvvHandshakeReady(restart, 2100);
    actions = restart.HandleControlCommand(ByteVector{0x08}, 2150);  // 旧窗内时刻
    assert(FindAtvvWriteTx(actions) != nullptr);
    assert(restart.state() == XiaomiAtvvSessionState::kTapPending);
}

// 方案 A（Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md）：wechat 模式下小米
// 语音键折叠为 click toggle——按下无事件、按住音频丢弃、松开合成 button_click。
// session_id 协议：启动击用新 id、停止击复用同 id（协调器停止匹配
// *event.session_id == *active_session_id_ 零改动命中）。
void TestXiaomiAtvvWechatClickToggle() {
    // 对齐 win32_app resolver 的真实填法：wechat + click_to_talk + 按住式输入法。
    XiaomiAtvvSession::Options options;
    options.interaction_mode = InteractionMode::kClickToTalk;
    options.wechat_click_toggle = true;
    XiaomiAtvvSession session(options);
    AtvvHandshakeReady(session, 0);

    // 2 Pro 一体帧 0x04 按下：无事件无 TX（0x04 不回 ACK），进 TapPending。
    auto actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x09}, 100);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);

    // 按住期间音频帧全部丢弃（无 AudioFrame 输出）。
    actions = session.HandleAudioData(ByteVector(480, 0x11), 150);
    assert(CollectAtvvFrames(actions).empty());

    // 跨过 300ms 长按阈值也不确认长按（无 button_down，长按也是一次 click）。
    actions = session.Tick(100 + XiaomiAtvvSession::kHoldThresholdMs);
    assert(FindAtvvEvent(actions, "button_down") == nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);

    // STOP → 合成 button_click（session_id=1、duration>0）→ Ready（不开双击窗）。
    actions = session.HandleControlCommand(ByteVector{0x00}, 800);
    const auto* click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && click->button == "primary");
    assert(click->session_id.has_value() && *click->session_id == 1);
    assert(click->duration_ms.has_value() && *click->duration_ms > 0);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 拒绝窗内（STOP 后 100ms）第二击被拒：无事件、状态不变。
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0A}, 900);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 窗外第二击（启动击）：STOP 后复用 session_id=1（协调器停止匹配用）。
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0A}, 1200);
    assert(CollectAtvvFrames(session.HandleAudioData(ByteVector(480, 0x11), 1250)).empty());
    actions = session.HandleControlCommand(ByteVector{0x00}, 1300);
    click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && *click->session_id == 1);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // 第三击（toggle 重启）：分配新 session_id=2。
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0B}, 2000);
    actions = session.HandleControlCommand(ByteVector{0x00}, 2100);
    click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && *click->session_id == 2);

    // 断开 Stop() 复位 toggle：重连后第一击用全新 id（3），第二击复用 3。
    session.Stop(2500);
    AtvvHandshakeReady(session, 2600);
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0C}, 2700);
    actions = session.HandleControlCommand(ByteVector{0x00}, 2800);
    click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && *click->session_id == 3);
    // 上一击 STOP@2800 武装拒绝窗至 3100，第四击须在窗外。
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x0D}, 3200);
    actions = session.HandleControlCommand(ByteVector{0x00}, 3300);
    click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && *click->session_id == 3);

    // RC003 入径（0x08 等待 ACK）：同样折叠为 click，ACK 照回。
    XiaomiAtvvSession rc003(options);
    AtvvHandshakeReady(rc003, 0);
    actions = rc003.HandleControlCommand(ByteVector{0x08}, 100);
    const auto* tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr);  // 0x0C ACK
    assert(FindAtvvEvent(actions, "button_down") == nullptr);
    actions = rc003.HandleControlCommand(ByteVector{0x00}, 200);
    click = FindAtvvEvent(actions, "button_click");
    assert(click != nullptr && click->session_id.has_value());
}

void TestXiaomiAtvvStreamStartOpensSession() {
    // 2 Pro 入径（真机实测）：按下语音键直接发 0x04 <interaction> <codec> <sid>
    //（按下+开流一体帧，无 0x08、主机不写 0x0C ACK），松开发 0x00（可带尾字节）。
    // hold_to_talk：0x04 → TapPending（无 ACK、无事件），音频即刻缓冲。
    XiaomiAtvvSession session;
    AtvvHandshakeReady(session, 0);

    auto actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x03}, 1000);
    assert(actions.empty());  // 无 ACK、无事件
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);

    // 流已激活：音频立即缓冲（240B=480 采样 <640 不出帧）；跨阈值确认长按。
    assert(session.HandleAudioData(ByteVector(240, 0x11), 1010).empty());
    actions = session.Tick(1000 + XiaomiAtvvSession::kHoldThresholdMs);
    const auto* down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && down->session_id.has_value() && *down->session_id == 1);
    assert(session.state() == XiaomiAtvvSessionState::kStreaming);

    actions = session.HandleAudioData(ByteVector(240, 0x11), 1310);  // 累计 960 → 出首帧
    auto frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].session_id == 1 && frames[0].IsStart());

    // STOP 带尾字节（00 02）：button_up + Draining，宽限到期出末帧回 Ready。
    actions = session.HandleControlCommand(ByteVector{0x00, 0x02}, 2000);
    assert(FindAtvvEvent(actions, "button_up") != nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kDraining);
    actions = session.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);
    frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].IsEnd());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // codec 非 16kHz（byte2=0x01）：上报错误、不开会话、不回 ACK。
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x01, 0x05}, 3000);
    assert(HasAtvvError(actions, "unsupported_codec"));
    assert(FindAtvvWriteTx(actions) == nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // STOP 后 300ms 重开拒绝窗对 0x04 同样生效：先跑一轮长按键程武装拒绝窗。
    session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x06}, 4000);
    session.Tick(4000 + XiaomiAtvvSession::kHoldThresholdMs);
    session.HandleControlCommand(ByteVector{0x00, 0x02}, 5000);  // Draining，拒绝窗至 5300
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x07}, 5100);
    assert(actions.empty());  // 窗内忽略
    assert(session.state() == XiaomiAtvvSessionState::kDraining);

    // 窗外 0x04 重开：Draining 收尾（本轮无音频故无末帧）后直接开新按下。
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x08}, 5400);
    assert(FindAtvvWriteTx(actions) == nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);

    // 短击松开 → 双击窗；窗内第二个 0x04 合成 button_double_click，按下被消费
    //（无 ACK、音频丢弃、跨阈值不确认、松手无事件）。
    actions = session.HandleControlCommand(ByteVector{0x00, 0x02}, 5410);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kWaitSecondTap);
    actions = session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x09}, 5500);
    assert(FindAtvvEvent(actions, "button_double_click") != nullptr);
    assert(FindAtvvWriteTx(actions) == nullptr);
    assert(session.state() == XiaomiAtvvSessionState::kTapPending);
    assert(session.HandleAudioData(ByteVector(240, 0x11), 5510).empty());
    assert(session.Tick(5500 + XiaomiAtvvSession::kHoldThresholdMs).empty());
    actions = session.HandleControlCommand(ByteVector{0x00, 0x02}, 5700);
    assert(actions.empty());
    assert(session.state() == XiaomiAtvvSessionState::kReady);

    // click_to_talk：0x04 直开立即发 button_down，音频即时流出。
    XiaomiAtvvSession::Options click_options;
    click_options.interaction_mode = InteractionMode::kClickToTalk;
    XiaomiAtvvSession clicker(click_options);
    AtvvHandshakeReady(clicker, 0);
    actions = clicker.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, 100);
    assert(FindAtvvWriteTx(actions) == nullptr);
    down = FindAtvvEvent(actions, "button_down");
    assert(down != nullptr && *down->session_id == 1);
    assert(clicker.state() == XiaomiAtvvSessionState::kStreaming);
    clicker.HandleAudioData(ByteVector(240, 0x11), 110);
    actions = clicker.HandleAudioData(ByteVector(240, 0x11), 120);
    frames = CollectAtvvFrames(actions);
    assert(frames.size() == 1 && frames[0].IsStart());
    actions = clicker.HandleControlCommand(ByteVector{0x00, 0x02}, 200);
    assert(FindAtvvEvent(actions, "button_up") != nullptr);

    // 0x04 直开路径下断开：MIC_CLOSE 透传 0x04 byte3 的会话计数。
    XiaomiAtvvSession closing;
    AtvvHandshakeReady(closing, 0);
    closing.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x42}, 100);
    actions = closing.Stop(200);
    const auto* tx = FindAtvvWriteTx(actions);
    assert(tx != nullptr && tx->bytes == (ByteVector{0x0D, 0x42}));
}

void TestXiaomiAtvvSessionEncoderResetPerSession() {
    // 上一会话的 Opus 编码器残余状态不得污染下一会话（对齐固件 audio_pipeline.c
    // 每次会话开始 OPUS_RESET_STATE）：复用 session 的第二会话首帧须与全新
    // session 的首帧逐字节一致；无 Reset 时 SILK/CELT 内部预测器状态会使其不同。
    const auto kHoldFirstFramePayload = [](XiaomiAtvvSession& session, std::int64_t t) {
        session.HandleControlCommand(ByteVector{0x08}, t);
        session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, t + 10);
        session.HandleAudioData(ByteVector(480, 0x11), t + 20);  // 960 采样 → 暂存 1 帧
        const auto actions = session.Tick(t + XiaomiAtvvSession::kHoldThresholdMs);
        const auto frames = CollectAtvvFrames(actions);
        assert(frames.size() == 1 && frames[0].IsStart());
        return frames[0].payload;
    };

    // 全新 session 的首会话首帧（编码器出厂状态）。
    XiaomiAtvvSession fresh;
    AtvvHandshakeReady(fresh, 0);
    const auto fresh_first = kHoldFirstFramePayload(fresh, 1000);

    // 复用 session：会话 1（多编几帧改变内部状态）结束后开会话 2。
    XiaomiAtvvSession reused;
    AtvvHandshakeReady(reused, 0);
    const auto reused_first = kHoldFirstFramePayload(reused, 1000);
    assert(reused_first == fresh_first);  // sanity：首会话本就该一致
    reused.HandleAudioData(ByteVector(480, 0x77), 1400);  // 会话 1 多喂两帧
    reused.HandleControlCommand(ByteVector{0x00}, 2000);
    reused.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(reused.state() == XiaomiAtvvSessionState::kReady);
    const auto reused_second = kHoldFirstFramePayload(reused, 10000);  // 远超拒绝窗
    assert(reused_second == fresh_first);  // 关键：Reset 后与全新逐字节一致

    // click_to_talk 立即路径同样每会话 Reset（BeginPress 非 suppressed 分支覆盖）。
    const auto kClickFirstFramePayload = [](XiaomiAtvvSession& session, std::int64_t t) {
        session.HandleControlCommand(ByteVector{0x08}, t);
        session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, t + 10);
        const auto actions = session.HandleAudioData(ByteVector(480, 0x11), t + 20);
        const auto frames = CollectAtvvFrames(actions);
        assert(frames.size() == 1 && frames[0].IsStart());
        return frames[0].payload;
    };
    XiaomiAtvvSession::Options click_options;
    click_options.interaction_mode = InteractionMode::kClickToTalk;
    XiaomiAtvvSession click_fresh(click_options);
    AtvvHandshakeReady(click_fresh, 0);
    const auto click_fresh_first = kClickFirstFramePayload(click_fresh, 1000);

    XiaomiAtvvSession click_reused(click_options);
    AtvvHandshakeReady(click_reused, 0);
    const auto click_reused_first = kClickFirstFramePayload(click_reused, 1000);
    assert(click_reused_first == click_fresh_first);
    click_reused.HandleControlCommand(ByteVector{0x00}, 2000);  // 按压 1000ms（长按路径）
    click_reused.Tick(2000 + XiaomiAtvvSession::kAudioTailGraceMs);
    assert(click_reused.state() == XiaomiAtvvSessionState::kReady);
    const auto click_reused_second = kClickFirstFramePayload(click_reused, 10000);
    assert(click_reused_second == click_fresh_first);
}

void TestXiaomiAtvvServiceUuidAd() {
    // ATVV service UUID 线上小端字节：AB5E0001-5A21-4F05-BC7D-AF01F617B664。
    const ByteVector atvv_ad = {
        0x02, 0x01, 0x06,  // flags
        0x11, 0x06,        // incomplete 128-bit service UUID list，16 字节
        0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
        0x05, 0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab,
    };
    assert(BleProtocol::HasXiaomiAtvvServiceUuid(atvv_ad));
    assert(!BleProtocol::HasVoiceStickServiceUuid(atvv_ad));

    // complete list（0x07）同样识别。
    ByteVector complete_ad = atvv_ad;
    complete_ad[4] = 0x07;
    assert(BleProtocol::HasXiaomiAtvvServiceUuid(complete_ad));

    // VoiceStick 广播不含 ATVV UUID；空/截断数据不误报。
    const ByteVector vs_ad = {
        0x02, 0x01, 0x06,
        0x11, 0x07,
        0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
        0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f,
    };
    assert(BleProtocol::HasVoiceStickServiceUuid(vs_ad));
    assert(!BleProtocol::HasXiaomiAtvvServiceUuid(vs_ad));
    assert(!BleProtocol::HasXiaomiAtvvServiceUuid(ByteVector{}));
    assert(!BleProtocol::HasXiaomiAtvvServiceUuid(ByteVector{0x11, 0x06, 0x64}));
}

// ---- 协调器 × 小米事件流（规格 §7.1）----
// 事件由 XiaomiAtvvSession 真实产出后直接注入 FakeBleCentral 回调（不需真 BLE）；
// 协调器对设备类别无感知，RC-XXXX 与 VS-XXXX 走同一状态机。

// 把 session 产出的动作注入协调器（WriteTx 是回遥控器字节、Error 无协调器语义，均忽略）。
void InjectAtvvActions(FakeBleCentral& ble, const std::string& device_id,
                       const std::vector<XiaomiAtvvAction>& actions) {
    for (const auto& action : actions) {
        if (const auto* event = std::get_if<XiaomiAtvvStateEvent>(&action)) {
            ble.on_state_event(device_id, event->event);
        } else if (const auto* frame = std::get_if<XiaomiAtvvAudioFrame>(&action)) {
            ble.on_audio_frame(device_id, frame->frame);
        }
    }
}


// hold_to_talk 按下段：MIC_OPEN → STREAM_START → 音频暂存 → 跨 300ms 阈值确认长按
// （button_down + 暂存帧注入协调器）。返回后协调器应处于 recording。
void AtvvBeginHoldRecording(FakeBleCentral& ble, const std::string& device_id,
                            XiaomiAtvvSession& session, std::int64_t& t) {
    InjectAtvvActions(ble, device_id, session.HandleControlCommand(ByteVector{0x08}, t));
    InjectAtvvActions(ble, device_id,
                      session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, t + 10));
    InjectAtvvActions(ble, device_id, session.HandleAudioData(ByteVector(480, 0x11), t + 20));
    InjectAtvvActions(ble, device_id, session.Tick(t + XiaomiAtvvSession::kHoldThresholdMs));
    t += XiaomiAtvvSession::kHoldThresholdMs;
}

// 松开段：STOP（button_up 注入）→ 150ms 尾包宽限到期 FinalizeStream（end 帧注入）。
void AtvvEndRecording(FakeBleCentral& ble, const std::string& device_id,
                      XiaomiAtvvSession& session, std::int64_t& t) {
    t += 600;
    InjectAtvvActions(ble, device_id, session.HandleControlCommand(ByteVector{0x00}, t));
    InjectAtvvActions(ble, device_id, session.Tick(t + XiaomiAtvvSession::kAudioTailGraceMs));
    t += XiaomiAtvvSession::kAudioTailGraceMs;
}

// ①hold_to_talk 正常录音 → ASR 送出 Ogg，final 后粘贴。

// Suite entry: core_tests.cc main() calls this once.
void RunXiaomiAtvvBatchTests() {
    TestXiaomiAtvvSessionFlow();
    TestXiaomiAtvvSessionKeyMapping();
    TestXiaomiAtvvSessionClickTapTimeoutSilent();
    TestXiaomiAtvvSessionReopenRejectWindow();
    TestXiaomiAtvvWechatClickToggle();
    TestXiaomiAtvvStreamStartOpensSession();
    TestXiaomiAtvvSessionEncoderResetPerSession();
    TestXiaomiAtvvServiceUuidAd();
}

