// N8 cut9: coordinator batch 6 (LocalMic suite) extracted from core_tests.cc
// (8 tests). Pre-checks: col0 non-test heads = 0, cross-span refs = 0.
#include "test_support.h"

void TestCoordinatorLocalMicSessionRoutesToLocalAsr() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    auto* cloud_asr_ptr = cloud_asr.get();
    auto local_asr = std::make_unique<FakeAsrClient>();
    auto* local_asr_ptr = local_asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.local_asr.enabled = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                      &ui, &input);
    auto capture = std::make_unique<FakeMicCapture>();
    auto* capture_ptr = capture.get();
    coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
    coordinator.Start();

    coordinator.HandleLocalMicHotkeyPressed();
    assert(capture_ptr->start_count == 1);
    assert(ui.show_listening_count == 1);
    assert(HasUiState(*ble_ptr, "recording", "local-mic"));

    // 喂 1 秒 16kHz PCM（模拟采集线程回调），等待会话时长跨过最短录音阈值后
    // 再喂一段：第二段触发 can_start_asr（时长 >= 0.5s）启动本地 ASR 并冲刷缓冲。
    const std::vector<std::int16_t> pcm(16000, 1200);
    capture_ptr->on_pcm(pcm);
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    capture_ptr->on_pcm(pcm);

    coordinator.HandleLocalMicHotkeyReleased();
    assert(capture_ptr->stop_count == 1);
    assert(local_asr_ptr->started);
    assert(local_asr_ptr->sent_chunks >= 1);
    assert(local_asr_ptr->last_chunk_was_final);
    // 路由断言：本会话音频只进本地 ASR，云端客户端全程未启动。
    assert(!cloud_asr_ptr->started);

    local_asr_ptr->on_final("本地识别结果");
    assert(input.pasted_text == "本地识别结果");
    assert(ui.hide_overlay_count == 1);
}

// 离线授权闸（Doc/Plan/offline-license-activation.md）：闸返回 false 时，主键
// 本地分支与 local-mic 热键均不下发本地会话（通知用户后放弃）；闸空（默认）放行，
// 既有测试不受影响。
void TestCoordinatorLicenseGate() {
    // 场景 1：设备主键 + [local_asr].enabled（会话将路由本地引擎）→ 被闸拦下。
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto* ble_ptr = ble.get();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto local_asr = std::make_unique<FakeAsrClient>();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto capture = std::make_unique<FakeMicCapture>();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.SetLicenseGate([] { return false; });
        coordinator.Start();

        ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 42));
        assert(ui.show_listening_count == 0);
        assert(!HasUiState(*ble_ptr, "recording", "5A74"));
        assert(ui.timed_messages.size() == 1);
        assert(ui.timed_messages[0] ==
               Tr(StringId::kLicenseLocalBlocked, EffectiveUiLanguage(config.ui_language)) +
                   ":3000");

        // 闸放行后恢复本地会话下发。
        coordinator.SetLicenseGate([] { return true; });
        ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 43));
        assert(ui.show_listening_count == 1);
        assert(HasUiState(*ble_ptr, "recording", "5A74"));
    }
    // 场景 2：local-mic 热键被闸拦下（不启动采集、无本地会话）。
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto local_asr = std::make_unique<FakeAsrClient>();
        auto* local_asr_ptr = local_asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto capture = std::make_unique<FakeMicCapture>();
        auto* capture_ptr = capture.get();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.SetLicenseGate([] { return false; });
        coordinator.Start();

        coordinator.HandleLocalMicHotkeyPressed();
        assert(capture_ptr->start_count == 0);
        assert(ui.show_listening_count == 0);
        assert(!local_asr_ptr->started);
        assert(ui.timed_messages.size() == 1);

        // 闸=true：热键正常启动本地会话。
        coordinator.SetLicenseGate([] { return true; });
        coordinator.HandleLocalMicHotkeyPressed();
        assert(capture_ptr->start_count == 1);
        assert(ui.show_listening_count == 1);
        coordinator.HandleLocalMicHotkeyReleased();
    }
}

