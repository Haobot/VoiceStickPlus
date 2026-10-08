// N8 cut12: device/input misc batch extracted from core_tests.cc (59 tests,
// the largest contiguous block: tap/serial/device/airmouse/pcm/... pure test
// bodies, no interleaved helpers — three-assertion pre-check all clean).
#include "test_support.h"

void TestTapEventInjectsArrowDown() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", TapEvent("double"));

    assert(input.arrow_down_count == 1);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

void TestTapDisabledWhenConfigOff() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = false;  // 总开关关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", TapEvent("double"));

    assert(input.arrow_down_count == 0);
}

void TestTapIgnoredDuringRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入录音态（hold_to_talk 默认，主键按下即录音）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));

    ble_ptr->on_state_event("5A74", TapEvent("double"));

    // 录音中 tap 应被忽略，不注入方向键，也不取消当前录音。
    assert(input.arrow_down_count == 0);
}

void TestTapThrottledWithin500ms() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 500ms 内连续两次 tap：第二次应被节流，只注入一次方向键。
    ble_ptr->on_state_event("5A74", TapEvent("double"));
    ble_ptr->on_state_event("5A74", TapEvent("double"));

    assert(input.arrow_down_count == 1);
}

void TestTapThrottleRecoversAfter500ms() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 第一次 tap 注入；间隔超过 500ms 后第二次 tap 应再次注入。
    ble_ptr->on_state_event("5A74", TapEvent("double"));
    assert(input.arrow_down_count == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_state_event("5A74", TapEvent("double"));

    assert(input.arrow_down_count == 2);
}

void TestInputInjectorArrowUpFakeWiring() {
    // Fake 直连验证 SendArrowUp 接线：协调器映射测试（Task 8）依赖此计数。
    FakeInputInjector input;
    input.SendArrowUp();
    assert(input.arrow_up_count == 1);
    assert(input.arrow_down_count == 0);
}

void TestInputInjectorKeyComboFakeWiring() {
    // Fake 直连验证 SendKeyCombo 接线：协调器路由测试（Task 9）依赖此记录。
    FakeInputInjector input;
    const auto spec = ParseKeySpec("ctrl+shift+v");
    assert(spec.has_value());
    input.SendKeyCombo(*spec);
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Shift+V");
}

void TestKeySpecParse() {
    // 单键：方向键/enter/单字符/f 键/音量键。
    auto down = ParseKeySpec("down");
    assert(down.has_value());
    assert(down->modifiers.empty());
    assert(down->vk == VK_DOWN);
    assert(down->display_text == "Down");

    auto up = ParseKeySpec("UP");  // 大小写不敏感
    assert(up.has_value() && up->vk == VK_UP);

    auto enter = ParseKeySpec(" enter ");  // 前后空白容忍
    assert(enter.has_value() && enter->vk == VK_RETURN && enter->display_text == "Enter");

    auto v = ParseKeySpec("v");
    assert(v.has_value() && v->vk == 'V' && v->display_text == "V");

    auto f5 = ParseKeySpec("f5");
    assert(f5.has_value() && f5->vk == VK_F5 && f5->display_text == "F5");

    auto vol = ParseKeySpec("volumeup");
    assert(vol.has_value() && vol->vk == VK_VOLUME_UP);

    auto pgdn = ParseKeySpec("pagedown");
    assert(pgdn.has_value() && pgdn->vk == VK_NEXT);

    // 修饰键组合：display_text 修饰键固定 Ctrl/Alt/Shift/Win 序。
    auto combo = ParseKeySpec("win+shift+ctrl+v");
    assert(combo.has_value());
    assert(combo->vk == 'V');
    assert(combo->modifiers.size() == 3);
    assert(combo->modifiers[0] == VK_CONTROL);
    assert(combo->modifiers[1] == VK_SHIFT);
    assert(combo->modifiers[2] == VK_LWIN);
    assert(combo->display_text == "Ctrl+Shift+Win+V");

    auto alt_f4 = ParseKeySpec("alt+f4");
    assert(alt_f4.has_value() && alt_f4->vk == VK_F4 && alt_f4->display_text == "Alt+F4");

    // 非法：未知键名、仅修饰键、空串、重复主键。
    assert(!ParseKeySpec("bogus").has_value());
    assert(!ParseKeySpec("ctrl").has_value());
    assert(!ParseKeySpec("").has_value());
    assert(!ParseKeySpec("ctrl+").has_value());
    assert(!ParseKeySpec("a+b").has_value());

    // 分支覆盖：重复修饰键、空中间 part、f 键边界、大写修饰键、纯数字主键、非 ASCII。
    assert(!ParseKeySpec("ctrl+ctrl+v").has_value());
    assert(!ParseKeySpec("ctrl++v").has_value());
    assert(!ParseKeySpec("f0").has_value());
    assert(!ParseKeySpec("f25").has_value());
    auto f24 = ParseKeySpec("f24");
    assert(f24.has_value() && f24->vk == VK_F24);
    auto ctrl_v = ParseKeySpec("CTRL+V");
    assert(ctrl_v.has_value() && ctrl_v->display_text == "Ctrl+V");
    auto digit5 = ParseKeySpec("5");
    assert(digit5.has_value() && digit5->vk == '5');
    assert(!ParseKeySpec("上").has_value());  // UTF-8 非 ASCII 输入不崩且拒绝
}

