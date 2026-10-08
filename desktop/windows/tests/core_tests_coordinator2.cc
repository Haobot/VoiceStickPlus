// N8 cut4: coordinator batch 2 extracted from core_tests.cc (17 tests).
// Depends only on test_support.h (fakes + builders).
#include "test_support.h"

void TestCoordinatorAirMouseToggleViaSecondary() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->sent_air_mouse_enabled.clear();

    // 空闲态侧键单击 → 无操作：不进入体感，无 air_mouse_enabled 下发、无 air_mouse ui_state。
    ble_ptr->sent_ui_states.clear();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(ble_ptr->sent_air_mouse_enabled.empty());
    assert(!HasUiState(*ble_ptr, "air_mouse", "5A74"));

    // 直接切换进入体感，下发 air_mouse_enabled:true + ui_state:air_mouse。
    // ui_state=air_mouse 让设备显示体感态提示，避免用户不知情下主键变鼠标左键。
    ble_ptr->sent_ui_states.clear();
    coordinator.ToggleAirMouse("5A74");
    assert(!ble_ptr->sent_air_mouse_enabled.empty());
    assert(ble_ptr->sent_air_mouse_enabled.back().first == true);
    assert(HasUiState(*ble_ptr, "air_mouse", "5A74"));

    // 体感态下侧键单击 → 退出体感，下发 air_mouse_enabled:false + ui_state:ready。
    ble_ptr->sent_ui_states.clear();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(ble_ptr->sent_air_mouse_enabled.back().first == false);
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
}

// 体感态下主键单击映射为鼠标左键，不启动录音。
void TestCoordinatorAirMousePrimaryClickIsLeftButton() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感态。
    coordinator.ToggleAirMouse("5A74");

    // 主键单击 → 左键点击，不启动 ASR/录音。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary", 5));
    assert(input.left_click_count == 1);
    assert(!asr_ptr->started);
    assert(ui.show_listening_count == 0);
}

// 体感态下 motion 帧只更新 omega，不直接注入；由 AirMouseTick 驱动光标移动。非体感态忽略。
void TestCoordinatorMotionMovesCursorOnlyWhenActive() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 未进入体感态时 motion 应被忽略。
    ble_ptr->on_motion_event("5A74", MotionEvent{10, -5});
    assert(input.move_mouse_count == 0);

    // 进入体感态后 motion 不直接注入（由 AirMouseTick 驱动）。
    coordinator.ToggleAirMouse("5A74");
    ble_ptr->on_motion_event("5A74", MotionEvent{100, 0});
    assert(input.move_mouse_count == 0);

    // AirMouseTick 驱动速度控制；v 累积后注入光标位移。
    for (int i = 0; i < 20; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{100, 0});
        coordinator.AirMouseTick();
    }
    assert(input.move_mouse_count >= 1);
}

// AirMouseTick 驱动速度环：进入体感 + motion 后，tick 产生非零位移。
void TestCoordinatorAirMouseTickMovesCursor() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");
    for (int i = 0; i < 20; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{100, 0});
        coordinator.AirMouseTick();
    }
    assert(input.move_mouse_count >= 1);
    assert(input.total_dx > 0);  // omega_x=100 正向，位移为正
}

// 退出再进入体感，速度状态复位（v 从 0 开始，无残留漂移）。
void TestCoordinatorAirMouseStateResetOnToggle() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入并累积角度。
    coordinator.ToggleAirMouse("5A74");
    for (int i = 0; i < 20; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{100, 0});
        coordinator.AirMouseTick();
    }
    assert(input.move_mouse_count >= 1);

    // 退出再进入：状态应复位。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    coordinator.ToggleAirMouse("5A74");
    const int count_before = input.move_mouse_count;
    // 无 motion，立即 tick：v=0、omega=0，应产生零位移（不调 MoveMouse）。
    coordinator.AirMouseTick();
    assert(input.move_mouse_count == count_before);
}

// 进入/退出体感触发 on_air_mouse_active_changed 回调。
void TestCoordinatorAirMouseActiveChangedCallback() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    bool callback_called = false;
    bool last_active = false;
    coordinator.on_air_mouse_active_changed = [&](bool active) {
        callback_called = true;
        last_active = active;
    };
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感 → 回调 true。
    coordinator.ToggleAirMouse("5A74");
    assert(callback_called);
    assert(last_active);

    // 退出 → 回调 false（体感态下侧键单击退出）。
    callback_called = false;
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(callback_called);
    assert(!last_active);
}

// 体感态下主键长按（button_down）不启动录音，tap 事件被忽略。
void TestCoordinatorAirMouseGatesRecordingAndTap() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感态。
    coordinator.ToggleAirMouse("5A74");

    // 主键按下不启动录音。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));
    assert(ui.show_listening_count == 0);
    assert(!asr_ptr->started);

    // tap 被忽略。
    ble_ptr->on_state_event("5A74", TapEvent("double"));
    assert(input.arrow_down_count == 0);
}