// 本地识别会话钉住本地精修：final 文本过本地三层（规则 → LLM → 守卫），
// 注入守卫放行的 LLM 结果；引擎失败回退规则级文本（最差不劣于规则）。
// 云端 refine 保持关闭：本用例同时验证钉住互斥——本地会话不触发云端精修。
void TestCoordinatorLocalMicSessionRefinesFinalText() {
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        int chat_calls = 0;
        std::string last_user;
        bool Chat(const std::string&, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            ++chat_calls;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    // 子场景 1：LLM 结果过守卫，注入精修后文本
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto* cloud_asr_ptr = cloud_asr.get();
        auto local_asr = std::make_unique<FakeAsrClient>();
        auto* local_asr_ptr = local_asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        config.local_asr.refine_enabled = true;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto engine = std::make_unique<FakeEngine>();
        engine->reply = "帮我把这个文件重命名一下。";
        FakeEngine* engine_ptr = engine.get();
        coordinator.SetLocalRefiner(
            std::make_unique<LocalRefinementClient>(std::move(engine)));
        auto capture = std::make_unique<FakeMicCapture>();
        auto* capture_ptr = capture.get();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.Start();

        coordinator.HandleLocalMicHotkeyPressed();
        const std::vector<std::int16_t> pcm(16000, 1200);
        capture_ptr->on_pcm(pcm);
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        capture_ptr->on_pcm(pcm);
        coordinator.HandleLocalMicHotkeyReleased();
        assert(local_asr_ptr->started);
        assert(!cloud_asr_ptr->started);

        local_asr_ptr->on_final("嗯，帮我把这个文件重命名一下。");
        // 本地精修在 client 内部线程异步完成：轮询等待注入（对齐云端精修
        // 测试的等待模式）。
        for (int i = 0; i < 250 && input.pasted_text.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(input.pasted_text == "帮我把这个文件重命名一下。");
        assert(engine_ptr->chat_calls == 1);
        // L1 规则先滤句首语气词，喂给 LLM 的已是规则级文本。
        assert(engine_ptr->last_user.find("输入：帮我把这个文件重命名一下") !=
               std::string::npos);
    }
    // 子场景 2：引擎失败回退规则级文本
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto local_asr = std::make_unique<FakeAsrClient>();
        auto* local_asr_ptr = local_asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        config.local_asr.refine_enabled = true;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto engine = std::make_unique<FakeEngine>();
        engine->fail = true;
        coordinator.SetLocalRefiner(
            std::make_unique<LocalRefinementClient>(std::move(engine)));
        auto capture = std::make_unique<FakeMicCapture>();
        auto* capture_ptr = capture.get();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.Start();

        coordinator.HandleLocalMicHotkeyPressed();
        const std::vector<std::int16_t> pcm(16000, 1200);
        capture_ptr->on_pcm(pcm);
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        capture_ptr->on_pcm(pcm);
        coordinator.HandleLocalMicHotkeyReleased();

        local_asr_ptr->on_final("嗯，帮我打开浏览器。");
        for (int i = 0; i < 250 && input.pasted_text.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(input.pasted_text == "帮我打开浏览器。");
    }
    // 子场景 3：refine_enabled=true 但未注入 refiner——退化为原文直通
    //（模型未就绪/未下载时不阻塞本地语音输入）。
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto local_asr = std::make_unique<FakeAsrClient>();
        auto* local_asr_ptr = local_asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        config.local_asr.refine_enabled = true;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto capture = std::make_unique<FakeMicCapture>();
        auto* capture_ptr = capture.get();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.Start();

        coordinator.HandleLocalMicHotkeyPressed();
        const std::vector<std::int16_t> pcm(16000, 1200);
        capture_ptr->on_pcm(pcm);
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        capture_ptr->on_pcm(pcm);
        coordinator.HandleLocalMicHotkeyReleased();

        local_asr_ptr->on_final("嗯，没有精修引擎的原文");
        assert(input.pasted_text == "嗯，没有精修引擎的原文");
    }
}

