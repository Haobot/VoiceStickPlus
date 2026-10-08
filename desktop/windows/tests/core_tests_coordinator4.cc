// N8 cut6: coordinator batch 4 extracted from core_tests.cc (9 tests).
// Depends only on test_support.h (fakes + builders); cross-span refs pre-scanned = 0.
#include "test_support.h"

void TestCoordinatorWechatInputMethodStopsOnDeviceDisconnect() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    assert(fake_renderer != nullptr);
    assert(fake_renderer->start_count == 1);

    // 模拟 BLE 闪断：设备断连后，wechat 会话必须被完整停止（renderer 停、状态清），
    // 否则重连后 button_up 条件不匹配、button_down 被残留 active 忽略，卡在 Recording。
    ble_ptr->connected_device_ids.erase("5A74");
    ble_ptr->on_connection_change({});
    assert(fake_renderer->stop_count >= 1);

    // 重连后应能重新开始录音（wechat 状态已清理）。
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));
    assert(fake_renderer->start_count == 2);
}

void TestCoordinatorWechatInputMethodHandlesEmptyEndFrame() {
    // 空 payload + IsEnd 的结束帧（固件常用此表示音频流结束），wechat 路径应识别为
    // audio_end 并收尾，与主路径一致；button_up 后应落盘调试音频并回到 ready。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.debug_audio_cache = true;
    const auto debug_dir =
        std::filesystem::temp_directory_path() / "voicestick_wechat_empty_end_test";
    std::filesystem::remove_all(debug_dir);
    std::filesystem::create_directories(debug_dir);
    config.debug_audio_directory = debug_dir;

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 跨过 0.5s 最小录音时长阈值，避免被短录音过滤丢弃。
    // sleep 须在音频帧之前：audio_end 帧到达即触发落盘判断，此时 duration 须已过阈值。
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(7, 2));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
        if (entry.path().extension() == ".ogg" && std::filesystem::file_size(entry) > 0) {
            found = true;
            break;
        }
    }
    assert(found);
    // button_up 后应回到 ready（EnterReady 广播 ready，device_id 为空）。
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");

    std::filesystem::remove_all(debug_dir);
}

// wechat 模式 button_down 后零音频帧即 button_up（hold_to_talk_instant 按下即开录音，
// 无意点按或 button_up 抢跑早于所有音频帧到达），调试音频应 Discard 不落盘，
// 避免产生仅含 ogg 头+EOS 的 128 字节空文件。与 focused_app/subtitle 路径行为对齐。
void TestCoordinatorWechatInputMethodDiscardsZeroFrameRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.debug_audio_cache = true;
    const auto debug_dir =
        std::filesystem::temp_directory_path() / "voicestick_wechat_discard_zero_test";
    std::filesystem::remove_all(debug_dir);
    std::filesystem::create_directories(debug_dir);
    config.debug_audio_directory = debug_dir;

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 零音频帧即松开。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
        if (entry.path().extension() == ".ogg" && std::filesystem::file_size(entry) > 0) {
            found = true;
            break;
        }
    }
    assert(!found);

    std::filesystem::remove_all(debug_dir);
}

// wechat 模式仅收到 1 个 40ms 音频帧即 audio_end（短于 0.5s 最小录音时长），调试音频应
// Discard 不落盘，避免产生仅含 ogg 头+1 帧的 289 字节极小文件。
void TestCoordinatorWechatInputMethodDiscardsShortSingleFrameRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.debug_audio_cache = true;
    const auto debug_dir =
        std::filesystem::temp_directory_path() / "voicestick_wechat_discard_short_test";
    std::filesystem::remove_all(debug_dir);
    std::filesystem::create_directories(debug_dir);
    config.debug_audio_directory = debug_dir;

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 仅 1 个 40ms 音频帧即 IsEnd（测试同步执行，duration 远小于 0.5s）。
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1, true));

    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
        if (entry.path().extension() == ".ogg" && std::filesystem::file_size(entry) > 0) {
            found = true;
            break;
        }
    }
    assert(!found);

    std::filesystem::remove_all(debug_dir);
}

// wechat 模式主键双击必须注入 Enter（与 focused_app 一致），不能被 wechat 分支吞掉。
void TestCoordinatorWechatInputMethodDoubleClickSendsEnter() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 长按录音后松开（正常结束），微信文字已进输入框。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    // 双击应注入 Enter 发送。
    ble_ptr->on_state_event("5A74", DoubleClickEvent("primary"));
    assert(input.send_enter_called);
}

