// N8 cut8: coordinator batch 5 extracted from core_tests.cc (8 tests).
// Pre-checks: col0 non-test heads = 0, cross-span refs = 0 (multi-line aware).
#include "test_support.h"

void TestCoordinatorWechatClickHoldModelStickNoLocalMic() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;
    config.wechat_input_method.session_model = InteractionMode::kHoldToTalk;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    FakeVirtualMicRenderer* fake_renderer = nullptr;
    auto* fake_capture = new FakeMicCapture();
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [&fake_renderer](const IVirtualMicRenderer::Options&) {
            auto p = std::make_unique<FakeVirtualMicRenderer>(true);
            fake_renderer = p.get();
            return p;
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();
    coordinator.SetLocalMicRuntime(std::unique_ptr<IMicCapture>(fake_capture), nullptr);

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 7, 150));
    assert(fake_hotkey->send_down_count == 1);
    assert(fake_capture->start_count == 0);  // 非小米：不启本机麦
    assert(fake_renderer != nullptr && fake_renderer->start_count == 1);  // CABLE 管道保留
}

// 停止顺序（WeType commit 挂死定案，2026-09-11 真机）：keyup 必须先于 CABLE 管道
// 拆除——WeType 诊断日志显示先停音频流再发 keyup 时，finalize/commit 卡死
//（composition_commit_timeout / composition 永久不终止）；物理松开时麦克风永远
// 还在供电（房间底噪持续），合成释放须模拟同一语义（宽限窗口 wechat_stop_audio_grace_）。

// 带事件顺序记录的渲染/热键假件（order 由测试持有，仅测试线程触达）。
class RecordingVirtualMicRenderer : public FakeVirtualMicRenderer {
public:
    explicit RecordingVirtualMicRenderer(std::vector<std::string>* order)
        : FakeVirtualMicRenderer(true), order_(order) {}
    void Stop() override {
        order_->push_back("renderer_stop");
        FakeVirtualMicRenderer::Stop();
    }
private:
    std::vector<std::string>* order_;
};

class RecordingHotkey : public FakeWechatInputMethodHotkey {
public:
    explicit RecordingHotkey(std::vector<std::string>* order = nullptr) : order_(order) {}
    bool SendDown() const override {
        if (order_) order_->push_back("send_down");
        return FakeWechatInputMethodHotkey::SendDown();
    }
    bool SendUp() const override {
        if (order_) order_->push_back("send_up");
        return FakeWechatInputMethodHotkey::SendUp();
    }
private:
    std::vector<std::string>* order_;
};

void TestCoordinatorWechatStopReleasesHotkeyBeforeStoppingMic() {
    // StickS3 hold_to_talk（BLE 音频经 CABLE 管道；方案 A 直连麦克风模式无本端
    // 管道，不适用本顺序约束）：停止时 SendUp 必须早于 renderer 停止，保证 keyup
    // 到达时音频流仍存活。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    std::vector<std::string> order;
    RecordingHotkey* recording_hotkey = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [&order](const IVirtualMicRenderer::Options&) {
            return std::make_unique<RecordingVirtualMicRenderer>(&order);
        },
        [&recording_hotkey, &order](const std::string&) {
            auto p = std::make_unique<RecordingHotkey>(&order);
            recording_hotkey = p.get();
            return p;
        });
    coordinator.SetWechatStopAudioGrace(std::chrono::milliseconds{0});
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 首帧解码成功触发 SendDown。
    AudioFrame first;
    first.session_id = 7;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    assert(recording_hotkey != nullptr);
    assert(order.size() >= 3);
    assert(order[0] == "send_down");
    const auto up_pos = std::find(order.begin(), order.end(), "send_up");
    const auto stop_pos = std::find(order.begin(), order.end(), "renderer_stop");
    assert(up_pos != order.end());
    assert(stop_pos != order.end());
    assert(up_pos < stop_pos);
}