void TestKeySpecMakeFromVk() {
    // 单键无修饰键（按键映射场景）。
    const auto single = MakeKeySpecFromVk({}, VK_DOWN);
    assert(single.modifiers.empty());
    assert(single.vk == VK_DOWN);
    assert(single.display_text == "Down");

    // 乱序修饰键输入归一化为 Ctrl/Alt/Shift/Win 序；重复项去重（按集合语义）。
    const auto combo = MakeKeySpecFromVk({VK_LWIN, VK_SHIFT, VK_CONTROL}, 'V');
    assert(combo.vk == 'V');
    assert(combo.modifiers.size() == 3);
    assert(combo.modifiers[0] == VK_CONTROL);
    assert(combo.modifiers[1] == VK_SHIFT);
    assert(combo.modifiers[2] == VK_LWIN);
    assert(combo.display_text == "Ctrl+Shift+Win+V");

    // 不可识别 VK：display_text 退化 "VK0xXX" 大写十六进制，恒成功。
    const auto unknown = MakeKeySpecFromVk({}, 0x1F);
    assert(unknown.vk == 0x1F);
    assert(unknown.display_text == "VK0x1F");

    // 非白名单修饰键不进结果。
    const auto stray = MakeKeySpecFromVk({VK_CAPITAL, VK_MENU}, VK_F4);
    assert(stray.modifiers.size() == 1 && stray.modifiers[0] == VK_MENU);
    assert(stray.display_text == "Alt+F4");
}

void TestAppConfigEncoderRoundTrip() {
    // 默认值：旋转注入开、不翻转。
    assert(AppConfig::Defaults().default_encoder_settings.to_arrow == true);
    assert(AppConfig::Defaults().default_encoder_settings.rotation_invert == false);

    // TOML 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_config_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.to_arrow = false;
    config.default_encoder_settings.rotation_invert = true;
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_encoder_settings.to_arrow == false);
    assert(loaded.default_encoder_settings.rotation_invert == true);
    std::filesystem::remove(temp);
}

void TestAppConfigEncoderSettingsRoundTrip() {
    // 默认值等价当前硬编码行为。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.default_encoder_settings.rotate_cw_key == "down");
    assert(defaults.default_encoder_settings.rotate_ccw_key == "up");
    assert(defaults.default_encoder_settings.led_color == "red");
    assert(defaults.default_encoder_settings.press_action == "recording");
    assert(defaults.default_encoder_settings.press_key.empty());
    assert(defaults.default_encoder_settings.double_click_action == "key");
    assert(defaults.default_encoder_settings.double_click_key == "enter");

    // 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_settings_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_cw_key = "pageup";
    config.default_encoder_settings.rotate_ccw_key = "pagedown";
    config.default_encoder_settings.led_color = "cyan";
    config.default_encoder_settings.press_action = "key";
    config.default_encoder_settings.press_key = "ctrl+z";
    config.default_encoder_settings.double_click_action = "recording";
    config.default_encoder_settings.double_click_key = "ctrl+enter";
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_encoder_settings.rotate_cw_key == "pageup");
    assert(loaded.default_encoder_settings.rotate_ccw_key == "pagedown");
    assert(loaded.default_encoder_settings.led_color == "cyan");
    assert(loaded.default_encoder_settings.press_action == "key");
    assert(loaded.default_encoder_settings.press_key == "ctrl+z");
    assert(loaded.default_encoder_settings.double_click_action == "recording");
    assert(loaded.default_encoder_settings.double_click_key == "ctrl+enter");
    std::filesystem::remove(temp);
}

void TestAppConfigEncoderSettingsInvalidFallback() {
    // 非法值回退默认：未知 action/颜色/按键语法。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_invalid_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "encoder_rotate_cw_key = \"bogus\"\n";
        out << "encoder_led_color = \"pink\"\n";
        out << "encoder_press_action = \"fly\"\n";
        out << "encoder_press_key = \"ctrl+\"\n";
        out << "encoder_double_click_action = \"fly\"\n";
        out << "encoder_double_click_key = \"a+b\"\n";
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_encoder_settings.rotate_cw_key == "down");
    assert(loaded.default_encoder_settings.led_color == "red");
    assert(loaded.default_encoder_settings.press_action == "recording");
    assert(loaded.default_encoder_settings.press_key.empty());
    assert(loaded.default_encoder_settings.double_click_action == "key");
    assert(loaded.default_encoder_settings.double_click_key == "enter");
    std::filesystem::remove(temp);
}

void TestEncoderRotateMapsDirectionToArrows() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定，专注验证方向映射
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 默认映射：cw→Down、ccw→Up，每个 step 注入一次。默认配置 down/up 合法，
    // 走 SendKeyCombo 通道（与 SendArrowDown/Up 等价：同一 VK_DOWN/VK_UP 键事件）。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.sent_key_combos.size() == 3);
    assert(input.sent_key_combos[0] == "Down");
    assert(input.sent_key_combos[1] == "Down");
    assert(input.sent_key_combos[2] == "Up");
}

void TestEncoderRotateInvertFlipsDirection() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotation_invert = true;
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 翻转后：cw→Up、ccw→Down（同样走 SendKeyCombo 通道）。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.sent_key_combos.size() == 3);
    assert(input.sent_key_combos[0] == "Up");
    assert(input.sent_key_combos[1] == "Up");
    assert(input.sent_key_combos[2] == "Down");
}

