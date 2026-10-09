// N8 cut3: coordinator batch extracted from core_tests.cc (20 tests,
// contiguous run at extraction time). Depends only on test_support.h
// (fakes + builders) and the shared include block it carries.
#include "test_support.h"

void TestCoordinatorCancelsShortPrimaryPress() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 42));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 42));

    assert(asr_ptr->cancelled);
    assert(ui.show_listening_count == 1);
    assert(ui.hide_overlay_count == 1);
    assert(HasUiState(*ble_ptr, "recording", "5A74"));
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

void TestCoordinatorPrimaryDuringFinalizingRefreshesThinking() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    assert(!asr_ptr->started);

    const auto before = ble_ptr->sent_ui_states.size();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", std::nullopt));

    assert(ble_ptr->sent_ui_states.size() == before + 1);
    assert(ble_ptr->sent_ui_states.back().state == "thinking");
    assert(ble_ptr->sent_ui_states.back().device_id == std::optional<std::string>("5A74"));

    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(7, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);
}

void TestCoordinatorSecondaryCancelsFinalizing() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(8, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 8));

    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "secondary"));

    assert(asr_ptr->cancelled);
    assert(ui.hide_overlay_count == 1);
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

void TestCoordinatorAcceptsAudioFramesAfterButtonUpUntilEnd() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 14));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(14, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 14));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(14, 2));

    assert(!asr_ptr->started);

    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(14, 3));

    assert(asr_ptr->started);
    assert(asr_ptr->sent_chunks >= 3);
    assert(asr_ptr->last_chunk_was_final);
}

// BLE 断连（僵尸链路）发生在 final 音频块已发出、ASR final 尚在网络侧在途时：
// 不得取消 ASR——final 到达后应照常粘贴。修复「流式已上屏但断连导致不粘贴」。
void TestCoordinatorDisconnectAwaitingAsrFinalKeepsSession() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 关异步精修，聚焦断连与 ASR final 的交互
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 21));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(21, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 21));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(21, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);

    ble_ptr->connected_device_ids.erase("5A74");
    ble_ptr->on_connection_change({});
    assert(!asr_ptr->cancelled);

    // ASR final 在断连后到达（走网络与 BLE 无关）：必须照常粘贴。
    asr_ptr->on_final("hello");
    assert(input.pasted_text == "hello");
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

// 对照：仍在录音（final 块未发出）时断连，维持原有取消语义。
void TestCoordinatorDisconnectDuringRecordingCancelsSession() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 22));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(22, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));

    ble_ptr->connected_device_ids.erase("5A74");
    ble_ptr->on_connection_change({});
    assert(asr_ptr->cancelled);
}

void TestCoordinatorMainFinalPastesWithoutConfirmation() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 本用例验证同步粘贴流程，关闭异步精修以免触发真实 LLM 调用
    config.auto_enter = true;       // 默认已改为 false，本用例显式开启以验证粘贴后回车
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 9));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(9, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 9));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(9, 2));
    asr_ptr->on_final("hello");

    assert(input.pasted_text == "hello");
    assert(input.pasted_enter);
    assert(ui.final_countdowns.empty());
    assert(ui.paused_finals.empty());
    assert(ui.hide_overlay_count == 1);
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

// 开启精修时，ASR final 到达后应立即把原文刷上悬浮窗（ShowRefining），
// 而非冻结在旧 partial 上等待 LLM 首 token。不可达 base_url 使精修快速失败回退到原文。
void TestCoordinatorRefineShowsOriginalTextImmediately() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    // refine_enabled 默认 true；用不可达 base_url 强制 LLM 快速失败回退。
    // 不依赖 llm_api_key 空：开发/MSI 构建内置 key 非空时 ActiveLlmApiKey 仍非空，
    // 改用无效 base_url 让 WinHttpCrackUrl 同步失败，触发 on_error →
    // Refine → ChatSync 同样失败 → on_complete(false, 原文)。
    config.refine_enabled = true;  // 默认已改为 false，本用例显式开启以验证精修回退路径
    assert(config.refine_enabled);
    config.llm_base_url = "http://[";
    config.auto_enter = true;      // 默认已改为 false，本用例显式开启以验证粘贴后回车
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 21));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(21, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 21));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(21, 2));
    asr_ptr->on_final("hello refine");

    // 关键断言：final 后立即调用 ShowRefining 显示原文，不等 LLM 首 token。
    assert(!ui.refining_texts.empty());
    assert(ui.refining_texts.front() == "hello refine");

    // 等待后台精修失败回退完成，最终粘贴原文。
    for (int i = 0; i < 50 && input.pasted_text.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(input.pasted_text == "hello refine");
    assert(input.pasted_enter);
}