// 点动式残留 active（停止 click + audio_end 都丢）时，新启动 click（新 session_id）
// 与 hold 模式 TestCoordinatorWechatInputMethodRecoversFromStaleActive 对称，验证 click_to_talk
// 残留自愈（HandleWechatInputMethodPrimaryButtonDown 先 Stop 旧再 Start 新）。
void TestCoordinatorWechatClickToTalkStaleActiveNewClickStartsNew() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;

    FakeVirtualMicRenderer* fake_renderer = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [&fake_renderer](const IVirtualMicRenderer::Options&) {
            auto p = std::make_unique<FakeVirtualMicRenderer>(true);
            fake_renderer = p.get();
            return p;
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 会话 41 启动（残留 active 前提：停止 click + audio_end 都丢，不发任何结束信号）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 41));
    assert(fake_renderer->start_count == 1);

    // 新启动 click(42)（新 session_id）：不得当停止，应先停旧再启新。
    // bug 下（:824 不校验 session_id）被误当停止，start_count 仍为 1。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 42));
    assert(fake_renderer->start_count == 2);  // 新会话已 Start
    assert(fake_renderer->stop_count >= 1);   // 旧会话已 Stop
}

void TestCoordinatorWechatHotkeyDeferredUntilFirstAudioFrame() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    FakeVirtualMicRenderer* fake_renderer = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [&fake_renderer](const IVirtualMicRenderer::Options&) {
            auto p = std::make_unique<FakeVirtualMicRenderer>(true);
            fake_renderer = p.get();
            return p;
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));

    // button_down 后 WASAPI Start 已完成，但 SendDown 尚未触发（等首帧）。
    assert(fake_renderer != nullptr);
    assert(fake_renderer->start_count == 1);
    assert(fake_hotkey != nullptr);
    assert(fake_hotkey->send_down_count == 0);

    // 注入有效 Opus 首帧（解码成功入 ring_buffer）后才 SendDown 弹框。
    AudioFrame first;
    first.session_id = 7;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    assert(fake_hotkey->send_down_count == 1);
}

// 首帧到达前用户已松开（快速点按）：未 SendDown 故 Stop 时不补 SendUp，会话正常收尾回 ready。
void TestCoordinatorWechatHotkeySkippedBeforeFirstFrameButtonUp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 首帧前即松开：未 SendDown，Stop 不补 SendUp。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    assert(fake_hotkey != nullptr);
    assert(fake_hotkey->send_down_count == 0);
    assert(fake_hotkey->send_up_count == 0);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// 首帧 SendDown 后 button_up：Stop 必须配对 SendUp，否则 Ctrl+Win 卡住。
void TestCoordinatorWechatHotkeySendUpPairedAfterFirstFrame() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    AudioFrame first;
    first.session_id = 7;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    assert(fake_hotkey->send_down_count == 1);

    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    assert(fake_hotkey->send_up_count == 1);
}

// 首帧解码失败（无效 Opus payload）时不 SendDown，等下一帧解码成功才弹框。
void TestCoordinatorWechatHotkeySkippedOnDecodeFailure() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 无效 Opus payload（{1,2,3,4}）解码失败，不 SendDown。
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1));
    assert(fake_hotkey->send_down_count == 0);
    // 下一帧有效 Opus，解码成功 -> SendDown。
    AudioFrame valid;
    valid.session_id = 7;
    valid.seq = 2;
    valid.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", valid);
    assert(fake_hotkey->send_down_count == 1);
}

// 首帧即 audio_end（空 payload，极端短按）时不 SendDown，正常收尾回 ready。
void TestCoordinatorWechatHotkeySkippedOnEmptyEndFirstFrame() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [&fake_hotkey](const std::string&) {
            auto p = std::make_unique<FakeWechatInputMethodHotkey>();
            fake_hotkey = p.get();
            return p;
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(7, 1));

    assert(fake_hotkey->send_down_count == 0);
    assert(fake_hotkey->send_up_count == 0);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}


// Suite entry: core_tests.cc main() calls this once.
void RunCoordinatorBatch5Tests() {
    TestCoordinatorWechatClickHoldModelStickNoLocalMic();
    TestCoordinatorWechatStopReleasesHotkeyBeforeStoppingMic();
    TestCoordinatorWechatClickToTalkStaleActiveNewClickStartsNew();
    TestCoordinatorWechatHotkeyDeferredUntilFirstAudioFrame();
    TestCoordinatorWechatHotkeySkippedBeforeFirstFrameButtonUp();
    TestCoordinatorWechatHotkeySendUpPairedAfterFirstFrame();
    TestCoordinatorWechatHotkeySkippedOnDecodeFailure();
    TestCoordinatorWechatHotkeySkippedOnEmptyEndFirstFrame();
}