void TestEncoderRotateDisabledWhenConfigOff() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.to_arrow = false;  // 总开关关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));

    assert(input.arrow_down_count == 0);
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateIgnoredDuringRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入录音态（hold_to_talk 默认，主键按下即录音）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));

    // 录音中旋转应被忽略，不注入方向键，也不取消当前录音。
    assert(input.arrow_down_count == 0);
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateUnknownDirectionTreatedAsCw() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 未知 direction（固件拼写错误/未来新值）兜底按 cw 处理（SendKeyCombo 通道）。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("up", 2));

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[0] == "Down");
    assert(input.sent_key_combos[1] == "Down");
}

void TestEncoderRotateStepsClamped() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    // 抬高快慢分档阈值，隔离快慢分档对注入按键的影响，专注验证 steps 钳制。
    config.default_encoder_settings.rotate_fast_threshold = 100000;
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 异常大步数应被钳到 kMaxEncoderRotateSteps=64，防伪造帧放大注入循环（SendKeyCombo 通道）。
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 300));

    assert(input.sent_key_combos.size() == 64);
    assert(std::all_of(input.sent_key_combos.begin(), input.sent_key_combos.end(),
                       [](const std::string& s) { return s == "Down"; }));
}

void TestEncoderPressRecordingStartsSession() {
    // 默认 press_action=recording：编码器 button_down 走主键路径启动录音会话。
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

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_down", 30));
    // 与物理主键 down 同一行为：进入录音态；ASR 与物理路径一致在首帧音频且
    // 录音时长过阈值后懒启动。
    assert(HasUiState(*ble_ptr, "recording", "5A74"));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(30, 1));
    assert(asr_ptr->started);
    assert(input.sent_key_combos.empty());
}

void TestEncoderPressKeyInjectsComboWithoutRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.press_action = "key";
    config.default_encoder_settings.press_key = "ctrl+z";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_click"));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Z");
    assert(!asr_ptr->started);  // 不录音
}

void TestEncoderPressKeyInvalidIgnored() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.press_action = "key";
    config.default_encoder_settings.press_key = "bogus";  // 运行期非法（绕过配置校验直造）
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_click"));
    assert(input.sent_key_combos.empty());  // 记日志忽略，不注入
}

void TestEncoderDoubleClickDefaultEnterCancelsSession() {
    // 双击默认 enter：取消活跃录音 + 注入 Enter（等价物理主键双击现行为）。
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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));  // 物理键开播
    std::this_thread::sleep_for(std::chrono::milliseconds(520));  // 过最短录音时长阈值
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(30, 1));  // 首帧音频触发 ASR 懒启动
    assert(asr_ptr->started);

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    // 合法配置键经 SendKeyCombo 注入（默认 enter 与 SendEnter 同为 VK_RETURN 按下/松开，
    // 物理行为等价），不再走 SendEnter 专用通道。
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Enter");
    assert(asr_ptr->cancelled);
}

void TestEncoderDoubleClickCustomKey() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.double_click_action = "key";
    config.default_encoder_settings.double_click_key = "ctrl+enter";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Enter");
    assert(!input.send_enter_called);  // 不再走 SendEnter
}

void TestEncoderDoubleClickRecordingTogglesRemoteButton() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.double_click_action = "recording";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 空闲双击 → remote down 开播。
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(!ble_ptr->sent_remote_buttons.empty());
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kDown);

    // 录音中双击 → remote up 停播。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));  // 过最短录音时长阈值
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(30, 1));  // 首帧音频触发 ASR 懒启动
    assert(asr_ptr->started);
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kUp);
}

void TestEncoderRotateCustomKeys() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_cw_key = "pagedown";
    config.default_encoder_settings.rotate_ccw_key = "pageup";
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    assert(input.sent_key_combos.size() == 3);
    assert(input.sent_key_combos[0] == "PageDown");
    assert(input.sent_key_combos[1] == "PageDown");
    assert(input.sent_key_combos[2] == "PageUp");
    assert(input.arrow_down_count == 0);  // 不再走硬编码方向键
}

void TestEncoderRotateCustomKeysPendingPath() {
    // 与 TestEncoderRotateCustomKeys 的区别：不关闭延迟判定（rotate_decide_window_ms
    // 保持默认 80），走 pending -> EncoderRotateTick -> FlushEncoderRotatePending 路径，
    // 验证 FlushEncoderRotatePending 重新读取 config 时能拿到自定义按键。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_cw_key = "pagedown";
    config.default_encoder_settings.rotate_ccw_key = "pageup";
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    // rotate_decide_window_ms 保持默认 80（pending 路径）
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));
    assert(input.sent_key_combos.empty());  // 窗内未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");  // 自定义按键，非默认 Down
    assert(input.arrow_down_count == 0);

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    assert(input.sent_key_combos.size() == 1);  // 窗内未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[1] == "PageUp");  // 自定义按键，非默认 Up
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateCustomKeysPendingPathDeviceOverride() {
    // 设备覆盖场景的 pending -> EncoderRotateTick -> FlushEncoderRotatePending 回归测试：
    // 冲刷必须使用该设备的 [device.<id>.encoder] 覆盖键而非全局默认。
    // 历史 bug：FlushEncoderRotatePending 参数以 const 引用绑定成员 encoder_pending_device_id_，
    // 函数体内 clear() 成员把参数引用的对象清空成空串，EncoderSettingsForDevice("") 查不到
    // 设备覆盖，回落全局默认 Down/Up——慢速旋转输出锁死在方向键。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    EncoderSettings override_settings;
    override_settings.rotate_cw_key = "pagedown";
    override_settings.rotate_ccw_key = "pageup";
    override_settings.rotate_fast_threshold = 100000;
    config.device_encoder_settings["5A74"] = override_settings;
    // rotate_decide_window_ms 保持默认 80（pending 路径）
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));
    assert(input.sent_key_combos.empty());  // 窗内未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");  // 设备覆盖键，非全局默认 Down
    assert(input.arrow_down_count == 0);

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    assert(input.sent_key_combos.size() == 1);  // 窗内未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[1] == "PageUp");  // 设备覆盖键，非全局默认 Up
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateCustomKeysAfterUpdateConfig() {
    // 模拟生产场景：协调器以默认配置启动，用户通过对话框 UpdateConfig 改旋转键，
    // 随后旋转编码器。验证 UpdateConfig 后 pending 路径使用新按键。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 先用默认配置旋转一次，确认默认键 Down
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Down");

    // UpdateConfig 改旋转键（模拟对话框保存）
    AppConfig updated = AppConfig::Defaults();
    updated.default_encoder_settings.rotate_cw_key = "pagedown";
    updated.default_encoder_settings.rotate_ccw_key = "pageup";
    updated.default_encoder_settings.rotate_fast_threshold = 100000;
    coordinator.UpdateConfig(updated);

    // 旋转后应注入自定义键 PageDown
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));
    assert(input.sent_key_combos.size() == 1);  // pending 未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();
    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[1] == "PageDown");
}

