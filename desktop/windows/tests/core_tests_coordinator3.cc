// N8 cut5: coordinator batch 3 extracted from core_tests.cc (11 tests).
// Depends only on test_support.h (fakes + builders).
#include "test_support.h"

void TestCoordinatorRecordingHardTimeoutRecoversFromLostButtonUp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(
        AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input,
        {}, {}, {}, {}, {}, std::chrono::milliseconds(300));
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 42));
    assert(ui.show_listening_count == 1);

    // 不发 button_up / audio_end（模拟两者都丢），等硬超时触发。
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    assert(asr_ptr->cancelled);
    assert(ui.hide_overlay_count >= 1);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// finalizing 闲置兜底：audio_end 后 ASR 服务端始终不回 final（on_final 不触发）时，
// finalizing watchdog 超时必须报错退出 finalizing，用户确认后回 ready，不永久卡 Processing。
void TestCoordinatorFinalizingWatchdogTimesOutWithoutAsrFinal() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 排除异步精修干扰
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input,
        {}, {}, {}, {}, {},
        std::chrono::milliseconds(5000),  // recording_hard_timeout 放大，排除干扰
        std::chrono::milliseconds(400));  // finalizing_timeout
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(30, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 30));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(30, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);

    // 不发 on_final（模拟服务端不回 SessionFinished），等 watchdog 触发报错。
    bool saw_timeout_error = false;
    for (int i = 0; i < 50 && !saw_timeout_error; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        for (const auto& e : ui.errors) {
            if (e.find("ASR response timeout") != std::string::npos) saw_timeout_error = true;
        }
    }
    assert(saw_timeout_error);
    assert(asr_ptr->cancelled);
    assert(input.pasted_text.empty());

    // 用户确认错误后回 ready，不再卡 Processing。
    assert(ui.error_completion);
    ui.error_completion();
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

// finalizing watchdog 按「无进展时长」判活：finalizing 期间持续有 partial 到达时不得误触发，
// 活动停止前的 final 仍正常粘贴。
void TestCoordinatorFinalizingWatchdogResetByPartialActivity() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input,
        {}, {}, {}, {}, {},
        std::chrono::milliseconds(5000),
        std::chrono::milliseconds(400));  // finalizing_timeout
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 32));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(32, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 32));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(32, 2));
    assert(asr_ptr->started);

    // 持续 1s 每 100ms 一个 partial（超过 400ms 闲置阈值但有活动），watchdog 不应触发。
    for (int i = 0; i < 10; ++i) {
        asr_ptr->on_partial("partial " + std::to_string(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(ui.errors.empty());
    assert(!asr_ptr->cancelled);

    asr_ptr->on_final("final text");
    assert(input.pasted_text == "final text");
    assert(ui.errors.empty());
}

// 音频流停滞兜底：录音中帧流中断（button_up 与 audio_end 双丢）时，stall watchdog 超时
// 进入等 audio_end 路径收尾，迟到的 END 帧仍被接受并 finalize，不等 120s 硬超时。
void TestCoordinatorAudioStallFinalizesWithoutButtonUp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input,
        {}, {}, {}, {}, {},
        std::chrono::milliseconds(5000),  // recording_hard_timeout 放大，排除干扰
        std::chrono::milliseconds(5000),  // finalizing_timeout 放大，排除干扰
        std::chrono::milliseconds(400));  // audio_stall_timeout
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 31));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(31, 1));

    // 之后帧流中断。轮询等 stall watchdog 触发进入 finalizing（Processing），
    // 同时保证录音时长超过 0.5s（否则按短录音取消，ASR 不启动）。
    bool entered_finalizing = false;
    for (int i = 0; i < 50 && !entered_finalizing; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        for (const auto& s : ui.statuses) {
            if (s == "Processing") entered_finalizing = true;
        }
    }
    assert(entered_finalizing);
    assert(!asr_ptr->started);  // 等 audio_end 期间 ASR 尚未启动

    // 迟到的 END 帧仍被接受：finalize 并启动 ASR 发 final chunk。
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(31, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);
}