// 设备断连时清理体感态，避免残留激活拦截重连后的主键录音。
void TestCoordinatorAirMouseResetOnDisconnect() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    bool last_active = false;
    coordinator.on_air_mouse_active_changed = [&](bool active) { last_active = active; };
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感态 → 回调 true。
    coordinator.ToggleAirMouse("5A74");
    assert(last_active);

    // 断连 → 体感态必须清理（回调 false），否则残留激活会吞掉后续主键录音。
    ble_ptr->connected_device_ids.erase("5A74");
    ble_ptr->on_connection_change({});
    assert(!last_active);

    // 重连后主键按下应启动录音（体感已清，不再被拦截）。
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->sent_air_mouse_enabled.clear();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));
    assert(ui.show_listening_count >= 1);
    // 重连后不应残留体感下发。
    assert(ble_ptr->sent_air_mouse_enabled.empty());
}

// forget 设备时清理体感态，避免残留拦截重连后的主键录音。
void TestCoordinatorAirMouseResetOnForget() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids = {"5A74"};
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    bool last_active = false;
    coordinator.on_air_mouse_active_changed = [&](bool active) { last_active = active; };
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");
    assert(last_active);

    // forget → 体感态必须清理（回调 false + 下发 false 通知固件停表）。
    coordinator.RemovePairedDevice("5A74");
    assert(!last_active);
    assert(!ble_ptr->sent_air_mouse_enabled.empty());
    assert(ble_ptr->sent_air_mouse_enabled.back().first == false);
}

// 10 级灵敏度下，真机典型手腕角速率(omega=40dps，firmware dx=160 @ REPORT_GAIN=4)在 0.8s 内应产生足够光标位移。
// kAngle 模式现直接用瞬时 omega 驱动速度：v = omega×gain×factor(|omega|)，
// 转动期间即达到稳态速度，位移充足。
void TestCoordinatorAirMouseHighSensitivityRealisticSpeed() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 10;  // 最高档
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");
    for (int i = 0; i < 50; ++i) {  // 0.8s @60Hz
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});  // 真机典型手腕转动（40dps @ REPORT_GAIN=4）
        coordinator.AirMouseTick();
    }
    assert(input.total_dx >= 4000);  // 角度模型 theta 累积，0.8s 应产生足够位移
}

// P0 回归：kAngle 模式持续匀速转动时，光标速度应恒定（不随转动时长增长），即无失控。
// 旧实现把积分转角 theta 套入增益曲线，匀速转 3s 速度从万级飙到数十万 px/s。
void TestCoordinatorAirMouseSustainedRunBounded() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");
    // 阶段 1：匀速转动 0.5s（omega=40dps 恒定），光标达到稳态速度。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_first = input.total_dx;
    // 阶段 2：继续匀速转动 4.5s（omega=40dps 恒定）。速度应保持不变（无失控）。
    for (int i = 0; i < 270; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_second = input.total_dx - dx_first;
    const double v1 = static_cast<double>(dx_first) / 0.5;
    const double v2 = static_cast<double>(dx_second) / 4.5;
    // 两段时长不同(0.5s vs 4.5s)，但都是匀速转动：稳态速度应一致，v2 不应因转动更久而变大。
    // 允许速度环收敛/帧边界差异，但绝不应指数增长（旧 bug 下 v2 会是 v1 的数十倍）。
    assert(v2 <= v1 * 2.0);
    // 整体有界：5s 总位移不应离谱（旧 bug 会到数百万 px）。
    const double avg_speed = static_cast<double>(input.total_dx) / 5.0;
    assert(avg_speed <= 200000.0);
}

// 角度控制（kAngle）：停手（omega=0）后光标应在 tau 惯性滑行（≈0.15s）后彻底停止，
// 而非持续移动。等价于旧的"回正即停"。
void TestCoordinatorAirMouseStopsWhenOmegaZero() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");

    // 转动 0.5s：光标移动。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_during = input.total_dx;
    assert(dx_during > 0);

    // 停手：omega=0 持续 1.0s，光标应在惯性滑行后停止。
    for (int i = 0; i < 60; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    // 关键：再额外 0.5s 中立应几乎不动（确认已停，而非持续移动）。
    const int count_before_extra = input.total_dx;
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    const int dx_extra = input.total_dx - count_before_extra;
    assert(dx_extra < 500);  // 已停，额外位移极小
}