// 跨轮上下文纠错接线（M3，方案 §3.4/3.5.5）：refine_cross_turn 开关开时，
// 本地精修 final 轮次进协调器 RefineHistory（raw=ASR 原句，refined=守卫后
// 文本，instruction=当轮模型指令），下一轮引擎 user 携带续写块（与 KV 重放
// 同构形态）；开关关（默认）两轮互不可见，维持单句 few-shot 管线。
void TestCoordinatorLocalMicSessionCrossTurnRefinement() {
    class ScriptedEngine : public LocalLlmEngine {
    public:
        std::vector<std::string> replies;  // 第 k 次 Chat 的回复脚本
        std::vector<std::string> users;    // 每次 Chat 收到的完整 user
        std::vector<std::string> systems;
        int idx = 0;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            systems.push_back(system_prompt);
            users.push_back(user_text);
            const std::string reply =
                idx < static_cast<int>(replies.size()) ? replies[idx] : "无";
            ++idx;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct TwoTurnResult {
        std::string pasted1;
        std::string pasted2;
        std::vector<std::string> users;
        std::vector<std::string> systems;
    };
    // 两轮本机麦克风会话（同一协调器实例）：轮 1 建立词汇，轮 2 复刻
    // C01 真机错字案例。cross_turn 决定配置开关；replies 为两轮模型脚本。
    auto run_two_turns = [](bool cross_turn,
                            std::vector<std::string> replies) {
        auto ble = std::make_unique<FakeBleCentral>();
        auto cloud_asr = std::make_unique<FakeAsrClient>();
        auto local_asr = std::make_unique<FakeAsrClient>();
        auto* local_asr_ptr = local_asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.local_asr.enabled = true;
        config.local_asr.refine_enabled = true;
        config.local_asr.refine_cross_turn = cross_turn;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                          &ui, &input);
        auto engine = std::make_unique<ScriptedEngine>();
        engine->replies = std::move(replies);
        ScriptedEngine* engine_ptr = engine.get();
        coordinator.SetLocalRefiner(
            std::make_unique<LocalRefinementClient>(std::move(engine)));
        auto capture = std::make_unique<FakeMicCapture>();
        auto* capture_ptr = capture.get();
        coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
        coordinator.Start();

        const auto feed_session = [&](const char* final_text) {
            input.pasted_text.clear();
            coordinator.HandleLocalMicHotkeyPressed();
            const std::vector<std::int16_t> pcm(16000, 1200);
            capture_ptr->on_pcm(pcm);
            std::this_thread::sleep_for(std::chrono::milliseconds(520));
            capture_ptr->on_pcm(pcm);
            coordinator.HandleLocalMicHotkeyReleased();
            local_asr_ptr->on_final(final_text);
            for (int i = 0; i < 250 && input.pasted_text.empty(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
        feed_session("嗯，我们刚才测了语气词过滤。");
        const std::string pasted1 = input.pasted_text;
        feed_session("那些鱼器渍已经被过滤掉了。");
        TwoTurnResult r;
        r.pasted1 = std::move(pasted1);
        r.pasted2 = input.pasted_text;
        r.users = engine_ptr->users;
        r.systems = engine_ptr->systems;
        return r;
    };
    {   // 场景 A：开关开——轮 2 走纠正指令管线（system 切换 + 续写块携带
        //     轮 1 的 raw 原句与当轮指令「无」），守卫放行纠正
        const auto r = run_two_turns(true, {"无", "鱼器渍→语气词"});
        assert(r.pasted1 == "我们刚才测了语气词过滤。");  // 规则删「嗯，」
        assert(r.pasted2 == "那些语气词已经被过滤掉了。");
        assert(r.users.size() == 2);
        assert(r.systems[1].find("参考上文") != std::string::npos);
        assert(r.users[1].find(
                   "输入：嗯，我们刚才测了语气词过滤。\n处理：无\n") !=
               std::string::npos);
        assert(r.users[1].find("输入：那些鱼器渍已经被过滤掉了。\n处理：") !=
               std::string::npos);
        // 轮 1 无历史：单句续写（cross 管线、空历史也不带续写块）
        assert(r.systems[0].find("参考上文") != std::string::npos);
        assert(r.users[0].find("输入：我们刚才测了语气词过滤。\n处理：") !=
               std::string::npos);
    }
    {   // 场景 B：开关关（默认）——两轮独立 few-shot 管线，无续写块
        const auto r = run_two_turns(
            false, {"无", "那些鱼器渍已经被过滤掉了。"});
        assert(r.pasted1 == "我们刚才测了语气词过滤。");
        assert(r.pasted2 == "那些鱼器渍已经被过滤掉了。");
        assert(r.users.size() == 2);
        assert(r.systems[1].find("参考上文") == std::string::npos);
        assert(r.users[1].find("处理：") == std::string::npos);
        assert(r.users[1].find("输出：") != std::string::npos);
    }
    printf("TestCoordinatorLocalMicSessionCrossTurnRefinement passed\n");
}

// 选中本地语音识别（local_asr.enabled）时，设备会话（遥控器语音键/全局热键触发的
// 设备录音）也必须路由到本地 SenseVoice——断网场景下设备语音可用，云端客户端零启动。
void TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    auto* cloud_asr_ptr = cloud_asr.get();
    auto local_asr = std::make_unique<FakeAsrClient>();
    auto* local_asr_ptr = local_asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.local_asr.enabled = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                      &ui, &input);
    auto capture = std::make_unique<FakeMicCapture>();
    auto* capture_ptr = capture.get();
    coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
    coordinator.Start();

    const std::string kDev = "RC-9F0E";
    XiaomiAtvvSession session;  // 默认 hold_to_talk
    std::int64_t t = 1000;
    AtvvCoordinatorHandshake(session, t);

    AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
    assert(HasUiState(*ble_ptr, "recording", kDev));

    // 跨过 0.5s 最小录音时长（墙钟），期间持续出音频帧。
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    InjectAtvvActions(*ble_ptr, kDev, session.HandleAudioData(ByteVector(480, 0x11), t + 100));
    AtvvEndRecording(*ble_ptr, kDev, session, t);

    // 路由断言：设备会话音频只进本地 ASR，云端客户端全程未启动（断网可用）。
    assert(local_asr_ptr->started);
    assert(local_asr_ptr->sent_chunks > 0);
    assert(local_asr_ptr->last_chunk_was_final);
    assert(!cloud_asr_ptr->started);
    // 本机麦克风采集器与设备会话无关，不应被启动。
    assert(capture_ptr->start_count == 0);

    local_asr_ptr->on_final("设备本地识别");
    assert(input.pasted_text == "设备本地识别");
    assert(HasUiState(*ble_ptr, "ready", kDev));
}

// 短按（<0.5s）：释放后按短按取消路径丢弃会话，不启动 ASR、不注入。
void TestCoordinatorLocalMicShortPressDiscards() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    auto local_asr = std::make_unique<FakeAsrClient>();
    auto* local_asr_ptr = local_asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.local_asr.enabled = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                      &ui, &input);
    auto capture = std::make_unique<FakeMicCapture>();
    auto* capture_ptr = capture.get();
    coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
    coordinator.Start();

    coordinator.HandleLocalMicHotkeyPressed();
    capture_ptr->on_pcm(std::vector<std::int16_t>(3200, 100));  // 200ms
    coordinator.HandleLocalMicHotkeyReleased();

    assert(capture_ptr->stop_count == 1);
    assert(!local_asr_ptr->started);
    assert(local_asr_ptr->cancelled);
    assert(ui.hide_overlay_count == 1);
    assert(input.pasted_text.empty());
}