// focused_app 模式卡在 recording 后再来 button_down（模拟残留）必须先停旧会话再 Start 新的，
// 否则被第 983 行 return 吞掉，用户怎么按都没反应。安全前提同 wechat：固件 hold_to_talk
// 录音中再按主键不发新 button_down，故收到 button_down 时非 kReady 必为残留。
void TestCoordinatorRecoveringButtonDownStopsStaleRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    assert(ui.show_listening_count == 1);

    // 模拟残留：直接发第二个 button_down（新 session 8），无 button_up。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));

    assert(asr_ptr->cancelled);
    assert(HasUiState(*ble_ptr, "recording", "5A74"));
}

// StartWechatInputMethodSession 先 renderer.Start（提前到 SendDown 之前），Start 失败直接返回，
// 不 SendDown/SendUp（首帧才弹框，未弹框则无需回滚热键），避免热键卡住。
void TestCoordinatorWechatSessionRendererStartFailureSkipsHotkey() {
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
            auto p = std::make_unique<FakeVirtualMicRenderer>(false);  // Start 失败
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

    // renderer.Start 失败：未 SendDown（等首帧才弹框），故无需补 SendUp。
    assert(fake_hotkey != nullptr);
    assert(fake_hotkey->send_down_count == 0);
    assert(fake_hotkey->send_up_count == 0);
    assert(fake_renderer != nullptr);
    assert(fake_renderer->start_count == 1);
    // 启动失败不应进入录音态（未调 ShowListening）。
    assert(ui.show_listening_count == 0);
}

// wechat 模式：button_down 后不立即 SendDown（等首帧音频就绪再弹框，避免微信弹框即取音却
// 读到静音致首字卡顿）。WASAPI renderer.Start 在 SendDown 之前完成；收到首帧 Opus 解码成功
// 入 ring_buffer 后才 SendDown 触发微信弹框。
// 点按式（click_to_talk）+ wechat：首次 button_click 启动，首帧后发 SendClick（完整
// down+up）而非 SendDown。Typeless 等点按式输入法靠完整点击触发，仅按下不释放不弹框。
void TestCoordinatorWechatClickToTalkSendsClickOnStart() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 31));

    // 首帧前不弹框（与 hold 一致）。
    assert(fake_hotkey->send_click_count == 0);
    assert(fake_hotkey->send_down_count == 0);

    AudioFrame first;
    first.session_id = 31;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);

    // 点按式首帧后发 SendClick（完整点击），不发 SendDown。
    assert(fake_hotkey->send_click_count == 1);
    assert(fake_hotkey->send_down_count == 0);
}

// 点按式停止（第二次 button_click）发 SendClick（完整点击停止），不发 SendUp。
void TestCoordinatorWechatClickToTalkSendsClickOnStop() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 32));
    AudioFrame first;
    first.session_id = 32;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    assert(fake_hotkey->send_click_count == 1);

    // 第二次 button_click（停止）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 32));
    // 启动+停止各一次 SendClick，无 SendUp。
    assert(fake_hotkey->send_click_count == 2);
    assert(fake_hotkey->send_up_count == 0);
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// 点按式停止时 audio_end 帧抢跑 button_click（固件 stop_recording 产生的 audio_end 先于
// 停止 button_click 到达）：audio_end 先停会话，迟到的停止 button_click 不得误判为启动
// 新会话（否则又弹一次）。用 session_id + 时间窗口识别并忽略迟到停止 click。
void TestCoordinatorWechatClickToTalkAudioEndOvertakesStopClick() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 33));
    AudioFrame first;
    first.session_id = 33;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    assert(fake_hotkey->send_click_count == 1);

    // audio_end 先到（固件 stop_recording 产生），停会话并记 last_stopped。
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(33, 2));
    assert(fake_hotkey->send_click_count == 2);  // 停止 SendClick
    assert(ble_ptr->sent_ui_states.back().state == "ready");

    // 迟到的停止 button_click（同 session_id）：不得启动新会话。
    int click_before = fake_hotkey->send_click_count;
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 33));
    assert(fake_hotkey->send_click_count == click_before);  // 无新 SendClick
    assert(fake_renderer->start_count == 1);  // 未启动新会话
}