// kAngle 模式：光标仅在手腕转动（omega≠0）时移动；停转（omega=0）后应在惯性滑行后停止，
// 不再持续移动。这是 P0 修复的核心行为——旧实现"保持 theta 即持续移动"，会导致持续旋转失控。
void TestCoordinatorAngleMovesOnlyWhileRotating() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");

    // 阶段 1：转动 0.5s。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_while_moving = input.total_dx;
    assert(dx_while_moving > 0);

    // 阶段 2：omega=0 保持 0.5s，光标仅余 tau 惯性滑行（≈0.15s）后停止，不应持续移动。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    const int dx_while_holding = input.total_dx - dx_while_moving;
    // 保持阶段位移应远小于"等同时长持续转动"的贡献（即不是 sustained，仅是惯性滑行）。
    assert(dx_while_holding < dx_while_moving * 0.2);

    // 阶段 3：再 0.5s 中立，应基本不动（已停）。
    const int before_extra = input.total_dx;
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    const int dx_extra = input.total_dx - before_extra;
    assert(dx_extra < 500);
}

// P0 显式回归：匀速转动下，任意等长时段的位移应近似相等（速度恒定），
// 证明增益曲线不再对"累计转角"作用而正反馈失控。
void TestCoordinatorAirMouseSustainedRotationConstantSpeed() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.ToggleAirMouse("5A74");

    // 预热 0.3s 让速度环收敛到稳态。
    for (int i = 0; i < 18; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int base = input.total_dx;
    // 窗口 A：匀速转动 1.0s（omega=40dps 恒定）。
    for (int i = 0; i < 60; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_a = input.total_dx - base;
    // 窗口 B：继续匀速转动 1.0s（同样 omega=40dps）。
    for (int i = 0; i < 60; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{160, 0});
        coordinator.AirMouseTick();
    }
    const int dx_b = input.total_dx - base - dx_a;
    // 匀速转动时两窗口位移应近似相等（速度恒定，无增长/失控）。
    assert(dx_b > 0);
    assert(std::fabs(static_cast<double>(dx_b) - static_cast<double>(dx_a)) <=
           std::fabs(static_cast<double>(dx_a)) * 0.2);
}

// 侧键双击恢复上次输入确认（与单击进体感分离）。
void TestCoordinatorSecondaryDoubleClickRestoresLastInput() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 关闭异步精修，走同步粘贴以填充 last_recoverable_text_
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 先完成一次录音→final→粘贴，产生可恢复输入。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 9));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(9, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 9));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(9, 2));
    asr_ptr->on_final("hello");
    assert(input.pasted_text == "hello");

    // 空闲态侧键双击 → 恢复上次输入（ShowPausedFinal），不进入体感。
    ble_ptr->sent_air_mouse_enabled.clear();
    ble_ptr->on_state_event("5A74", DoubleClickEvent("secondary"));
    assert(!ui.paused_finals.empty());
    assert(ui.paused_finals.back() == "hello");
    assert(ble_ptr->sent_air_mouse_enabled.empty());  // 双击不触发体感
}

// 体感态下侧键双击被忽略（不恢复输入，避免冲突）。
void TestCoordinatorSecondaryDoubleClickIgnoredInAirMouse() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感态。
    coordinator.ToggleAirMouse("5A74");

    // 体感态下双击被忽略：无恢复、体感仍开启。
    ble_ptr->on_state_event("5A74", DoubleClickEvent("secondary"));
    assert(ui.paused_finals.empty());
    assert(ble_ptr->sent_air_mouse_enabled.back().first == true);
}

void TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 24));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(24, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 24));
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(24, 2));
    asr_ptr->on_error("ASR 44002: VoiceStick Cloud API key is invalid.");
    asr_ptr->on_upgrade_url("https://example.test/upgrade",
                            "ASR 44002: VoiceStick Cloud API key is invalid.");

    assert(HasUiStateText(*ble_ptr, "error", "ASR 44002: VoiceStick Cloud API key is invalid.", "5A74"));
    assert(HasUiState(*ble_ptr, "ready", "5A74"));
    assert(!ui.cloud_upgrades.empty());
}


// Suite entry: core_tests.cc main() calls this once.
void RunCoordinatorBatch2Tests() {
    TestCoordinatorAirMouseToggleViaSecondary();
    TestCoordinatorAirMousePrimaryClickIsLeftButton();
    TestCoordinatorMotionMovesCursorOnlyWhenActive();
    TestCoordinatorAirMouseTickMovesCursor();
    TestCoordinatorAirMouseStateResetOnToggle();
    TestCoordinatorAirMouseActiveChangedCallback();
    TestCoordinatorAirMouseGatesRecordingAndTap();
    TestCoordinatorAirMouseResetOnDisconnect();
    TestCoordinatorAirMouseResetOnForget();
    TestCoordinatorAirMouseHighSensitivityRealisticSpeed();
    TestCoordinatorAirMouseSustainedRunBounded();
    TestCoordinatorAirMouseStopsWhenOmegaZero();
    TestCoordinatorAngleMovesOnlyWhileRotating();
    TestCoordinatorAirMouseSustainedRotationConstantSpeed();
    TestCoordinatorSecondaryDoubleClickRestoresLastInput();
    TestCoordinatorSecondaryDoubleClickIgnoredInAirMouse();
    TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError();
}