void TestEncoderRotateInvalidKeyFallsBackToArrows() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_cw_key = "bogus";  // 运行期非法
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    assert(input.arrow_down_count == 2);  // 回退方向键
    assert(input.sent_key_combos.empty());
}

void TestEncoderRotateSpeedThreshold() {
    // 判快纯函数：对 EWMA 平滑估计值比较阈值；threshold<=0 视为关闭，永不判快。
    assert(!EncoderRotateIsFast(99.9, 100));
    assert(EncoderRotateIsFast(100.0, 100));   // 边界判快
    assert(EncoderRotateIsFast(350.0, 300));
    assert(!EncoderRotateIsFast(299.9, 300));
    assert(!EncoderRotateIsFast(800.0, 0));    // 阈值 <=0 永不判快
    assert(!EncoderRotateIsFast(800.0, -1));
}

void TestEncoderRotateSpeedEstimatorEwma() {
    // EWMA 平滑测速：单窗口格速 steps*100 按 α=0.5 指数平均，冷启动从零。
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    EncoderRotateSpeedEstimator est;
    // 持续 1 步/窗（100 格/秒）：估计渐近 100，永不达到。
    assert(est.AddSample(t0, 1) == 50.0);
    assert(est.AddSample(t0 + std::chrono::milliseconds(10), 1) == 75.0);
    double v = 0.0;
    for (int i = 0; i < 20; ++i) {
        v = est.AddSample(t0 + std::chrono::milliseconds(10 * (i + 2)), 1);
    }
    assert(v > 99.0 && v < 100.0);
    // 偶发 2 步窗口只把估计抬到 ~100，不会瞬间翻倍（阈值 110 不再误判）。
    EncoderRotateSpeedEstimator est2;
    assert(est2.AddSample(t0, 2) == 100.0);  // 孤立 2 步窗：冷启动减半
    // 持续 2 步/窗（真实 200 格/秒）：估计 2~3 窗后越过 150 区间。
    assert(est2.AddSample(t0 + std::chrono::milliseconds(10), 2) == 150.0);
    assert(est2.AddSample(t0 + std::chrono::milliseconds(20), 2) == 175.0);
    // 快甩首窗 8 步：估计 400，默认阈值 200 下立即判快。
    EncoderRotateSpeedEstimator est3;
    assert(est3.AddSample(t0, 8) == 400.0);
    assert(EncoderRotateIsFast(est3.AddSample(t0 + std::chrono::milliseconds(10), 8), 200));
}

void TestEncoderRotateSpeedEstimatorGestureGapResets() {
    // 静默超过停转窗口（250ms）视为新手势：估计值清零冷启动，旧手势高速不残留。
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    EncoderRotateSpeedEstimator est;
    est.AddSample(t0, 8);
    est.AddSample(t0 + std::chrono::milliseconds(10), 8);
    // 300ms 静默后 1 步：若未清零估计会远超 50。
    assert(est.AddSample(t0 + std::chrono::milliseconds(310), 1) == 50.0);
    // Reset 同样清零。
    est.AddSample(t0 + std::chrono::milliseconds(320), 8);
    est.Reset();
    assert(est.AddSample(t0 + std::chrono::milliseconds(330), 1) == 50.0);
}

void TestEncoderRotateFastBurstUsesFastKey() {
    // 默认配置：快甩（steps=8 → 800 格/秒 ≥ 默认阈值 400）走快速档按键，一次手势只注入一次。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));   // 快：注入 PageDown ×1，进入停转锁定
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 5));  // 锁定中（含换向），屏蔽

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");
    assert(input.arrow_down_count == 0);
    assert(input.arrow_up_count == 0);
}

void TestEncoderRotateDirectionChangeAfterStopStartsNewGesture() {
    // 停转（静默超 250ms）后换向快甩：正常开启新手势注入。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));
    assert(input.sent_key_combos.size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 5));

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[0] == "PageDown");
    assert(input.sent_key_combos[1] == "PageUp");
}