// button_up 走 BLE notify 无 ACK，闪断会丢；audio_end 帧到达时必须自愈结束整个会话，
// 否则 wechat_active 残留、renderer 不停、热键不松，下次长按被吞（"完全无反应"）。
void TestCoordinatorWechatInputMethodAudioEndStopsSessionWithoutButtonUp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1));
    // 只发 audio_end，不发 button_up（模拟 button_up 丢失）。
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(7, 2));
    assert(fake_renderer->stop_count >= 1);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// 上次会话 button_up/audio_end 都丢致 wechat_active 残留时，新 button_down 必须先 Stop
// 旧会话再 Start 新的，否则被 392 行 return 吞掉，用户长按完全无反应。
// 安全前提：固件 hold_to_talk 录音中再按主键不发新 button_down，故收到 button_down 时
// wechat_active=true 必为残留。
void TestCoordinatorWechatInputMethodRecoversFromStaleActive() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    assert(fake_renderer->start_count == 1);
    // 模拟残留：直接发第二个 button_down（新 session 8），无 button_up。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));
    assert(fake_renderer->start_count == 2);
    assert(fake_renderer->stop_count >= 1);
}

// wechat 模式 button_down 进会话后，若 button_up 与 audio_end 都丢失（固件 drain 超时 +
// BLE 抖动），硬超时兜底必须回 ready 并下发 ready 给设备，避免永久卡 listening（wechat 模式
// 原无硬超时，卡死后只能靠下次 button_down 残留自愈）。
void TestCoordinatorWechatRecordingHardTimeoutRecoversFromLostButtonUp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;

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
        },
        {}, {}, std::chrono::milliseconds(300));
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    assert(fake_renderer->start_count == 1);

    // 不发 button_up / audio_end（模拟固件 drain 超时丢帧 + BLE 抖动 button_up 丢），等硬超时。
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    assert(fake_renderer->stop_count >= 1);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// wechat 模式 + hold_to_talk 时下发给固件 hold_to_talk_instant（按下即录音跳过 300ms 阈值，
// 降低按下到弹框延迟）；非 wechat 模式仍下发用户配置的 hold_to_talk（保留 300ms 意图确认）。
void TestCoordinatorWechatModeSendsInstantInteractionMode() {
    auto ble1 = std::make_unique<FakeBleCentral>();
    auto* ble_ptr1 = ble1.get();
    auto asr1 = std::make_unique<FakeAsrClient>();
    FakeUi ui1;
    FakeInputInjector input1;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    VoiceStickCoordinator coordinator1(config, std::move(ble1), std::move(asr1), &ui1, &input1);
    coordinator1.Start();
    ble_ptr1->connected_device_ids.insert("5A74");
    ble_ptr1->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    assert(!ble_ptr1->sent_interaction_modes.empty());
    assert(ble_ptr1->sent_interaction_modes.back().first == InteractionMode::kHoldToTalkInstant);

    // 非 wechat 模式（focused_app）应下发用户配置的 hold_to_talk。
    auto ble2 = std::make_unique<FakeBleCentral>();
    auto* ble_ptr2 = ble2.get();
    auto asr2 = std::make_unique<FakeAsrClient>();
    FakeUi ui2;
    FakeInputInjector input2;
    AppConfig config2 = AppConfig::Defaults();
    config2.default_output_profile.target = OutputTarget::kFocusedApp;
    VoiceStickCoordinator coordinator2(config2, std::move(ble2), std::move(asr2), &ui2, &input2);
    coordinator2.Start();
    ble_ptr2->connected_device_ids.insert("5A74");
    ble_ptr2->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    assert(!ble_ptr2->sent_interaction_modes.empty());
    assert(ble_ptr2->sent_interaction_modes.back().first == InteractionMode::kHoldToTalk);
}

// wechat 选点按式（trigger_mode=kClickToTalk）但全局 interaction_mode=hold 时，切到
// focused_app 后下发给固件的应是 hold_to_talk（不被 wechat 点按式污染），否则固件
// click_to_talk 下长按主键不发 button_down，focused_app 长按无法录音。

// Suite entry: core_tests.cc main() calls this once.
void RunCoordinatorBatch4Tests() {
    TestCoordinatorWechatInputMethodStopsOnDeviceDisconnect();
    TestCoordinatorWechatInputMethodHandlesEmptyEndFrame();
    TestCoordinatorWechatInputMethodDiscardsZeroFrameRecording();
    TestCoordinatorWechatInputMethodDiscardsShortSingleFrameRecording();
    TestCoordinatorWechatInputMethodDoubleClickSendsEnter();
    TestCoordinatorWechatInputMethodAudioEndStopsSessionWithoutButtonUp();
    TestCoordinatorWechatInputMethodRecoversFromStaleActive();
    TestCoordinatorWechatRecordingHardTimeoutRecoversFromLostButtonUp();
    TestCoordinatorWechatModeSendsInstantInteractionMode();
}