// 方案 A 端到端（Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md）：小米设备 +
// click_to_talk + hold 型输入法（WeType）——click 启动立即 SendDown（不等音频帧，
// 物理 F5 流已随松开结束、静默期起算）+ 本机麦启动 + PCM 直写 ring buffer +
// 第二击（复用 session_id）SendUp/采集停止/renderer 停止完整收尾。
// 方案 A 修订（2026-09-11 真机定案）：小米 click/hold 会话由 WeType 直接采集
// 默认录音设备（真实麦克风）——本端不启动采集/渲染/自动切换默认设备。CABLE
// 绕行（本机麦 → ring buffer → CABLE → WeType）在 keyup 后被拆除会卡死 WeType
// finalize（面板不消失、TSF 宿主永久卡死），拆除即物理语义失配。
void TestCoordinatorWechatClickHoldModelXiaomiDirectDefaultMic() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;
    config.wechat_input_method.session_model = InteractionMode::kHoldToTalk;
    config.wechat_input_method.auto_switch_default_recording_device = true;
    config.wechat_input_method.virtual_mic_capture_name = "CABLE Output";

    FakeWechatInputMethodHotkey* fake_hotkey = nullptr;
    FakeVirtualMicRenderer* fake_renderer = nullptr;
    auto* fake_capture = new FakeMicCapture();
    auto* fake_switcher = new FakeDefaultAudioDeviceController();
    fake_switcher->default_capture = AudioDeviceInfo{L"real-mic-ep", L"Real Mic"};
    fake_switcher->capture_devices = {
        AudioDeviceInfo{L"real-mic-ep", L"Real Mic"},
        AudioDeviceInfo{L"cable-ep", L"CABLE Output (VB-Audio Virtual Cable)"},
    };
    const auto switch_state_path =
        std::filesystem::temp_directory_path() / "voicestick_wechat_switch_state_test.json";
    std::filesystem::remove(switch_state_path);
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
        },
        [fake_switcher]() -> std::unique_ptr<IDefaultAudioDeviceController> {
            return std::unique_ptr<IDefaultAudioDeviceController>(fake_switcher);
        },
        switch_state_path);
    // 自动松开注入长延迟：本用例只验证启动击/停止击序列（自动松开单独用例覆盖）。
    coordinator.SetWechatClickHoldRelease(std::chrono::seconds{60});
    coordinator.SetWechatDetachClickDelay(std::chrono::milliseconds{0});
    coordinator.Start();
    coordinator.SetLocalMicRuntime(std::unique_ptr<IMicCapture>(fake_capture), nullptr);

    // 登记小米设备（协调器凭 device_info 的 hardware 标签判定直连默认麦克风）。
    ble_ptr->connected_device_ids.insert("6459");
    ble_ptr->on_connection_change({ConnectedDevice{"6459", "RC-6459"}});
    StateEvent info;
    info.event = "device_info";
    info.hardware = std::string(kHardwareXiaomiRemote2Pro);
    ble_ptr->on_state_event("6459", info);

    // 第一击（启动 click，session_id=1）：立即注入按住（无 send_click、无音频帧参与）。
    ble_ptr->on_state_event("6459", ButtonEvent("button_click", "primary", 1, 120));
    assert(fake_hotkey->send_down_count == 1);
    assert(fake_hotkey->send_click_count == 0);
    // 直连默认麦克风：本端不启动采集、不 Start 虚拟麦渲染器、不动默认录音设备。
    //（渲染器对象仍由懒初始化创建，是跨会话共享成员，仅断言其未被 Start。）
    assert(fake_capture->start_count == 0);
    assert(fake_renderer != nullptr && fake_renderer->start_count == 0);
    assert(fake_switcher->set_call_count == 0);

    // 第二击（停止 click，复用 session_id=1，方案 B）：仅 SendUp 配对（文字提交靠
    // 「说完即点」时的松开提交）+ 两次鼠标左键点击（detach 关浮窗，detach 链
    // ~1.8s 故补一次），回 ready。直连模式无任何管道拆除动作。
    ble_ptr->on_state_event("6459", ButtonEvent("button_click", "primary", 1, 90));
    assert(fake_hotkey->send_down_count == 1);  // 仅启动击（无新按住提交）
    assert(fake_hotkey->send_up_count == 1);    // 停止击配对
    assert(fake_hotkey->send_click_count == 0);
    assert(fake_switcher->set_call_count == 0);
    assert(fake_capture->stop_count == 0);
    assert(input.left_click_count == 2);  // detach 点击 + 补点击
    assert(ble_ptr->sent_ui_states.back().state == "ready");
    std::filesystem::remove(switch_state_path);
}