void TestEncoderRotateFastBurstInjectsOncePerGesture() {
    // 一次快甩跨多个 10ms 窗口（多个快速事件）也只注入一次，避免连续翻页。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 6));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 4));

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");
}

void TestEncoderRotateFastBurstNewGestureAfterGap() {
    // 快速事件间隔超过停转窗口（250ms）后视为停稳，新手势再次注入。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));
    assert(input.sent_key_combos.size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[1] == "PageDown");
}

void TestEncoderRotateLockoutSuppressesDeceleration() {
    // 快甩后的减速段慢速事件被屏蔽：必须等停转才恢复识别。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));  // 快：注入 PageDown ×1，进入锁定
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));  // 减速段慢速：屏蔽
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));  // 减速段慢速：屏蔽
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 6));  // 锁定中快速：屏蔽

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");
}

void TestEncoderRotateSlowResumesAfterStop() {
    // 停转后慢速识别恢复：屏蔽减速段 → 静默停稳 → 慢转逐格注入。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定，专注验证停转锁定/恢复
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));  // 快：PageDown ×1，进入锁定
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));  // 减速段：屏蔽
    assert(input.sent_key_combos.size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 停稳
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));  // 慢转恢复：Down ×1

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[0] == "PageDown");
    assert(input.sent_key_combos[1] == "Down");
}

void TestEncoderRotateSlowStillUsesNormalKey() {
    // 慢速事件不受快速档配置影响：steps=2 → 200 格/秒 < 400，走普通按键。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_decide_window_ms = 0;  // 关闭延迟判定
    config.default_encoder_settings.rotate_fast_threshold = 100000;  // 隔离快慢分档，专注验证普通键路径
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[0] == "Down");
    assert(input.sent_key_combos[1] == "Down");
}

void TestEncoderRotateAccelerationDiscardedByFast() {
    // 延迟判定（默认 80ms 窗）：加速段慢速事件先挂起，窗内判快后整段丢弃，
    // 一次快甩只输出快速键，不夹带加速段的慢速注入。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));  // 加速段：挂起，不注入
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));  // 加速段：挂起，不注入
    assert(input.sent_key_combos.empty());
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));  // 判快：丢弃 pending，注入快速键

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");
}

void TestEncoderRotateSlowFlushesAfterDecisionWindow() {
    // 真慢转：判定窗（80ms）内无快速事件，到期由 tick 冲刷，按累计格数注入。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));
    assert(input.sent_key_combos.empty());  // 窗内未冲刷
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Down");
}

void TestEncoderRotateIsolatedTwoStepNudgeStaysSlow() {
    // 低阈值（110 格/秒）下的孤立 2 步轻拨：EWMA 冷启动把单窗 200 格/秒减半到 100，
    // 不误判快速档；判定窗到期按普通按键补注 2 格。修复单窗口量化导致的
    // 「阈值 100~200 间手感相同、稍微快一点就触发快速档」的非线性。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_fast_threshold = 110;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));  // 估计 100 < 110：挂起
    assert(input.sent_key_combos.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 2);  // 普通按键补注 2 格，无 PageDown
    assert(input.sent_key_combos[0] == "Down");
    assert(input.sent_key_combos[1] == "Down");
}

void TestEncoderRotateSustainedTwoStepRotationGoesFast() {
    // 同样阈值 110：持续 2 步/窗（真实 200 格/秒）第二窗估计即达 150 ≥ 110，
    // 判快并丢弃起步 pending——阈值在 100~300 全程获得近线性单调手感。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_fast_threshold = 110;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));  // 估计 100 < 110：挂起
    assert(input.sent_key_combos.empty());
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));  // 估计 150 ≥ 110：判快
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "PageDown");
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));  // 锁定中：屏蔽
    assert(input.sent_key_combos.size() == 1);
}

void TestEncoderRotateSlowContinuousBatches() {
    // 连续慢转：pending 跨事件累计，到期一次性补注全部格数（总量不变）。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    assert(input.sent_key_combos.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 3);
    assert(std::all_of(input.sent_key_combos.begin(), input.sent_key_combos.end(),
                       [](const std::string& s) { return s == "Up"; }));
}

void TestEncoderRotatePendingDirectionChangeFlushesOld() {
    // pending 期间换向：旧方向 pending 立即冲刷（不是加速段），新方向重新挂起。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 1));   // cw pending 1 格
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));  // 换向：冲刷 cw，ccw 挂起
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Down");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    coordinator.EncoderRotateTick();

    assert(input.sent_key_combos.size() == 2);
    assert(input.sent_key_combos[1] == "Up");
}

void TestEncoderRotateFastInvalidKeyFallsBackToNormalKey() {
    // 快速档按键运行期非法时回退普通按键（而非方向键兜底），一次手势仍只注入一次。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_cw_fast_key = "bogus";  // 运行期非法
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 8));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Down");
    assert(input.arrow_down_count == 0);  // 普通按键合法，不触发方向键兜底
}