void TestCoordinatorOtherDeviceDuringRecordingGetsReady() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 10));

    const auto before = ble_ptr->sent_ui_states.size();
    ble_ptr->on_state_event("6B85", ButtonEvent("button_down", "primary", 11));

    assert(ble_ptr->sent_ui_states.size() == before + 1);
    assert(ble_ptr->sent_ui_states.back().state == "ready");
    assert(ble_ptr->sent_ui_states.back().device_id == std::optional<std::string>("6B85"));
}

void TestCoordinatorSubtitleOutputSkipsPaste() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    FakeAsrClient* subtitle_asr_ptr = nullptr;
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kClickToTalk;
    config.refine_enabled = false;  // 字幕用例验证同步显示流程，关闭异步精修以免触发真实 LLM 调用
    config.device_theme_colors["5A74"] = OverlayThemeColor::kBlue;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [&](const AppConfig&) {
            auto asr = std::make_unique<FakeAsrClient>();
            subtitle_asr_ptr = asr.get();
            return asr;
        });
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 12));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(12, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 12));
    assert(subtitle_asr_ptr != nullptr);
    assert(!subtitle_asr_ptr->started);
    subtitle_asr_ptr->on_partial("early subtitle");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "early subtitle");
    assert(ble_ptr->sent_ui_states.back().text != "early subtitle");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(12, 2));
    assert(subtitle_asr_ptr->started);
    assert(subtitle_asr_ptr->last_options.show_utterances);
    assert(subtitle_asr_ptr->last_options.result_type == AsrResultType::kSingle);
    subtitle_asr_ptr->on_partial("interim subtitle");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "interim subtitle");
    subtitle_asr_ptr->on_final("hello subtitle");

    assert(input.pasted_text.empty());
    assert(!ui.subtitles.empty());
    assert(ui.subtitles.back() == "5A74:hello subtitle:blue");
    assert(ui.hide_overlay_count > 0);
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

void TestCoordinatorSubtitleFinalDoesNotBlockNextSession() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    std::vector<FakeAsrClient*> subtitle_asrs;
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kHoldToTalk;
    config.refine_enabled = false;  // 字幕用例验证同步显示流程，关闭异步精修以免触发真实 LLM 调用
    config.device_theme_colors["5A74"] = OverlayThemeColor::kBlue;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [&](const AppConfig&) {
            auto asr = std::make_unique<FakeAsrClient>();
            subtitle_asrs.push_back(asr.get());
            return asr;
        });
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 12));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(12, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 12));
    assert(subtitle_asrs.size() == 1);
    assert(!subtitle_asrs[0]->started);
    assert(HasUiState(*ble_ptr, "ready", "5A74"));

    const auto ready_count_after_first_release = std::count_if(
        ble_ptr->sent_ui_states.begin(), ble_ptr->sent_ui_states.end(), [](const SentUiState& sent) {
            return sent.state == "ready" && sent.device_id == std::optional<std::string>("5A74");
        });

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 13));
    assert(subtitle_asrs.size() == 2);
    assert(ble_ptr->sent_ui_states.back().state == "recording");

    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(12, 2));
    assert(subtitle_asrs[0]->started);
    subtitle_asrs[0]->on_final("first late subtitle");
    assert(!ui.subtitles.empty());
    assert(ui.subtitles.back() == "5A74:first late subtitle:blue");
    const auto ready_count_after_late_final = std::count_if(
        ble_ptr->sent_ui_states.begin(), ble_ptr->sent_ui_states.end(), [](const SentUiState& sent) {
            return sent.state == "ready" && sent.device_id == std::optional<std::string>("5A74");
        });
    assert(ready_count_after_late_final == ready_count_after_first_release);
}