// 配置关闭时热键完全旁路：不建会话、不启动采集；释放也不得有副作用。
void TestCoordinatorLocalMicDisabledDoesNothing() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    auto local_asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                      std::move(cloud_asr), &ui, &input);
    auto capture = std::make_unique<FakeMicCapture>();
    auto* capture_ptr = capture.get();
    coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
    coordinator.Start();

    coordinator.HandleLocalMicHotkeyPressed();
    assert(capture_ptr->start_count == 0);
    assert(ui.show_listening_count == 0);

    coordinator.HandleLocalMicHotkeyReleased();
    assert(capture_ptr->stop_count == 0);
}

// 采集启动失败（无麦克风/设备被占用）：会话立即按取消路径收尾并给出用户可见提示，
// 后续 PCM 全部被会话校验丢弃。
void TestCoordinatorLocalMicCaptureStartFailureCancelsSession() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    auto local_asr = std::make_unique<FakeAsrClient>();
    auto* local_asr_ptr = local_asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.local_asr.enabled = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(cloud_asr),
                                      &ui, &input);
    auto capture = std::make_unique<FakeMicCapture>();
    auto* capture_ptr = capture.get();
    capture_ptr->start_result = false;
    capture_ptr->start_error = "no microphone";
    coordinator.SetLocalMicRuntime(std::move(capture), std::move(local_asr));
    coordinator.Start();

    coordinator.HandleLocalMicHotkeyPressed();
    assert(capture_ptr->start_count == 1);
    assert(ui.show_listening_count == 1);
    assert(ui.hide_overlay_count == 1);          // 取消路径收起浮窗
    assert(!ui.timed_messages.empty());          // 用户可见失败提示

    capture_ptr->on_pcm(std::vector<std::int16_t>(640, 100));
    coordinator.HandleLocalMicHotkeyReleased();
    assert(!local_asr_ptr->started);
    assert(input.pasted_text.empty());
}

// 真实 WASAPI 采集冒烟：默认麦克风采集 2 秒应至少回调一帧 PCM；无麦克风/被占用
// 时如实报告跳过原因（不伪造通过）。

// Suite entry: core_tests.cc main() calls this once.
void RunCoordinatorBatch6Tests() {
    TestCoordinatorLocalMicSessionRoutesToLocalAsr();
    TestCoordinatorLicenseGate();
    TestCoordinatorLocalMicSessionRefinesFinalText();
    TestCoordinatorLocalMicSessionCrossTurnRefinement();
    TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled();
    TestCoordinatorLocalMicShortPressDiscards();
    TestCoordinatorLocalMicDisabledDoesNothing();
    TestCoordinatorLocalMicCaptureStartFailureCancelsSession();
}