void TestAppConfigEncoderFastSettingsRoundTrip() {
    // 默认值等价当前硬编码行为。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.default_encoder_settings.rotate_fast_threshold == 200);
    assert(defaults.default_encoder_settings.rotate_cw_fast_key == "pagedown");
    assert(defaults.default_encoder_settings.rotate_ccw_fast_key == "pageup");
    assert(defaults.default_encoder_settings.rotate_decide_window_ms == 80);

    // 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_fast_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.rotate_fast_threshold = 250;
    config.default_encoder_settings.rotate_cw_fast_key = "ctrl+pagedown";
    config.default_encoder_settings.rotate_ccw_fast_key = "ctrl+pageup";
    config.default_encoder_settings.rotate_decide_window_ms = 120;
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_encoder_settings.rotate_fast_threshold == 250);
    assert(loaded.default_encoder_settings.rotate_cw_fast_key == "ctrl+pagedown");
    assert(loaded.default_encoder_settings.rotate_ccw_fast_key == "ctrl+pageup");
    assert(loaded.default_encoder_settings.rotate_decide_window_ms == 120);
    std::filesystem::remove(temp);
}

void TestAppConfigEncoderFastSettingsInvalidFallback() {
    // 非法值回退默认：阈值 <=0、按键语法非法。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_fast_invalid_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "encoder_rotate_fast_threshold = -5\n";
        out << "encoder_rotate_cw_fast_key = \"bogus\"\n";
        out << "encoder_rotate_ccw_fast_key = \"ctrl+\"\n";
        out << "encoder_rotate_decide_window_ms = -10\n";
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_encoder_settings.rotate_fast_threshold == 200);
    assert(loaded.default_encoder_settings.rotate_cw_fast_key == "pagedown");
    assert(loaded.default_encoder_settings.rotate_ccw_fast_key == "pageup");
    assert(loaded.default_encoder_settings.rotate_decide_window_ms == 80);
    std::filesystem::remove(temp);
}

void TestPhysicalPrimaryUnaffectedByEncoderConfig() {
    // press_action=key 只影响 source=encoder 的事件；物理主键单击行为不变。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.press_action = "key";
    config.default_encoder_settings.press_key = "ctrl+z";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 物理主键单击（无 source）：hold_to_talk 默认下走现有 ready 回写，不注入组合键。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary"));
    assert(input.sent_key_combos.empty());
}

void TestEncoderPressRecordingButtonUpStopsSession() {
    // press_action=recording（默认）：编码器 button_up 与物理主键 up 同一收尾路径。
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

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_down", 40));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));  // 过最短录音时长阈值
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(40, 1));  // 首帧音频触发 ASR 懒启动
    assert(asr_ptr->started);
    assert(!asr_ptr->last_chunk_was_final);

    // 编码器 button_up → 走主键 up 路径进入等 audio_end（ui_state=thinking）。
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_up"));
    assert(HasUiState(*ble_ptr, "thinking", "5A74"));

    // audio_end 到达后正常收尾：最终帧标记 is_last。
    ble_ptr->on_audio_frame("5A74", EmptyEndFrame(40, 2));
    assert(asr_ptr->last_chunk_was_final);
}

void TestEncoderSourceSecondaryFallsBackToPhysicalPath() {
    // source=encoder 只分流 primary；button=secondary 即使带 encoder 标签也走物理侧键路径。
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

    StateEvent event;
    event.event = "button_click";
    event.button = "secondary";
    event.source = "encoder";
    ble_ptr->sent_ui_states.clear();
    ble_ptr->on_state_event("5A74", event);

    // 与物理侧键单击一致：空闲态不再进入体感鼠标（断言方式对齐 TestCoordinatorAirMouseToggleViaSecondary）。
    assert(ble_ptr->sent_air_mouse_enabled.empty());
    assert(!HasUiState(*ble_ptr, "air_mouse", "5A74"));
    assert(input.sent_key_combos.empty());
}

void TestEncoderConfigUpdateTakesEffectImmediately() {
    // UpdateConfig 改 press_action 后无需重启协调器，编码器单击行为立即切换。
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

    // 初始 press_action=recording：编码器 down 开播确认。
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_down", 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));  // 过最短录音时长阈值
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(50, 1));
    assert(asr_ptr->started);

    // 热更新为 key 动作（UpdateConfig 会取消活跃会话，属既有语义）。
    AppConfig updated = AppConfig::Defaults();
    updated.default_encoder_settings.press_action = "key";
    updated.default_encoder_settings.press_key = "ctrl+z";
    coordinator.UpdateConfig(updated);

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_click"));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Z");
}

// 空闲态侧键单击不再进入体感鼠标；体感态下侧键单击退出（体感优先决策）。
void TestSseParser() {
    // 正常 token
    bool done = false;
    const auto line1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
    auto token1 = LLMChatClient::ParseSseLine(line1, &done);
    assert(!done);
    assert(token1 == "Hello");

    // 空 delta（finish_reason 但没有 content）
    const auto line2 = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}";
    auto token2 = LLMChatClient::ParseSseLine(line2, &done);
    assert(!done);
    assert(token2.empty());

    // [DONE] 信号
    const auto line3 = "data: [DONE]";
    auto token3 = LLMChatClient::ParseSseLine(line3, &done);
    assert(done);
    assert(token3.empty());

    // 注释行（以 : 开头）
    bool done4 = false;
    auto token4 = LLMChatClient::ParseSseLine(": heartbeat", &done4);
    assert(!done4);
    assert(token4.empty());

    // 空行
    bool done5 = false;
    auto token5 = LLMChatClient::ParseSseLine("", &done5);
    assert(!done5);
    assert(token5.empty());

    // data: 后有前导空格
    bool done6 = false;
    const auto line6 = "data:     {\"choices\":[{\"delta\":{\"content\":\"World\"}}]}";
    auto token6 = LLMChatClient::ParseSseLine(line6, &done6);
    assert(!done6);
    assert(token6 == "World");

    // 非法 JSON（不崩溃，返回空）
    bool done7 = false;
    auto token7 = LLMChatClient::ParseSseLine("data: {not valid json", &done7);
    assert(!done7);
    assert(token7.empty());

    // 多字节 UTF-8 中文 token
    bool done8 = false;
    const auto line8 = "data: {\"choices\":[{\"delta\":{\"content\":\"你好世界\"}}]}";
    auto token8 = LLMChatClient::ParseSseLine(line8, &done8);
    assert(!done8);
    assert(token8 == "你好世界");

    // 非 data: 前缀的 event: 行（SSE event 字段，忽略）
    bool done9 = false;
    auto token9 = LLMChatClient::ParseSseLine("event: message", &done9);
    assert(!done9);
    assert(token9.empty());
}