// 点按折叠按住流限时自动松开（2026-09-12 真机定案）：click 启动的 SendDown+repeat
// 只为弹框，面板弹出后必须松开——持续 repeats 会让用户任何关闭面板的尝试（鼠标
// detach/VAD 收尾/超时）被下一个 keydown 立即重新弹开（「浮窗点关又弹出」真机
// 复验）。松开后会话仍存活；文字提交由停止击的「新按住提交」保证（见直连模式用例）。
void TestCoordinatorWechatClickHoldAutoRelease() {
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
    coordinator.SetWechatClickHoldRelease(std::chrono::milliseconds{0});
    coordinator.SetWechatDetachClickDelay(std::chrono::milliseconds{0});
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("6459");
    ble_ptr->on_connection_change({ConnectedDevice{"6459", "RC-6459"}});
    StateEvent info;
    info.event = "device_info";
    info.hardware = std::string(kHardwareXiaomiRemote2Pro);
    ble_ptr->on_state_event("6459", info);

    // 启动击：SendDown 立即注入；0ms 延迟下自动松开线程立即 SendUp。
    ble_ptr->on_state_event("6459", ButtonEvent("button_click", "primary", 1, 120));
    assert(fake_hotkey->send_down_count == 1);
    for (int i = 0; i < 100 && fake_hotkey->send_up_count == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(fake_hotkey->send_up_count == 1);  // 按住流已自动松开

    // 停止击（方案 B）：SendUp 配对 + 两次左键点击（detach 关浮窗），回 ready。
    ble_ptr->on_state_event("6459", ButtonEvent("button_click", "primary", 1, 90));
    assert(fake_hotkey->send_down_count == 1);
    assert(fake_hotkey->send_up_count == 2);
    assert(input.left_click_count == 2);
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// SavePairedDeviceInfo 未知设备（内存配对列表无此 id）不得新建零地址条目：
// 该分支曾让测试进程把 Defaults 配置整份覆盖真实 config.toml（2026-09-11 事故：
// 凭据被抹 + 配对地址清零致启动期排队连接失效，设备永远"正在连接中"）。零地址
// 条目对用户毫无价值（连接排队要求地址非零），合并仅对既有条目有意义。

// Suite entry: core_tests.cc main() calls this once.
void RunCoordinatorBatch3Tests() {
    TestCoordinatorRecordingHardTimeoutRecoversFromLostButtonUp();
    TestCoordinatorFinalizingWatchdogTimesOutWithoutAsrFinal();
    TestCoordinatorFinalizingWatchdogResetByPartialActivity();
    TestCoordinatorAudioStallFinalizesWithoutButtonUp();
    TestCoordinatorRecoveringButtonDownStopsStaleRecording();
    TestCoordinatorWechatSessionRendererStartFailureSkipsHotkey();
    TestCoordinatorWechatClickToTalkSendsClickOnStart();
    TestCoordinatorWechatClickToTalkSendsClickOnStop();
    TestCoordinatorWechatClickToTalkAudioEndOvertakesStopClick();
    TestCoordinatorWechatClickHoldModelXiaomiDirectDefaultMic();
    TestCoordinatorWechatClickHoldAutoRelease();
}