void TestCoordinatorShortSubtitleEndReturnsReady() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.interaction_mode = InteractionMode::kHoldToTalk;
    VoiceStickCoordinator coordinator(
        config,
        std::move(ble),
        std::move(primary_asr),
        &ui,
        &input,
        [](const AppConfig&) {
            return std::make_unique<FakeAsrClient>();
        });
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 12));
    assert(ble_ptr->sent_ui_states.back().state == "recording");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(12, 1));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 12));

    assert(ui.hide_overlay_count > 0);
    assert(!ui.statuses.empty());
    assert(ui.statuses.back() == "Ready");
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

void TestCoordinatorClickToTalkPrimaryClickTogglesRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.interaction_mode = InteractionMode::kClickToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 21));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(21, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 21));

    assert(!asr_ptr->started);
    assert(ble_ptr->sent_ui_states.back().state == "thinking");

    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(21, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);
}

// click_to_talk 消歧回归：固件启动 click 发 duration_ms=0、停止 click 发 >0（均带
// session_id）。无活跃会话时收到停止 click 属于失步残留（典型：识别中启动 click 被忽略、
// 固件仍在录音，桌面回 ready 后停止 click 才到），必须忽略；否则启动永远收不到音频的
// 幽灵会话，数秒后经停滞看门狗以 "No audio frames from device" 报错。
void TestCoordinatorClickToTalkIgnoresStrayStopClick() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.interaction_mode = InteractionMode::kClickToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 99, 3000));
    assert(ui.show_listening_count == 0);
    assert(!HasUiState(*ble_ptr, "recording", "5A74"));
}

// 完整失步链路：识别中（finalizing）点击启动被桌面忽略但固件已在录音 → ASR final 后桌面
// 回 ready → 停止 click 到达。该 click 不得误判为启动新会话。
void TestCoordinatorClickToTalkStaleStopClickAfterFinalizingIgnored() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.interaction_mode = InteractionMode::kClickToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 会话 50：正常启动、说话、停止 → 等 audio_end（finalizing）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 50, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(50, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 50, 3000));
    assert(ble_ptr->sent_ui_states.back().state == "thinking");
    assert(ui.show_listening_count == 1);

    // finalizing 期间点击启动会话 51：桌面忽略（设备屏 thinking），固件实际在录音。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 51, 0));
    assert(ui.show_listening_count == 1);
    assert(ble_ptr->sent_ui_states.back().state == "thinking");

    // 会话 50 收尾：audio_end → ASR final → 粘贴回 ready。
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(50, 2));
    assert(asr_ptr->started);
    asr_ptr->on_final("hello");
    assert(input.pasted_text == "hello");
    assert(HasUiState(*ble_ptr, "ready", "5A74"));

    // 会话 51 的停止 click 迟到：不得启动幽灵会话。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 51, 4200));
    assert(ui.show_listening_count == 1);
}

// 停止 click 的 session_id 与活跃会话不匹配时不得停止当前会话：固件录音中只对本次会话
// 发停止 click，不匹配说明该 click 属于另一个已结束会话（失步残留）。
void TestCoordinatorClickToTalkStopClickSessionMismatchIgnored() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.interaction_mode = InteractionMode::kClickToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 70, 0));
    assert(ui.show_listening_count == 1);
    assert(ble_ptr->sent_ui_states.back().state == "recording");

    // 不匹配的停止 click：当前会话 70 保持录音，不被误停。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 71, 3000));
    assert(ui.hide_overlay_count == 0);
    assert(ble_ptr->sent_ui_states.back().state == "recording");
}

// 录音中收到新启动 click（旧会话停止 click 丢失的失步自愈）：先取消残留旧会话，
// 再以新 session_id 启动新会话，与 button_down 路径的残留自愈一致。
void TestCoordinatorClickToTalkStartClickDuringRecordingStartsNew() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.interaction_mode = InteractionMode::kClickToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 80, 0));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(80, 1));
    assert(ui.show_listening_count == 1);

    // 固件已结束 80 并开启 81（其停止 click 丢失）：启动 click(81) 自愈切换到新会话。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 81, 0));
    assert(ui.show_listening_count == 2);
    assert(ble_ptr->sent_ui_states.back().state == "recording");

    // 新会话正常工作：音频帧被接受，匹配停止 click 正常结束。
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(81, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 81, 3000));
    assert(ble_ptr->sent_ui_states.back().state == "thinking");
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(81, 2));
    assert(asr_ptr->started);
    assert(asr_ptr->last_chunk_was_final);
}

void TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 22));
    asr_ptr->on_partial("early");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "early");
    assert(ble_ptr->sent_ui_states.back().text != "early");

    ble_ptr->on_audio_frame("5A74", AudioDataFrame(22, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 22));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(22, 2));
    asr_ptr->on_partial("late");

    assert(ble_ptr->sent_ui_states.back().state == "thinking");
    assert(ble_ptr->sent_ui_states.back().text == "late");
}

void TestCoordinatorShowsDetailedAsrStartError() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    asr_ptr->start_result = false;
    asr_ptr->start_error = "Missing ASR API key";
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 23));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(23, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 23));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(23, 2));

    assert(!ui.errors.empty());
    assert(ui.errors.back() == "Missing ASR API key");
    assert(HasUiStateText(*ble_ptr, "error", "Missing ASR API key", "5A74"));
}

// 系统休眠/恢复后 ASR 保活 WebSocket 底层 TCP 已断，但 AsrClientWin 状态机仍认为
// kReady。平台层在 WM_POWERBROADCAST resume 时调 InvalidateAsrConnection() 通知
// 协调器丢弃保活连接，下次 Start 强制重新握手。本测试验证协调器正确转发给 asr_。
void TestCoordinatorInvalidateAsrConnectionForwardsToClient() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    assert(asr_ptr->invalidate_call_count == 0);
    coordinator.InvalidateAsrConnection();
    assert(asr_ptr->invalidate_call_count == 1);
}


// Suite entry: core_tests.cc main() calls this once.
// r121：Windows 侧连接事件链（对齐 mac r116 范式）——Start 装配 → 直调捕获回调 →
// FakeUi 观察（连接即状态刷新；配对表空 = Pair a VoiceStick 语义，非 mac 的 Ready）。
void TestCoordinatorConnectionChangeChain() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                       std::make_unique<FakeAsrClient>(), &ui, &input);
    coordinator.Start();

    ble_ptr->on_connection_change(std::vector<ConnectedDevice>{
        ConnectedDevice{"5A74", "VS-TEST", ""}});

    assert(ui.connected_devices.size() == 1);
    assert(ui.connected_devices[0].id == "5A74");
    assert(!ui.statuses.empty());
    assert(ui.statuses.back() == "Pair a VoiceStick");
    printf("TestCoordinatorConnectionChangeChain passed\n");
}
void RunCoordinatorBatchTests() {
    TestCoordinatorConnectionChangeChain();
    TestCoordinatorCancelsShortPrimaryPress();
    TestCoordinatorPrimaryDuringFinalizingRefreshesThinking();
    TestCoordinatorSecondaryCancelsFinalizing();
    TestCoordinatorAcceptsAudioFramesAfterButtonUpUntilEnd();
    TestCoordinatorDisconnectAwaitingAsrFinalKeepsSession();
    TestCoordinatorDisconnectDuringRecordingCancelsSession();
    TestCoordinatorMainFinalPastesWithoutConfirmation();
    TestCoordinatorRefineShowsOriginalTextImmediately();
    TestCoordinatorOtherDeviceDuringRecordingGetsReady();
    TestCoordinatorSubtitleOutputSkipsPaste();
    TestCoordinatorSubtitleFinalDoesNotBlockNextSession();
    TestCoordinatorShortSubtitleEndReturnsReady();
    TestCoordinatorClickToTalkPrimaryClickTogglesRecording();
    TestCoordinatorClickToTalkIgnoresStrayStopClick();
    TestCoordinatorClickToTalkStaleStopClickAfterFinalizingIgnored();
    TestCoordinatorClickToTalkStopClickSessionMismatchIgnored();
    TestCoordinatorClickToTalkStartClickDuringRecordingStartsNew();
    TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio();
    TestCoordinatorShowsDetailedAsrStartError();
    TestCoordinatorInvalidateAsrConnectionForwardsToClient();
}