void TestStreamPayload() {
    // 流式 payload 包含 "stream":true
    const auto payload = LLMChatClient::BuildChatPayload("gpt-x", "sys", "user text", /*stream=*/true);
    assert(payload.find("\"stream\":true") != std::string::npos);
    assert(payload.find("\"model\":\"gpt-x\"") != std::string::npos);
    assert(payload.find("\"temperature\":0") != std::string::npos);

    // 非流式 payload 不含 stream 字段（向后兼容）
    const auto regular = LLMChatClient::BuildChatPayload("gpt-x", "sys", "user text", /*stream=*/false);
    assert(regular.find("\"stream\"") == std::string::npos);

    // 默认无 stream 参数（向后兼容）
    const auto default_payload = LLMChatClient::BuildChatPayload("gpt-x", "sys", "user text");
    assert(default_payload.find("\"stream\"") == std::string::npos);

    // 默认不注入关思考参数；显式开启时流式 payload 也带（disable_thinking 与 stream 正交）。
    assert(default_payload.find("enable_thinking") == std::string::npos);
    const auto no_think_stream = LLMChatClient::BuildChatPayload(
        "gpt-x", "sys", "user text", /*stream=*/true, /*disable_thinking=*/true);
    assert(no_think_stream.find("\"stream\":true") != std::string::npos);
    assert(no_think_stream.find("\"enable_thinking\":false") != std::string::npos);
}

void TestDeepSeekThinkingDisabled() {
    // DeepSeek V4 系列思考模式默认开启且 effort=high，会显著拖慢精修/翻译 TTFT。
    // disable_thinking=true 时 BuildChatPayload 检测到模型名含 "deepseek" 应额外
    // 注入 thinking:{type:disabled} 关闭思考模式。
    const auto ds_payload = LLMChatClient::BuildChatPayload(
        "deepseek-v4-flash", "sys", "user", /*stream=*/false, /*disable_thinking=*/true);
    assert(ds_payload.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
    // 同样适用于 deepseek-v4-pro 与 deepseek-chat 别名。
    const auto pro_payload = LLMChatClient::BuildChatPayload(
        "deepseek-v4-pro", "sys", "user", /*stream=*/false, /*disable_thinking=*/true);
    assert(pro_payload.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
    const auto chat_payload = LLMChatClient::BuildChatPayload(
        "deepseek-chat", "sys", "user", /*stream=*/false, /*disable_thinking=*/true);
    assert(chat_payload.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
    // 非 DeepSeek 模型不应添加 thinking 字段，避免对其他 OpenAI 兼容端点造成干扰。
    const auto gpt_payload = LLMChatClient::BuildChatPayload(
        "gpt-5.5", "sys", "user", /*stream=*/false, /*disable_thinking=*/true);
    assert(gpt_payload.find("\"thinking\"") == std::string::npos);
    // 流式 + DeepSeek 也应关闭思考模式。
    const auto ds_stream = LLMChatClient::BuildChatPayload(
        "deepseek-v4-flash", "sys", "user", /*stream=*/true, /*disable_thinking=*/true);
    assert(ds_stream.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
    assert(ds_stream.find("\"stream\":true") != std::string::npos);
    // 大小写不敏感匹配：用户可能配置 "DeepSeek-V4-Flash" 等大写写法。
    const auto caps_payload = LLMChatClient::BuildChatPayload(
        "DeepSeek-V4-Flash", "sys", "user", /*stream=*/false, /*disable_thinking=*/true);
    assert(caps_payload.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
    // 整体仍是合法 JSON。
    auto* root = cJSON_Parse(ds_payload.c_str());
    assert(root != nullptr);
    auto* thinking = cJSON_GetObjectItemCaseSensitive(root, "thinking");
    assert(cJSON_IsObject(thinking));
    auto* type = cJSON_GetObjectItemCaseSensitive(thinking, "type");
    assert(cJSON_IsString(type) && std::string(type->valuestring) == "disabled");
    cJSON_Delete(root);
}


void TestAudioOpusDecoderRoundTrip() {
    const auto pcm_in = MakeSinePcm(440);
    const auto packet = EncodeOpusPacket(pcm_in);

    AudioOpusDecoder decoder(16000, 1);
    std::vector<int16_t> pcm_out(pcm_in.size(), 0);
    auto result = decoder.Decode(packet.data(), packet.size(), pcm_out.data(), pcm_out.size());

    assert(result.opus_error == 0);
    assert(result.decoded_samples == static_cast<int>(pcm_in.size()));

    // 验证解码输出不是静音，且能量与原始信号处于同一数量级。
    double input_rms = 0.0;
    double output_rms = 0.0;
    for (std::size_t i = 0; i < pcm_in.size(); ++i) {
        input_rms += static_cast<double>(pcm_in[i]) * pcm_in[i];
        output_rms += static_cast<double>(pcm_out[i]) * pcm_out[i];
    }
    input_rms = std::sqrt(input_rms / pcm_in.size());
    output_rms = std::sqrt(output_rms / pcm_out.size());
    assert(output_rms > 1000.0);  // 明显不是静音。
    assert(output_rms > input_rms * 0.3 && output_rms < input_rms * 3.0);  // 能量在同一数量级。
}

void TestAudioOpusDecoderNullData() {
    AudioOpusDecoder decoder(16000, 1);
    std::vector<int16_t> pcm_out(640, 0);
    auto result = decoder.Decode(nullptr, 10, pcm_out.data(), pcm_out.size());
    assert(result.opus_error != 0);
    assert(result.decoded_samples == 0);
}

void TestAudioOpusDecoderInvalidData() {
    AudioOpusDecoder decoder(16000, 1);
    std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<int16_t> pcm_out(640, 0);
    auto result = decoder.Decode(garbage.data(), garbage.size(), pcm_out.data(), pcm_out.size());
    assert(result.opus_error != 0);
    assert(result.decoded_samples == 0);
}

void TestAudioOpusDecoderSmallBuffer() {
    const auto pcm_in = MakeSinePcm(440);
    const auto packet = EncodeOpusPacket(pcm_in);

    AudioOpusDecoder decoder(16000, 1);
    std::vector<int16_t> pcm_out(10, 0);  // 远小于 640 样本。
    auto result = decoder.Decode(packet.data(), packet.size(), pcm_out.data(), pcm_out.size());
    assert(result.opus_error != 0);
    assert(result.decoded_samples == 0);
}



// Suite entry: core_tests.cc main() calls this once.
void RunDeviceInputMiscBatchTests() {
    TestTapEventInjectsArrowDown();
    TestTapDisabledWhenConfigOff();
    TestTapIgnoredDuringRecording();
    TestTapThrottledWithin500ms();
    TestTapThrottleRecoversAfter500ms();
    TestInputInjectorArrowUpFakeWiring();
    TestInputInjectorKeyComboFakeWiring();
    TestKeySpecParse();
    TestKeySpecMakeFromVk();
    TestAppConfigEncoderRoundTrip();
    TestAppConfigEncoderSettingsRoundTrip();
    TestAppConfigEncoderSettingsInvalidFallback();
    TestEncoderRotateMapsDirectionToArrows();
    TestEncoderRotateInvertFlipsDirection();
    TestEncoderRotateDisabledWhenConfigOff();
    TestEncoderRotateIgnoredDuringRecording();
    TestEncoderRotateUnknownDirectionTreatedAsCw();
    TestEncoderRotateStepsClamped();
    TestEncoderPressRecordingStartsSession();
    TestEncoderPressKeyInjectsComboWithoutRecording();
    TestEncoderPressKeyInvalidIgnored();
    TestEncoderDoubleClickDefaultEnterCancelsSession();
    TestEncoderDoubleClickCustomKey();
    TestEncoderDoubleClickRecordingTogglesRemoteButton();
    TestEncoderRotateCustomKeys();
    TestEncoderRotateCustomKeysPendingPath();
    TestEncoderRotateCustomKeysPendingPathDeviceOverride();
    TestEncoderRotateCustomKeysAfterUpdateConfig();
    TestEncoderRotateInvalidKeyFallsBackToArrows();
    TestEncoderRotateSpeedThreshold();
    TestEncoderRotateSpeedEstimatorEwma();
    TestEncoderRotateSpeedEstimatorGestureGapResets();
    TestEncoderRotateFastBurstUsesFastKey();
    TestEncoderRotateDirectionChangeAfterStopStartsNewGesture();
    TestEncoderRotateFastBurstInjectsOncePerGesture();
    TestEncoderRotateFastBurstNewGestureAfterGap();
    TestEncoderRotateLockoutSuppressesDeceleration();
    TestEncoderRotateSlowResumesAfterStop();
    TestEncoderRotateSlowStillUsesNormalKey();
    TestEncoderRotateAccelerationDiscardedByFast();
    TestEncoderRotateSlowFlushesAfterDecisionWindow();
    TestEncoderRotateIsolatedTwoStepNudgeStaysSlow();
    TestEncoderRotateSustainedTwoStepRotationGoesFast();
    TestEncoderRotateSlowContinuousBatches();
    TestEncoderRotatePendingDirectionChangeFlushesOld();
    TestEncoderRotateFastInvalidKeyFallsBackToNormalKey();
    TestAppConfigEncoderFastSettingsRoundTrip();
    TestAppConfigEncoderFastSettingsInvalidFallback();
    TestPhysicalPrimaryUnaffectedByEncoderConfig();
    TestEncoderPressRecordingButtonUpStopsSession();
    TestEncoderSourceSecondaryFallsBackToPhysicalPath();
    TestEncoderConfigUpdateTakesEffectImmediately();
    TestSseParser();
    TestStreamPayload();
    TestDeepSeekThinkingDisabled();
    TestAudioOpusDecoderRoundTrip();
    TestAudioOpusDecoderNullData();
    TestAudioOpusDecoderInvalidData();
    TestAudioOpusDecoderSmallBuffer();
}

