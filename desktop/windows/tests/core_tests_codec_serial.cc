// N8 cut13: codec/fixture/serial/esptool/flash/power batch extracted from
// core_tests.cc (largest contiguous block; count converges as shared-helper
// barriers move to test_support.h). Helpers migrated in this change:
// ATVV fixtures quartet, ArgvContains, Flash test doubles, power/state frame
// builders, plus anything else found by the convergent two-direction scan.
#include "test_support.h"

void TestImaAdpcmDecoderGoldenFixtures() {
    const auto root = ResolveAtvvFixturesRoot();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        std::printf("SKIP: ATVV golden fixtures 目录不存在: %s\n",
                    root.string().c_str());
        return;
    }
    std::vector<std::filesystem::path> adpcm_files;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         it != end && !ec; it.increment(ec)) {
        const auto& p = it->path();
        if (it->is_regular_file(ec) && p.extension() == ".adpcm" &&
            p.filename().string().rfind("session_", 0) == 0) {
            adpcm_files.push_back(p);
        }
    }
    std::sort(adpcm_files.begin(), adpcm_files.end());
    if (adpcm_files.empty()) {
        std::printf("SKIP: %s 下无 session_*.adpcm\n", root.string().c_str());
        return;
    }

    int checked = 0;
    int failed = 0;
    for (const auto& adpcm_path : adpcm_files) {
        const std::string stem = adpcm_path.stem().string();  // session_N
        const auto dir = adpcm_path.parent_path();
        const auto sidecar_path = dir / (stem + ".json");
        const auto raw_wav_path = dir / (stem + ".raw.wav");
        const auto wav_path = dir / (stem + ".wav");
        // 前置校验与数据准备必须用显式检查：写进 assert 会在 NDEBUG 下整式短路，
        // 曾经导致 Release 全部 session 以空 segments 对拍失败（Debug 却全绿）。
        bool fixture_ok = std::filesystem::exists(sidecar_path, ec) &&
                          std::filesystem::exists(raw_wav_path, ec) &&
                          std::filesystem::exists(wav_path, ec);
        // 有 .adpcm 但缺 sidecar/WAV 属于残缺 fixtures，直接失败暴露问题。
        if (!fixture_ok) {
            std::printf("FAIL %s fixtures 四件套不完整\n", stem.c_str());
            ++failed;
            continue;
        }

        double gain_db = 0.0;
        std::vector<AtvvGoldenSegment> segments;
        if (!ParseAtvvSidecarForTest(ReadTextFileForTest(sidecar_path),
                                     &gain_db, &segments)) {
            std::printf("FAIL %s sidecar 解析失败\n", stem.c_str());
            ++failed;
            continue;
        }

        const std::string adpcm_text = ReadTextFileForTest(adpcm_path);
        const auto* adpcm = reinterpret_cast<const std::uint8_t*>(adpcm_text.data());
        const std::size_t adpcm_size = adpcm_text.size();
        ImaAdpcmDecoder decoder;
        std::vector<std::int16_t> pcm;
        bool bounds_ok = true;
        for (const auto& seg : segments) {
            if (seg.offset + seg.bytes > adpcm_size) {
                bounds_ok = false;
                break;
            }
            decoder.Reset(static_cast<std::int16_t>(seg.predictor), seg.step_index);
            auto part = decoder.Decode(
                std::span<const std::uint8_t>(adpcm + seg.offset, seg.bytes));
            pcm.insert(pcm.end(), part.begin(), part.end());
        }
        if (!bounds_ok) {
            std::printf("FAIL %s sidecar 段区间越界 adpcm\n", stem.c_str());
            ++failed;
            continue;
        }

        // 对拍 1：纯解码 == session_N.raw.wav（逐样本相等）。
        const auto expected_raw = ReadWavPcm16ForTest(raw_wav_path);
        if (pcm != expected_raw) {
            std::size_t diff = 0;
            while (diff < std::min(pcm.size(), expected_raw.size()) &&
                   pcm[diff] == expected_raw[diff]) {
                ++diff;
            }
            std::printf("FAIL %s raw.wav 对拍失败: sizes %zu vs %zu, "
                        "首个差异样本 #%zu\n", stem.c_str(), pcm.size(),
                        expected_raw.size(), diff);
            ++failed;
        }

        // 对拍 2：解码 + PcmPostprocessor(sidecar 增益) == session_N.wav。
        // Python 侧 smooth3+apply_gain 的舍入已对齐 std::lround。
        const PcmPostprocessor postprocessor(gain_db);
        const auto processed = postprocessor.Process(pcm);
        const auto expected_wav = ReadWavPcm16ForTest(wav_path);
        if (processed != expected_wav) {
            std::printf("FAIL %s wav 对拍失败（gain_db=%.2f）\n",
                        stem.c_str(), gain_db);
            ++failed;
        }

        ++checked;
        std::printf("  golden %s: %zu samples, %zu segment(s) %s\n",
                    (dir.filename().string() + "/" + stem).c_str(),
                    pcm.size(), segments.size(),
                    (pcm == expected_raw && processed == expected_wav) ? "OK" : "FAIL");
    }
    std::printf("ATVV golden fixtures: %d session(s) checked\n", checked);
    if (failed > 0) {
        std::fprintf(stderr, "TestImaAdpcmDecoderGoldenFixtures: %d 处对拍失败\n",
                     failed);
        std::abort();
    }
}


void TestFrameAccumulator() {
    // 跨包切帧：3 + 5 字节凑出两个 4 字节帧。
    FrameAccumulator accumulator(4);
    assert(accumulator.Append(ByteVector{1, 2, 3}).empty());
    assert(accumulator.pending_bytes() == 3);
    const auto frames = accumulator.Append(ByteVector{4, 5, 6, 7, 8});
    assert(frames.size() == 2);
    assert((frames[0] == ByteVector{1, 2, 3, 4}));
    assert((frames[1] == ByteVector{5, 6, 7, 8}));
    assert(accumulator.pending_bytes() == 0);  // 8 字节恰好两帧

    // Reset 清空部分累积。
    accumulator.Reset();
    assert(accumulator.pending_bytes() == 0);

    // 自定义协商帧长。
    accumulator.set_frame_bytes(3);
    assert(accumulator.frame_bytes() == 3);
    assert(accumulator.pending_bytes() == 0);  // set_frame_bytes 同时清空
    const auto custom = accumulator.Append(ByteVector{1, 2, 3, 4});
    assert(custom.size() == 1 && (custom[0] == ByteVector{1, 2, 3}));
    assert(accumulator.pending_bytes() == 1);
}

void TestPcmPostprocessor() {
    // 三点平滑公式（首尾样本不动），增益 0dB。
    PcmPostprocessor flat(0.0);
    const std::vector<std::int16_t> in{0, 100, 200, 300, 400};
    const auto smoothed = flat.Process(in);
    // out[1]=(0+200+200)>>2=100, out[2]=(100+400+300)>>2=200, out[3]=(200+600+400)>>2=300
    assert((smoothed == std::vector<std::int16_t>{0, 100, 200, 300, 400}));

    // 增益 +6.02dB ≈ ×2；-6.02dB ≈ ×0.5（少于 3 样本跳过平滑只作增益）。
    PcmPostprocessor boost(6.020599913279624);
    const auto boosted = boost.Process(std::vector<std::int16_t>{1000});
    assert(boosted.size() == 1 && boosted[0] == 2000);
    PcmPostprocessor cut(-6.020599913279624);
    const auto attenuated = cut.Process(std::vector<std::int16_t>{1000});
    assert(attenuated.size() == 1 && attenuated[0] == 500);

    // 增益钳位 ±24dB。
    PcmPostprocessor clamped(30.0);
    assert(clamped.gain_db() == 24.0);
    clamped.set_gain_db(-30.0);
    assert(clamped.gain_db() == -24.0);
    clamped.set_gain_db(24.0);
    const auto gained = clamped.Process(std::vector<std::int16_t>{1000});
    assert(gained[0] == 15849);  // 1000 * 10^(24/20)

    // int16 限幅。
    const auto limited = clamped.Process(std::vector<std::int16_t>{3000});
    assert(limited[0] == 32767);
    const auto limited_neg = clamped.Process(std::vector<std::int16_t>{-3000});
    assert(limited_neg[0] == -32768);
}

void TestAudioOpusEncoderRoundTrip() {
    const auto pcm_in = MakeSinePcm(440);  // 640 采样（40ms）
    AudioOpusEncoder encoder;
    std::vector<std::uint8_t> packet(1500);
    const auto result = encoder.Encode(pcm_in.data(), pcm_in.size(), packet.data(), packet.size());
    assert(result.opus_error == 0);
    assert(result.encoded_bytes > 0);

    AudioOpusDecoder decoder(16000, 1);
    std::vector<int16_t> pcm_out(pcm_in.size(), 0);
    const auto decoded = decoder.Decode(packet.data(), result.encoded_bytes,
                                        pcm_out.data(), pcm_out.size());
    assert(decoded.opus_error == 0);
    assert(decoded.decoded_samples == static_cast<int>(pcm_in.size()));

    // 能量 sanity：不是静音，且与输入同数量级（Opus 有损，容差放宽）。
    double input_rms = 0.0;
    double output_rms = 0.0;
    for (std::size_t i = 0; i < pcm_in.size(); ++i) {
        input_rms += static_cast<double>(pcm_in[i]) * pcm_in[i];
        output_rms += static_cast<double>(pcm_out[i]) * pcm_out[i];
    }
    input_rms = std::sqrt(input_rms / pcm_in.size());
    output_rms = std::sqrt(output_rms / pcm_out.size());
    assert(output_rms > 1000.0);
    assert(output_rms > input_rms * 0.5 && output_rms < input_rms * 2.0);

    // 非法参数：空指针、非 Opus 合法帧长。
    auto bad = encoder.Encode(nullptr, 640, packet.data(), packet.size());
    assert(bad.opus_error != 0 && bad.encoded_bytes == 0);
    bad = encoder.Encode(pcm_in.data(), 319, packet.data(), packet.size());
    assert(bad.opus_error != 0 && bad.encoded_bytes == 0);

    // 组帧器：不足 640 采样不吐帧，跨 Append 累积，余量正确。
    OpusFrameSlicer slicer;
    std::vector<std::int16_t> chunk(300, 7);
    assert(slicer.Append(chunk).empty());
    assert(slicer.remainder().size() == 300);
    chunk.assign(400, 9);
    const auto frames = slicer.Append(chunk);
    assert(frames.size() == 1);
    assert(frames[0].size() == AudioOpusEncoder::kFrameSamples);
    assert(frames[0][0] == 7 && frames[0][299] == 7 && frames[0][300] == 9);
    assert(slicer.remainder().size() == 60);
    const auto rest_pcm = slicer.TakeRemainder();
    assert(rest_pcm.size() == 60 && slicer.remainder().empty());
    slicer.Reset();
    assert(slicer.remainder().empty());
}

// ①hold_to_talk 正常录音 → ASR 送出 Ogg，final 后粘贴。
void TestCoordinatorXiaomiHoldToTalkStreamsOggToAsr() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 关闭异步精修，验证同步粘贴流程
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    const std::string kDev = "RC-1A2B";
    XiaomiAtvvSession session;  // 默认 hold_to_talk
    std::int64_t t = 1000;
    AtvvCoordinatorHandshake(session, t);

    AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
    assert(HasUiState(*ble_ptr, "recording", kDev));

    // 跨过协调器 0.5s 最小录音时长（墙钟），期间持续出音频帧。
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    InjectAtvvActions(*ble_ptr, kDev, session.HandleAudioData(ByteVector(480, 0x11), t + 100));
    AtvvEndRecording(*ble_ptr, kDev, session, t);

    // end 帧到达后协调器启动 ASR 并把整段 Ogg 送出，末包 is_last。
    assert(asr_ptr->started);
    assert(asr_ptr->sent_chunks > 0);
    assert(asr_ptr->last_chunk_was_final);

    asr_ptr->on_final("hello xiaomi");
    assert(input.pasted_text == "hello xiaomi");
    assert(HasUiState(*ble_ptr, "ready", kDev));
}

// ②识别中侧键取消 / 录音中主键双击取消语义对 RC 设备 id 不变。
void TestCoordinatorXiaomiCancelSemantics() {
    const std::string kDev = "RC-3C4D";

    // 场景 A：识别中（finalizing）secondary 单击取消，不粘贴。
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto* ble_ptr = ble.get();
        auto asr = std::make_unique<FakeAsrClient>();
        auto* asr_ptr = asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.refine_enabled = false;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
        coordinator.Start();

        XiaomiAtvvSession session;
        std::int64_t t = 1000;
        AtvvCoordinatorHandshake(session, t);
        AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        AtvvEndRecording(*ble_ptr, kDev, session, t);
        assert(asr_ptr->started);  // finalizing：ASR 已启动等 final

        ble_ptr->on_state_event(kDev, ButtonEvent("button_up", "secondary"));
        assert(asr_ptr->cancelled);
        assert(input.pasted_text.empty());
        assert(HasUiState(*ble_ptr, "ready", kDev));
    }

    // 场景 B：录音中主键双击 → 取消录音并注入 Enter，ASR 未启动。
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto* ble_ptr = ble.get();
        auto asr = std::make_unique<FakeAsrClient>();
        auto* asr_ptr = asr.get();
        FakeUi ui;
        FakeInputInjector input;
        AppConfig config = AppConfig::Defaults();
        config.refine_enabled = false;
        VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
        coordinator.Start();

        XiaomiAtvvSession session;
        std::int64_t t = 1000;
        AtvvCoordinatorHandshake(session, t);
        AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
        assert(HasUiState(*ble_ptr, "recording", kDev));

        ble_ptr->on_state_event(kDev, DoubleClickEvent("primary"));
        assert(input.send_enter_called);
        assert(!asr_ptr->started);
        assert(input.pasted_text.empty());
        assert(HasUiState(*ble_ptr, "ready", kDev));
    }
}

// ③wechat_input_method 路径：音频经 Opus 解码写入虚拟麦 PCM ring buffer。
void TestCoordinatorXiaomiWechatInputMethodDecodesToVirtualMic() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
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

    const std::string kDev = "RC-5E6F";
    ble_ptr->connected_device_ids.insert(kDev);
    ble_ptr->on_connection_change({ConnectedDevice{kDev, "RC-5E6F"}});

    XiaomiAtvvSession session;
    std::int64_t t = 1000;
    AtvvCoordinatorHandshake(session, t);

    AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
    assert(fake_renderer != nullptr);
    assert(fake_renderer->start_count == 1);
    PcmRingBuffer* ring = fake_renderer->last_ring;
    assert(ring != nullptr);

    // button_down 时暂存帧已放出（≥1 个 640 采样 Opus 帧）；继续喂一帧。
    InjectAtvvActions(*ble_ptr, kDev, session.HandleAudioData(ByteVector(480, 0x11), t + 100));

    // 协调器把 Opus 解码为 PCM 写入 ring：至少有 640 采样且非全零（ADPCM 0x11
    // 解码为缓升信号，Opus 有损但能量保留）。轮询兜底异步解码。
    std::vector<std::int16_t> pcm(AudioOpusEncoder::kFrameSamples, 0);
    std::size_t readable = 0;
    for (int i = 0; i < 50 && readable < pcm.size(); ++i) {
        readable = ring->Available();
        if (readable < pcm.size()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(readable >= pcm.size());
    const auto read_count = ring->Read(pcm.data(), pcm.size());
    assert(read_count == pcm.size());
    const bool all_zero = std::all_of(pcm.begin(), pcm.end(),
                                      [](std::int16_t s) { return s == 0; });
    assert(!all_zero);

    // 松开后 wechat 会话完整停止。
    AtvvEndRecording(*ble_ptr, kDev, session, t);
    assert(fake_renderer->stop_count >= 1);
    assert(asr_ptr->sent_chunks == 0);  // wechat 路径不经 ASR
}

// ④字幕路径：subtitle ASR 整句识别，字幕显示、不粘贴。
void TestCoordinatorXiaomiSubtitlePath() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto primary_asr = std::make_unique<FakeAsrClient>();
    FakeAsrClient* subtitle_asr_ptr = nullptr;
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kSubtitle;
    config.refine_enabled = false;  // 字幕用例验证同步显示流程，关闭异步精修
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

    const std::string kDev = "RC-7A8B";
    XiaomiAtvvSession session;
    std::int64_t t = 1000;
    AtvvCoordinatorHandshake(session, t);

    AtvvBeginHoldRecording(*ble_ptr, kDev, session, t);
    assert(HasUiState(*ble_ptr, "recording", kDev));
    // 帧在录音时长不足 0.5s 时注入（can_start_asr=false，保持缓冲）；
    // 随后 sleep 跨过最小录音时长，模拟真实长录音。
    InjectAtvvActions(*ble_ptr, kDev, session.HandleAudioData(ByteVector(480, 0x11), t + 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(520));

    // STOP（button_up）：字幕 cycle 已建但 ASR 未启动（等整段音频）。
    const auto stop_actions_t = t + 600;
    InjectAtvvActions(*ble_ptr, kDev,
                      session.HandleControlCommand(ByteVector{0x00}, stop_actions_t));
    assert(subtitle_asr_ptr != nullptr);
    assert(!subtitle_asr_ptr->started);

    // end 帧：字幕 ASR 启动并收到整段 Ogg。
    InjectAtvvActions(*ble_ptr, kDev,
                      session.Tick(stop_actions_t + XiaomiAtvvSession::kAudioTailGraceMs));
    assert(subtitle_asr_ptr->started);
    assert(subtitle_asr_ptr->sent_chunks > 0);

    subtitle_asr_ptr->on_partial("interim xiaomi");
    assert(!ui.partials.empty());
    assert(ui.partials.back() == "interim xiaomi");
    subtitle_asr_ptr->on_final("hello xiaomi subtitle");

    // 字幕显示且不粘贴；设备回 ready。
    assert(input.pasted_text.empty());
    assert(!ui.subtitles.empty());
    assert(ui.subtitles.back().find("hello xiaomi subtitle") != std::string::npos);
    assert(ui.subtitles.back().find(kDev) != std::string::npos);
    assert(HasUiState(*ble_ptr, "ready", kDev));
}

void TestDeviceIdRcPrefix() {
    // RC- 前缀归一化：大小写不敏感、去前缀后 4 位大写 hex，与 VS- 并存。
    assert(BleProtocol::NormalizeDeviceId("RC-3A7F") == "3A7F");
    assert(BleProtocol::NormalizeDeviceId("rc-3a7f") == "3A7F");
    assert(BleProtocol::NormalizeDeviceId(" vs-c3d8 ") == "C3D8");
    assert(BleProtocol::NormalizeDeviceId("3a7f") == "3A7F");
    assert(BleProtocol::NormalizeDeviceId("RC-12").empty());
    assert(BleProtocol::NormalizeDeviceId("RC-XYZW").empty());

    assert(BleProtocol::DeviceIdFromName("RC-3A7F").value() == "3A7F");
    assert(BleProtocol::DeviceIdFromName("VS-C3D8").value() == "C3D8");
    assert(!BleProtocol::DeviceIdFromName("RC-123").has_value());
    assert(!BleProtocol::DeviceIdFromName("Other").has_value());

    // 小米名称白名单（trim+小写，中文名按 UTF-8 字节比较）。
    assert(BleProtocol::IsXiaomiRemoteName("MI RC"));
    assert(BleProtocol::IsXiaomiRemoteName(" mi rc "));
    assert(BleProtocol::IsXiaomiRemoteName("Xiaomi Bluetooth Remote 2 Pro"));
    assert(BleProtocol::IsXiaomiRemoteName("小米蓝牙语音遥控器"));
    assert(BleProtocol::IsXiaomiRemoteName("RC001"));
    assert(BleProtocol::IsXiaomiRemoteName("rc003"));
    assert(!BleProtocol::IsXiaomiRemoteName("VS-C3D8"));
    assert(!BleProtocol::IsXiaomiRemoteName(""));
    assert(!BleProtocol::IsXiaomiRemoteName("MI RC2"));

    // DeviceClassFromName：白名单 → 小米；VS- 前缀 → StickS3；其余 nullopt。
    assert(BleProtocol::DeviceClassFromName("MI RC") == DeviceClass::kXiaomiRemote2Pro);
    assert(BleProtocol::DeviceClassFromName("RC-3A7F") == DeviceClass::kXiaomiRemote2Pro);
    assert(BleProtocol::DeviceClassFromName("VS-C3D8") == DeviceClass::kStickS3);
    assert(!BleProtocol::DeviceClassFromName("Random Speaker").has_value());
}

void TestAppConfigXiaomiTable() {
    // 默认值。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.xiaomi_suppress_f5);
    assert(defaults.XiaomiSettingsForDevice(std::nullopt).gain_db == 12.0);
    assert(defaults.XiaomiSettingsForDevice(std::nullopt).double_click_ms == 350);
    assert(!defaults.XiaomiSettingsForDevice(std::nullopt).hid_tap_enabled);
    // 未覆盖设备回落全局默认。
    assert(defaults.XiaomiSettingsForDevice("RC-3A7F").gain_db == 12.0);

    // TOML 文本加载：[device.RC-3A7F.xiaomi] + 全局开关。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_xiaomi_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "paired_device_ids = \"RC-3A7F\"\n";
        out << "xiaomi_suppress_f5 = false\n";
        out << "[device.RC-3A7F.xiaomi]\n";
        out << "gain_db = 20.5\n";
        out << "double_click_ms = 450\n";
        out << "hid_tap_enabled = true\n";
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(!loaded.xiaomi_suppress_f5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").gain_db == 20.5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").double_click_ms == 450);
    assert(loaded.XiaomiSettingsForDevice("3A7F").hid_tap_enabled);
    // 访问器内部归一化：带前缀与不带前缀等价。
    assert(loaded.XiaomiSettingsForDevice("RC-3A7F").gain_db == 20.5);
    assert(loaded.XiaomiSettingsForDevice("FFFF").gain_db == 12.0);
    assert(!loaded.XiaomiSettingsForDevice("FFFF").hid_tap_enabled);
    std::filesystem::remove(temp);

    // 保存/加载往返。
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids = {"3A7F"};
    config.xiaomi_suppress_f5 = false;
    config.device_xiaomi_settings["3A7F"] = XiaomiSettings{.gain_db = 18.0, .double_click_ms = 400, .hid_tap_enabled = true};
    config.Save(temp);
    loaded = AppConfig::Load(temp);
    assert(!loaded.xiaomi_suppress_f5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").gain_db == 18.0);
    assert(loaded.XiaomiSettingsForDevice("3A7F").double_click_ms == 400);
    assert(loaded.XiaomiSettingsForDevice("3A7F").hid_tap_enabled);

    // 与默认相同不落盘。
    config.device_xiaomi_settings["3A7F"] = XiaomiSettings{};
    config.Save(temp);
    {
        std::ifstream in(temp);
        std::stringstream buffer;
        buffer << in.rdbuf();
        assert(buffer.str().find(".xiaomi") == std::string::npos);
    }
    std::filesystem::remove(temp);
}

void TestAppConfigXiaomiKeyMap() {
    // 默认可映射按键集：12 键；mic（语音键专用）与未知键不在列。
    assert(kXiaomiMappableButtons.size() == 12);
    for (std::string_view id : kXiaomiMappableButtons) {
        assert(IsXiaomiMappableButton(id));
    }
    assert(!IsXiaomiMappableButton("mic"));
    assert(!IsXiaomiMappableButton("bogus"));
    assert(!IsXiaomiMappableButton(""));

    // 默认 key_map 为空。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.default_xiaomi_settings.key_map.empty());
    assert(defaults.XiaomiSettingsForDevice("RC-3A7F").key_map.empty());

    // TOML 文本加载：全局 [xiaomi.keys] + [device.RC-3A7F.xiaomi.keys] 覆盖。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_xiaomi_keymap_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "paired_device_ids = \"RC-3A7F\"\n";
        out << "[xiaomi.keys]\n";
        out << "back = \"esc\"\n";
        out << "home = \"ctrl+h\"\n";
        out << "[device.RC-3A7F.xiaomi.keys]\n";
        out << "home = \"\"\n";              // 空串显式取消全局映射
        out << "menu = \"ctrl+shift+m\"\n";   // 设备新增
        out << "back = \"esc\"\n";            // 与全局相同
        out << "mic = \"f5\"\n";              // 非法键名忽略
        out << "tv = \"not a key\"\n";        // 非法 key_spec 忽略
        out << "power = 123\n";               // 非字符串值忽略
    }
    AppConfig loaded = AppConfig::Load(temp);
    // 全局默认。
    assert(loaded.default_xiaomi_settings.key_map.size() == 2);
    assert(loaded.default_xiaomi_settings.key_map.at("back") == "esc");
    assert(loaded.default_xiaomi_settings.key_map.at("home") == "ctrl+h");
    // 设备填平：未覆盖的键取全局默认，覆盖的键取设备值，空串优先于全局值。
    const auto& dev = loaded.XiaomiSettingsForDevice("RC-3A7F");
    assert(dev.key_map.size() == 3);
    assert(dev.key_map.at("back") == "esc");
    assert(dev.key_map.at("home").empty());
    assert(dev.key_map.at("menu") == "ctrl+shift+m");
    assert(dev.key_map.count("mic") == 0);
    assert(dev.key_map.count("tv") == 0);
    assert(dev.key_map.count("power") == 0);
    // 未覆盖设备回落全局默认。
    assert(loaded.XiaomiSettingsForDevice("FFFF").key_map ==
           loaded.default_xiaomi_settings.key_map);
    std::filesystem::remove(temp);

    // 保存/加载往返：全局默认 + 设备覆盖。
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids = {"3A7F"};
    config.default_xiaomi_settings.key_map = {{"back", "esc"}, {"home", "ctrl+h"}};
    XiaomiSettings dev_settings;  // 标量与全局默认相同，仅 key_map 不同
    dev_settings.key_map = {{"back", "esc"}, {"home", ""}, {"menu", "ctrl+shift+m"}};
    config.device_xiaomi_settings["3A7F"] = dev_settings;
    config.Save(temp);
    {
        std::ifstream in(temp);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        assert(text.find("[xiaomi.keys]") != std::string::npos);
        const auto dev_pos = text.find("[device.3A7F.xiaomi.keys]");
        assert(dev_pos != std::string::npos);
        // 与全局默认相同的条目（back=esc）不落盘；空串取消（home=""）要落盘。
        const std::string dev_section = text.substr(dev_pos);
        assert(dev_section.find("home = \"\"") != std::string::npos);
        assert(dev_section.find("menu = \"ctrl+shift+m\"") != std::string::npos);
        assert(dev_section.find("back") == std::string::npos);
    }
    loaded = AppConfig::Load(temp);
    assert(loaded.default_xiaomi_settings.key_map == config.default_xiaomi_settings.key_map);
    assert(loaded.XiaomiSettingsForDevice("RC-3A7F") == dev_settings);
    std::filesystem::remove(temp);
}

void TestXiaomiUsageTapParsing() {
    // 合法报文：back(0x00F1)+音量+(0x0080) → 升序去零 usage 集合。
    uint8_t report[9];
    MakeTapReport(report, 0x00F1, 0x0080, 0);
    const auto usages = ParseTapReportUsages(report, 9);
    assert(usages.has_value());
    assert(*usages == (std::vector<uint16_t>{0x0080, 0x00F1}));
    // 全空槽 → 空集合（全部松开）。
    MakeTapReport(report, 0, 0, 0);
    const auto empty = ParseTapReportUsages(report, 9);
    assert(empty.has_value() && empty->empty());
    // 同 usage 重复出现（报文冗余）→ 去重。
    MakeTapReport(report, 0x00F1, 0x00F1, 0x00F1);
    const auto dedup = ParseTapReportUsages(report, 9);
    assert(dedup.has_value());
    assert(*dedup == (std::vector<uint16_t>{0x00F1}));
    // 长度非法 → nullopt。
    MakeTapReport(report, 1, 2, 3);
    assert(!ParseTapReportUsages(report, 8).has_value());
    assert(!ParseTapReportUsages(report, 10).has_value());
    assert(!ParseTapReportUsages(nullptr, 9).has_value());
    assert(!ParseTapReportUsages(report, 0).has_value());
    // 前缀非法（非 01 00 00）→ nullopt。
    MakeTapReport(report, 0x00F1, 0, 0);
    report[0] = 0x02;
    assert(!ParseTapReportUsages(report, 9).has_value());
    MakeTapReport(report, 0x00F1, 0, 0);
    report[2] = 0x01;
    assert(!ParseTapReportUsages(report, 9).has_value());
}

void TestXiaomiUsageTapButtonTable() {
    // 13 键 usage 表（MiVibe FORWARD_USAGES 与本项目实测互证，方案 §1）。
    assert(XiaomiButtonFromUsage(0x00F1) == "back");
    assert(XiaomiButtonFromUsage(0x0028) == "ok");
    assert(XiaomiButtonFromUsage(0x0035) == "tv");
    assert(XiaomiButtonFromUsage(0x004A) == "home");
    assert(XiaomiButtonFromUsage(0x004F) == "right");
    assert(XiaomiButtonFromUsage(0x0050) == "left");
    assert(XiaomiButtonFromUsage(0x0051) == "down");
    assert(XiaomiButtonFromUsage(0x0052) == "up");
    assert(XiaomiButtonFromUsage(0x0065) == "menu");
    assert(XiaomiButtonFromUsage(0x0066) == "power");
    assert(XiaomiButtonFromUsage(0x007F) == "volume_mute");
    assert(XiaomiButtonFromUsage(0x0080) == "volume_up");
    assert(XiaomiButtonFromUsage(0x0081) == "volume_down");
    // 未知/空 usage。
    assert(XiaomiButtonFromUsage(0x0000) == std::nullopt);
    assert(XiaomiButtonFromUsage(0x00F2) == std::nullopt);
    assert(XiaomiButtonFromUsage(0x0027) == std::nullopt);
    // 直触发三键（RC003 系统不可见：厂商页 0xFF00 报告不被 kbdhid 翻译）。
    assert(XiaomiButtonIsTapDirect("back"));
    assert(XiaomiButtonIsTapDirect("volume_up"));
    assert(XiaomiButtonIsTapDirect("volume_down"));
    // 其余键系统可见（RC003 实测），走现有 LL+Raw Input 管线。
    assert(!XiaomiButtonIsTapDirect("home"));
    assert(!XiaomiButtonIsTapDirect("tv"));
    assert(!XiaomiButtonIsTapDirect("ok"));
    assert(!XiaomiButtonIsTapDirect("volume_mute"));  // 系统可见性未定，保守不直触发
    assert(!XiaomiButtonIsTapDirect("bogus"));
}

void TestUsageTapManagerStopBounded() {
    // B2 回归：管道句柄缺 FILE_FLAG_OVERLAPPED 时 ConnectNamedPipe/ReadFile 的
    // lpOverlapped 被忽略（实为阻塞调用），stop_event_ 永远观察不到 → Stop() 不返回
    // → 析构/进程退出挂死。用例 Start 后不连客户端（恰是阻塞点），异步 Stop，5s 内
    // 必须完成；卡住则 assert 失败（abort 立即结束测试，而非拖死 CI），并刻意泄漏
    // manager 与 stopper 线程（绝不 join，否则测试进程被一并拖死）。
    auto* manager = new XiaomiUsageTapManager();
    if (!manager->Start(nullptr, nullptr, nullptr)) {
        delete manager;
        printf("TestUsageTapManagerStopBounded: SKIP (start failed)\n");
        fflush(stdout);
        return;
    }
    std::atomic<bool> stopped{false};
    std::thread stopper([manager, &stopped] {
        manager->Stop();
        stopped.store(true);
    });
    for (int i = 0; i < 500 && !stopped.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool ok = stopped.load();
    if (ok) {
        stopper.join();
        delete manager;
    } else {
        stopper.detach();  // 卡住：不 join；manager 泄漏，随进程退出回收
    }
    assert(ok && "XiaomiUsageTapManager::Stop() must return within 5s");
}

void TestCoordinatorXiaomiCapabilityGating() {
    const std::string kRc = "RC-3A7F";
    const std::string kVs = "VS-5A74";
    auto make_seeded_config = [&]() {
        AppConfig config = AppConfig::Defaults();
        config.paired_device_ids.clear(); // 避免 CheckFirmwareUpdatesIfNeeded 起网络线程
        PairedDeviceEntry rc_entry;
        rc_entry.device_id = kRc;
        rc_entry.hardware = std::string(kHardwareXiaomiRemote2Pro);
        config.paired_devices.push_back(rc_entry);
        return config;
    };
    {
        auto ble = std::make_unique<FakeBleCentral>();
        auto* ble_ptr = ble.get();
        auto asr = std::make_unique<FakeAsrClient>();
        FakeUi ui;
        FakeInputInjector input;
        VoiceStickCoordinator coordinator(make_seeded_config(), std::move(ble), std::move(asr),
                                          &ui, &input);
        coordinator.Start();

        auto targets_device = [](const std::optional<std::string>& id, const std::string& dev) {
            return id.has_value() && *id == dev;
        };
        auto assert_no_sends_to = [&](const std::string& dev) {
            for (const auto& [_, id] : ble_ptr->sent_tap_enabled) {
                assert(!targets_device(id, dev));
            }
            for (const auto& item : ble_ptr->sent_tap_sensitivities) {
                assert(!targets_device(item.device_id, dev));
            }
            for (const auto& item : ble_ptr->sent_imu_wake_sensitivities) {
                assert(!targets_device(item.device_id, dev));
            }
            for (const auto& [_, id] : ble_ptr->sent_encoder_led_colors) {
                assert(!targets_device(id, dev));
            }
            for (const auto& [_, id] : ble_ptr->sent_encoder_recording_gates) {
                assert(!targets_device(id, dev));
            }
        };

        // 小米遥控器连接事件（hardware 由 BLE 层按类填充，镜像真实路径）。
        ble_ptr->connected_device_ids.insert(kRc);
        ble_ptr->on_connection_change(
            {ConnectedDevice{kRc, "RC-3A7F", std::string(kHardwareXiaomiRemote2Pro)}});

        // 连接同步循环：RC 设备不应收到任何交互/编码器单播（广播不在此断言）。
        assert_no_sends_to(kRc);

        // UpdateConfig 热更循环同样跳过 RC 设备（此时仅 RC 连接，应零新增）。
        const auto tap_count = ble_ptr->sent_tap_enabled.size();
        const auto tap_sens_count = ble_ptr->sent_tap_sensitivities.size();
        const auto imu_wake_count = ble_ptr->sent_imu_wake_sensitivities.size();
        const auto led_count = ble_ptr->sent_encoder_led_colors.size();
        const auto gate_count = ble_ptr->sent_encoder_recording_gates.size();
        coordinator.UpdateConfig(make_seeded_config());
        assert(ble_ptr->sent_tap_enabled.size() == tap_count);
        assert(ble_ptr->sent_tap_sensitivities.size() == tap_sens_count);
        assert(ble_ptr->sent_imu_wake_sensitivities.size() == imu_wake_count);
        assert(ble_ptr->sent_encoder_led_colors.size() == led_count);
        assert(ble_ptr->sent_encoder_recording_gates.size() == gate_count);

        // 固件更新门控：本地文件 OTA 直接拒绝，不触达底层 BLE。
        const auto fw_path =
            std::filesystem::temp_directory_path() / "voicestick_xiaomi_gating_test.bin";
        {
            std::ofstream f(fw_path, std::ios::binary);
            f << "ota-image";
        }
        bool rc_completion_called = false;
        bool rc_completion_ok = true;
        coordinator.UpdateFirmwareFromFile(
            fw_path.string(), kRc, nullptr,
            [&](bool ok, std::string message) {
                rc_completion_called = true;
                rc_completion_ok = ok;
                assert(message.find("does not support") != std::string::npos);
            });
        assert(rc_completion_called);
        assert(!rc_completion_ok);
        assert(ble_ptr->captured_firmware_device_id.empty());

        // 对照组：StickS3 设备正常下发、正常触达固件更新底层。
        ble_ptr->connected_device_ids.insert(kVs);
        ble_ptr->on_connection_change(
            {ConnectedDevice{kRc, "RC-3A7F", std::string(kHardwareXiaomiRemote2Pro)},
             ConnectedDevice{kVs, "VS-5A74"}});
        bool vs_got_sends = false;
        for (const auto& [_, id] : ble_ptr->sent_tap_enabled) {
            if (targets_device(id, kVs)) vs_got_sends = true;
        }
        for (const auto& [_, id] : ble_ptr->sent_encoder_led_colors) {
            if (targets_device(id, kVs)) vs_got_sends = true;
        }
        assert(vs_got_sends);
        assert_no_sends_to(kRc); // RC 依旧零下发

        bool vs_completion_called = false;
        coordinator.UpdateFirmwareFromFile(fw_path.string(), kVs, nullptr,
                                           [&](bool ok, std::string) {
                                               vs_completion_called = true;
                                               assert(ok);
                                           });
        assert(vs_completion_called);
        assert(ble_ptr->captured_firmware_device_id == kVs);

        std::filesystem::remove(fw_path);
        coordinator.Shutdown();
    }
}

void TestPcmRingBufferWriteRead() {
    PcmRingBuffer buffer(1024);
    std::vector<int16_t> in(100);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);

    assert(buffer.Write(in.data(), in.size()) == in.size());
    assert(buffer.Available() == in.size());

    std::vector<int16_t> out(100, 0);
    assert(buffer.Read(out.data(), out.size()) == in.size());
    assert(in == out);
    assert(buffer.Available() == 0);
}

void TestPcmRingBufferOverwrite() {
    PcmRingBuffer buffer(16);
    std::vector<int16_t> first(16, 1);
    std::vector<int16_t> second(16, 2);

    assert(buffer.Write(first.data(), first.size()) == first.size());
    assert(buffer.Write(second.data(), second.size()) == second.size());
    assert(buffer.Available() == 16);

    std::vector<int16_t> out(16, 0);
    assert(buffer.Read(out.data(), out.size()) == 16);
    assert(std::all_of(out.begin(), out.end(), [](int16_t v) { return v == 2; }));
}

void TestPcmRingBufferUnderrunSilence() {
    PcmRingBuffer buffer(16);
    std::vector<int16_t> in(4, 42);
    buffer.Write(in.data(), in.size());

    std::vector<int16_t> out(8, 0);
    assert(buffer.Read(out.data(), out.size(), /*silence_value=*/-1) == 4);
    assert(std::equal(out.begin(), out.begin() + 4, in.begin()));
    assert(std::all_of(out.begin() + 4, out.end(), [](int16_t v) { return v == -1; }));
}

void TestPcmRingBufferClear() {
    PcmRingBuffer buffer(16);
    std::vector<int16_t> in(8, 7);
    buffer.Write(in.data(), in.size());
    buffer.Clear();
    assert(buffer.Available() == 0);

    std::vector<int16_t> out(8, 0);
    assert(buffer.Read(out.data(), out.size(), /*silence_value=*/0) == 0);
    assert(std::all_of(out.begin(), out.end(), [](int16_t v) { return v == 0; }));
}

void TestPcmRingBufferWrapAround() {
    PcmRingBuffer buffer(16);
    std::vector<int16_t> in(12);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);

    // 先写 12 个再读 12 个，把 read_pos_ 推到 12。
    buffer.Write(in.data(), in.size());
    std::vector<int16_t> tmp(12, 0);
    buffer.Read(tmp.data(), tmp.size());

    // 再写 12 个，跨越尾部与头部。
    std::vector<int16_t> in2(12);
    for (std::size_t i = 0; i < in2.size(); ++i) in2[i] = static_cast<int16_t>(100 + i);
    buffer.Write(in2.data(), in2.size());

    std::vector<int16_t> out(12, 0);
    assert(buffer.Read(out.data(), out.size()) == 12);
    assert(in2 == out);
}

void TestWasapiRendererFailsOnMissingDevice() {
    WasapiVirtualMicRenderer::Options options;
    options.device_name_substring = L"DefinitelyNotARealDeviceNameXYZ123";
    WasapiVirtualMicRenderer renderer(options);
    PcmRingBuffer buffer(1024);

    assert(!renderer.IsRunning());
    assert(renderer.ActiveDeviceName().empty());
    assert(!renderer.Start(&buffer));
    assert(!renderer.IsRunning());
    assert(renderer.ActiveDeviceName().empty());
}

// 空设备名不得回退默认播放设备：语音 PCM 会从扬声器实时播出（用户听到自己
// 声音的回放，2026-09-11 事故——config.toml 两个虚拟麦字段为空时整个
// wechat 模式渲染到扬声器）。必须显式失败走「未找到虚拟麦克风」提示路径。
void TestWasapiRendererRejectsEmptyDeviceName() {
    WasapiVirtualMicRenderer::Options options;
    options.device_name_substring = L"";
    WasapiVirtualMicRenderer renderer(options);
    PcmRingBuffer buffer(1024);

    assert(!renderer.Start(&buffer));
    assert(!renderer.IsRunning());
    assert(renderer.ActiveDeviceName().empty());
}

void TestRenderPumpSubmitsFullAvailableNoCap() {
    // padding=0 → available=buffer_frame_count。事件驱动渲染去掉 frames_per_period 上限，
    // 应一次性提交全部可用空间（旧实现被 10ms=160 帧上限锁死，提交速率 < 消费速率致
    // WASAPI 稳态 underrun，输出被静音切断 → 第三方输入法识别卡顿）。
    FakeWasapiRenderSink sink(/*buffer_frames=*/800, /*channels=*/1);
    PcmRingBuffer ring(2048);
    std::vector<int16_t> samples(800);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<int16_t>(100 + i);
    }
    ring.Write(samples.data(), samples.size());

    RenderPump pump(&sink, &ring, /*channels=*/1);
    const UINT32 submitted = pump.PumpOnce();

    assert(submitted == 800);
    assert(sink.submitted_frame_counts.size() == 1);
    assert(sink.submitted_frame_counts[0] == 800);
    assert(sink.submitted_samples[0] == samples);
}

void TestRenderPumpFillsSilenceWhenRingEmpty() {
    // ring buffer 数据不足时用静音填满提交量，避免 WASAPI 播放残留旧数据或 pop 噪声。
    FakeWasapiRenderSink sink(800, 1);
    PcmRingBuffer ring(2048);  // 默认空。
    RenderPump pump(&sink, &ring, 1);

    const UINT32 submitted = pump.PumpOnce();

    assert(submitted == 800);
    assert(sink.submitted_samples.size() == 1);
    assert(sink.submitted_samples[0].size() == 800);
    for (int16_t s : sink.submitted_samples[0]) {
        assert(s == 0);
    }
}

void TestRenderPumpZeroWhenBufferFull() {
    // buffer 已满（padding == buffer_frame_count）→ 无可提交空间 → 返回 0 且不调 GetBuffer。
    FakeWasapiRenderSink sink(800, 1);
    sink.padding_ = 800;
    PcmRingBuffer ring(2048);
    std::vector<int16_t> samples(800, 1234);
    ring.Write(samples.data(), samples.size());
    RenderPump pump(&sink, &ring, 1);

    const UINT32 submitted = pump.PumpOnce();

    assert(submitted == 0);
    assert(sink.submitted_frame_counts.empty());
}

void TestWasapiRendererStopsCleanlyWakingBlockedThread() {
    // 事件驱动渲染线程阻塞于 WaitForSingleObject(NotifyEvent, INFINITE)。Stop 必须
    // SetEvent 唤醒并 join，否则死等。注入 FakeWasapiRenderSink（事件真实但不会自动
    // 触发），渲染线程将一直阻塞，验证 Stop 能唤醒线程退出（若未唤醒则 ctest
    // --timeout 会杀掉本测试判失败）。
    auto sink_owner = std::make_unique<FakeWasapiRenderSink>(800, 1);
    PcmRingBuffer ring(2048);
    WasapiVirtualMicRenderer renderer({}, std::move(sink_owner));

    assert(renderer.Start(&ring));
    assert(renderer.IsRunning());
    // 让渲染线程进入 WaitForSingleObject 阻塞。
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    renderer.Stop();
    assert(!renderer.IsRunning());
}

void TestWasapiRendererRestartsAfterStop() {
    // coordinator 的 wechat_renderer_ 在 session 间复用：Start→Stop→Start 必须可用。
    // 旧实现 Stop 里 sink_.reset() 销毁 sink，第二次 Start 调 sink_->OpenAndInitialize
    // 解引用 nullptr → 0xc0000005 访问违例（真机 session 2 崩溃，WER 02:02:55）。
    auto sink_owner = std::make_unique<FakeWasapiRenderSink>(800, 1);
    auto* sink = sink_owner.get();
    PcmRingBuffer ring(2048);
    WasapiVirtualMicRenderer renderer({}, std::move(sink_owner));

    assert(renderer.Start(&ring));
    assert(sink->open_call_count == 1);
    renderer.Stop();

    // 第二次 Start：sink 必须仍可用（Stop 不应销毁 sink）。
    assert(renderer.Start(&ring));
    assert(renderer.IsRunning());
    assert(sink->open_call_count == 2);
    renderer.Stop();
}

void TestOggOpusDemuxerParsesOpusHead() {
    // muxer 产出单帧流，demuxer 解析后验证 OpusHead 字段与 packet 完整性。
    OggOpusMuxer muxer(16000, 1);
    const ByteVector opus_frame = {0xAB, 0xCD, 0x12, 0x34, 0x56};
    const ByteVector ogg = muxer.Append(opus_frame, /*is_last=*/true);

    OggOpusStream stream;
    assert(ParseOggOpus(ogg, stream));
    assert(stream.sample_rate == 16000);
    assert(stream.channels == 1);
    assert(stream.preskip == 312);
    assert(stream.packets.size() == 1);
    assert(stream.packets[0] == opus_frame);
}

void TestOggOpusDemuxerMultiplePackets() {
    // 多帧按写入顺序解析，packet 逐字节一致。
    OggOpusMuxer muxer(16000, 1);
    const ByteVector a = {0x11, 0x11}, b = {0x22, 0x22, 0x22}, c = {0x33};
    ByteVector ogg;
    auto add = [&](const ByteVector& p, bool last) {
        const ByteVector chunk = muxer.Append(p, last);
        ogg.insert(ogg.end(), chunk.begin(), chunk.end());
    };
    add(a, false);
    add(b, false);
    add(c, true);

    OggOpusStream stream;
    assert(ParseOggOpus(ogg, stream));
    assert(stream.packets.size() == 3);
    assert(stream.packets[0] == a);
    assert(stream.packets[1] == b);
    assert(stream.packets[2] == c);
}

void TestOggOpusDemuxerRoundTripWithFinish() {
    // muxer.Finish 产生的 EOS 空页不影响已收 packet 的解析。
    OggOpusMuxer muxer(16000, 1);
    const ByteVector a = {0x77, 0x88};
    ByteVector ogg = muxer.Append(a, false);
    const ByteVector eos = muxer.Finish();
    ogg.insert(ogg.end(), eos.begin(), eos.end());

    OggOpusStream stream;
    assert(ParseOggOpus(ogg, stream));
    assert(stream.packets.size() == 1);
    assert(stream.packets[0] == a);
}

void TestOggOpusDemuxerRejectsBadMagic() {
    const ByteVector bad(100, 0);  // 无 OggS magic。
    OggOpusStream stream;
    assert(!ParseOggOpus(bad, stream));
}

void TestOggOpusDemuxerRejectsTruncatedStream() {
    OggOpusMuxer muxer(16000, 1);
    const ByteVector payload = {0xAB, 0xCD};
    const ByteVector ogg_full = muxer.Append(payload, true);
    ByteVector ogg(ogg_full.begin(), ogg_full.begin() + 10);  // 页头都未完整。
    OggOpusStream stream;
    assert(!ParseOggOpus(ogg, stream));
    assert(stream.packets.empty());
}

void TestWechatPipelineSteadyStateLatency() {
    // 稳态：每 20ms 写一帧 PCM（320 样本 @16kHz），设备消费 20ms，PumpOnce 填 buffer。
    // 管道滞留（ring+padding）稳态 ≈ buffer_frames，端到端延迟 ≈ buffer_duration_ms。
    // 量化 WASAPI buffer 对首字延迟的贡献（真机 buffer_duration_ms=50 -> 约 50ms）。
    const int sr = 16000;
    const int frame_ms = 20;
    const int frame_samples = sr * frame_ms / 1000;  // 320
    const int buffer_ms = 50;
    TimedFakeSink sink(sr, buffer_ms);
    PcmRingBuffer ring(8192);
    RenderPump pump(&sink, &ring, 1);

    std::vector<int16_t> pcm(frame_samples, 1);
    int ring_underrun = 0;
    UINT32 max_backlog = 0;
    for (int i = 0; i < 100; ++i) {
        ring.Write(pcm.data(), static_cast<std::size_t>(frame_samples));
        const UINT32 before = static_cast<UINT32>(ring.Available());
        const UINT32 submitted = pump.PumpOnce();
        const UINT32 after = static_cast<UINT32>(ring.Available());
        if (before - after < submitted) ++ring_underrun;  // ring 不足补静音
        const UINT32 backlog = static_cast<UINT32>(ring.Available()) + sink.padding_;
        if (backlog > max_backlog) max_backlog = backlog;
        sink.AdvanceTimeUs(frame_ms * 1000);  // 设备消费在填之后，匹配事件驱动
    }
    const double max_latency_ms = static_cast<double>(max_backlog) / sr * 1000.0;
    // buffer 50ms 在 20ms 帧节奏下余量充足，device 不应饿。
    assert(sink.device_underrun_count_ == 0);
    // 首帧 ring 仅 320 < buffer 800，必有一次 ring underrun（补静音填满 buffer）。
    assert(ring_underrun >= 1);
    // 稳态滞留 ≈ buffer（WASAPI buffer 是管道延迟主因），允许首帧 + 一帧波动。
    assert(max_backlog >= sink.buffer_frames_);
    assert(max_backlog <= sink.buffer_frames_ + static_cast<UINT32>(frame_samples));
    assert(max_latency_ms >= buffer_ms - 1.0);
    assert(max_latency_ms <= buffer_ms + frame_ms + 1.0);
}

void TestWechatPipelineBufferDurationPareto() {
    // 帕累托：buffer_duration_ms 越小管道延迟越低，但 < 帧节奏时 device underrun。
    // 量化各档延迟与 underrun，为阶段6 调优 50->20ms 提供数据支撑。
    const int sr = 16000;
    const int frame_ms = 20;
    const int frame_samples = sr * frame_ms / 1000;
    std::vector<int16_t> pcm(frame_samples, 1);
    double prev_latency = -1.0;
    for (const int buffer_ms : {20, 50, 100}) {
        TimedFakeSink sink(sr, buffer_ms);
        PcmRingBuffer ring(8192);
        RenderPump pump(&sink, &ring, 1);
        UINT32 max_backlog = 0;
        for (int i = 0; i < 200; ++i) {
            ring.Write(pcm.data(), static_cast<std::size_t>(frame_samples));
            pump.PumpOnce();
            const UINT32 backlog = static_cast<UINT32>(ring.Available()) + sink.padding_;
            if (backlog > max_backlog) max_backlog = backlog;
            sink.AdvanceTimeUs(frame_ms * 1000);
        }
        const double latency_ms = static_cast<double>(max_backlog) / sr * 1000.0;
        std::printf("[wechat-pareto] buffer_ms=%-3d max_latency=%6.1fms device_underrun=%d\n",
                    buffer_ms, latency_ms, sink.device_underrun_count_);
        // buffer >= 帧节奏(20ms) 时 device 不饿，延迟随 buffer_ms 递增。
        assert(sink.device_underrun_count_ == 0);
        assert(latency_ms >= prev_latency - 1.0);
        prev_latency = latency_ms;
    }
}

void TestWechatPipelineSmallBufferDeviceUnderrun() {
    // buffer_duration_ms < 帧节奏（20ms）时，设备单周期消费 > buffer 容量，device underrun。
    // 佐证 buffer_duration_ms 不可小于帧间隔，阶段6 调优下限为 20ms。
    const int sr = 16000;
    TimedFakeSink sink(sr, /*buffer_duration_ms=*/5);
    PcmRingBuffer ring(8192);
    RenderPump pump(&sink, &ring, 1);
    std::vector<int16_t> pcm(sr * 20 / 1000, 1);
    for (int i = 0; i < 50; ++i) {
        ring.Write(pcm.data(), pcm.size());
        pump.PumpOnce();
        sink.AdvanceTimeUs(20 * 1000);
    }
    assert(sink.device_underrun_count_ > 0);
}

void TestRingBurstBacklogAmplifiesLatency() {
    // 固件冷启动后可能突发发送 init_codec 期间缓冲的音频，一次到达多帧。
    // ring 接住积压，稳态消费 1:1 不 drain（生产=消费），积压持续，音频延迟
    // N ms 到达微信，放大首字与全程延迟。
    const int sr = 16000;
    const int frame_ms = 20;
    const int frame_samples = sr * frame_ms / 1000;  // 320
    TimedFakeSink sink(sr, 50);
    PcmRingBuffer ring(8192);
    RenderPump pump(&sink, &ring, 1);
    std::vector<int16_t> pcm(frame_samples, 1);

    // 突发 25 帧（500ms）。
    for (int i = 0; i < 25; ++i) {
        ring.Write(pcm.data(), static_cast<std::size_t>(frame_samples));
    }
    pump.PumpOnce();  // 填 buffer 800，ring 减 800
    // 积压 7200 samples = 450ms，稳态不 drain（生产=消费）。
    const double backlog_ms = static_cast<double>(ring.Available()) / sr * 1000.0;
    assert(backlog_ms >= 440.0 && backlog_ms <= 460.0);

    // 稳态 20 周期：每周期写 320 消费 320，积压不变。
    for (int i = 0; i < 20; ++i) {
        ring.Write(pcm.data(), static_cast<std::size_t>(frame_samples));
        pump.PumpOnce();
        sink.AdvanceTimeUs(frame_ms * 1000);
    }
    const double backlog_after = static_cast<double>(ring.Available()) / sr * 1000.0;
    // 稳态生产=消费，积压不 drain，延迟持续放大 N ms。
    assert(backlog_after > 400.0);
    // 结论：真机日志首帧后稳态无积压，故 ring 积压非真机观测主因；
    // 但冷启动突发是理论风险，ring capacity 是延迟放大上限。
}

void TestRingBacklogUpperBoundByCapacity() {
    // ring capacity 8192 samples = 512ms @16kHz。突发超过 capacity 时 drop-oldest，
    // 积压上限 512ms。结合 WASAPI buffer 50ms，管道最大滞留约 562ms，
    // 仍不足 1-2 秒，佐证 1-2 秒主因在下游（VB-CABLE/微信 ASR）。
    const int sr = 16000;
    PcmRingBuffer ring(8192);
    std::vector<int16_t> pcm(20000, 1);  // 远超 capacity
    ring.Write(pcm.data(), pcm.size());
    assert(ring.Available() == 8192);
    const double max_backlog_ms = static_cast<double>(ring.Available()) / sr * 1000.0;
    assert(max_backlog_ms >= 511.0 && max_backlog_ms <= 513.0);
}

} // namespace

void TestOutputTargetWechatInputMethod() {
    assert(OutputTargetFromName("wechat_input_method") == OutputTarget::kWechatInputMethod);
    assert(OutputTargetFromName("subtitle") == OutputTarget::kSubtitle);
    assert(OutputTargetFromName("focused_app") == OutputTarget::kFocusedApp);
    assert(OutputTargetFromName("unknown") == OutputTarget::kFocusedApp);
    assert(OutputTargetName(OutputTarget::kWechatInputMethod) == "wechat_input_method");
    assert(OutputTargetName(OutputTarget::kSubtitle) == "subtitle");
    assert(OutputTargetName(OutputTarget::kFocusedApp) == "focused_app");
}

void TestWechatInputMethodConfigRoundTrip() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_wechat_input_method_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.hotkey_hold = "ctrl+shift+w";
    config.wechat_input_method.hotkey_click = "ralt";
    config.wechat_input_method.virtual_mic_playback_name = "CABLE Input";
    config.wechat_input_method.virtual_mic_capture_name = "CABLE Output Test";
    config.wechat_input_method.auto_switch_default_recording_device = true;
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.default_output_profile.target == OutputTarget::kWechatInputMethod);
    assert(loaded.wechat_input_method.hotkey_hold == "ctrl+shift+w");
    assert(loaded.wechat_input_method.hotkey_click == "ralt");
    assert(loaded.wechat_input_method.virtual_mic_playback_name == "CABLE Input");
    assert(loaded.wechat_input_method.virtual_mic_capture_name == "CABLE Output Test");
    assert(loaded.wechat_input_method.auto_switch_default_recording_device);

    std::filesystem::remove(temp);
}

void TestWechatInputMethodPerModeHotkeyRoundTrip() {
    // 两套热键独立保存：改 hold 不影响 click，反之亦然。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_per_mode_hotkey_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.wechat_input_method.hotkey_hold = "ctrl+win";
    config.wechat_input_method.hotkey_click = "ralt";
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.wechat_input_method.hotkey_hold == "ctrl+win");
    assert(loaded.wechat_input_method.hotkey_click == "ralt");

    // 改 hold 再往返，click 应保持不变。
    loaded.wechat_input_method.hotkey_hold = "ctrl+shift+w";
    loaded.Save(temp);
    AppConfig loaded2 = AppConfig::Load(temp);
    assert(loaded2.wechat_input_method.hotkey_hold == "ctrl+shift+w");
    assert(loaded2.wechat_input_method.hotkey_click == "ralt");

    std::filesystem::remove(temp);
}

void TestWechatInputMethodLegacyHotkeyFallback() {
    // 旧配置只有 hotkey 字段：加载后 hotkey_hold/hotkey_click 都回退为该值，不丢用户配置。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_legacy_hotkey_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "[wechat_input_method]\nhotkey = \"ctrl+shift+w\"\n";
    }

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.wechat_input_method.hotkey_hold == "ctrl+shift+w");
    assert(loaded.wechat_input_method.hotkey_click == "ctrl+shift+w");

    std::filesystem::remove(temp);
}

void TestWechatInputMethodActiveHotkeyByMode() {
    WechatInputMethodConfig c;
    c.hotkey_hold = "ctrl+win";
    c.hotkey_click = "ralt";
    assert(c.ActiveHotkey(InteractionMode::kHoldToTalk) == "ctrl+win");
    assert(c.ActiveHotkey(InteractionMode::kHoldToTalkInstant) == "ctrl+win");
    assert(c.ActiveHotkey(InteractionMode::kClickToTalk) == "ralt");
}

// 旧配置迁移：[wechat_input_method] 缺 trigger_mode 字段时，从顶层 interaction_mode 继承
//（保留用户为 wechat 选的点按式），顶层 interaction_mode 重置为 kHoldToTalk（focused_app/
// 字幕不再继承 wechat 点按式，修复切输出目标后长按失效）。
void TestWechatTriggerModeMigratedFromLegacyInteractionMode() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_wechat_trigger_migrate_test.toml";
    {
        std::ofstream out(temp);
        out << "interaction_mode = \"click_to_talk\"\n";
        out << "\n[wechat_input_method]\nhotkey_hold = \"ctrl+win\"\nhotkey_click = \"ralt\"\n";
    }
    auto loaded = AppConfig::Load(temp);
    assert(loaded.interaction_mode == InteractionMode::kHoldToTalk);
    assert(loaded.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk);
    std::filesystem::remove(temp);
}

// trigger_mode 序列化往返：Save 写入 [wechat_input_method].trigger_mode，Load 读回。
void TestWechatTriggerModeRoundTrip() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_wechat_trigger_roundtrip_test.toml";
    AppConfig config;
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;
    config.Save(temp);
    auto loaded = AppConfig::Load(temp);
    assert(loaded.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk);
    std::filesystem::remove(temp);
}

// session_model（第三方输入法会话模型：hold=按住式 WeType / click=点按式 Typeless）
// 默认值推导与显式往返：未配置时按 trigger_mode 推导（click_to_talk→click、
// hold_to_talk→hold，兼容现状零破坏）；显式配置覆盖并可往返。
void TestWechatSessionModelConfig() {
    // 未配置 + trigger=click_to_talk → 推导 click（Typeless 现状不变）。
    {
        auto temp = std::filesystem::temp_directory_path() / "vs_wechat_sm_default_click.toml";
        {
            std::ofstream out(temp);
            out << "[wechat_input_method]\ntrigger_mode = \"click_to_talk\"\n";
        }
        auto loaded = AppConfig::Load(temp);
        assert(!loaded.wechat_input_method.session_model.has_value());  // 未显式配置
        assert(loaded.wechat_input_method.EffectiveSessionModel() ==
               InteractionMode::kClickToTalk);  // 访问时按 trigger 推导
        std::filesystem::remove(temp);
    }
    // 未配置 + trigger=hold_to_talk → 推导 hold。
    {
        auto temp = std::filesystem::temp_directory_path() / "vs_wechat_sm_default_hold.toml";
        {
            std::ofstream out(temp);
            out << "[wechat_input_method]\ntrigger_mode = \"hold_to_talk\"\n";
        }
        auto loaded = AppConfig::Load(temp);
        assert(loaded.wechat_input_method.EffectiveSessionModel() ==
               InteractionMode::kHoldToTalk);
        std::filesystem::remove(temp);
    }
    // 显式 click_to_talk + session_model=hold（方案 A 组合）往返保持。
    {
        auto temp = std::filesystem::temp_directory_path() / "vs_wechat_sm_explicit.toml";
        AppConfig config;
        config.default_output_profile.target = OutputTarget::kWechatInputMethod;
        config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;
        config.wechat_input_method.session_model = InteractionMode::kHoldToTalk;
        config.Save(temp);
        auto loaded = AppConfig::Load(temp);
        assert(loaded.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk);
        assert(loaded.wechat_input_method.session_model.has_value());
        assert(*loaded.wechat_input_method.session_model == InteractionMode::kHoldToTalk);
        assert(loaded.wechat_input_method.EffectiveSessionModel() == InteractionMode::kHoldToTalk);
        std::filesystem::remove(temp);
    }
}

void TestWechatInputMethodHotkeyParsing() {
    assert(WechatInputMethodHotkey("").KeyCount() == 0);
    assert(WechatInputMethodHotkey("ctrl+win").KeyCount() == 2);
    assert(WechatInputMethodHotkey("Ctrl+Win").KeyCount() == 2);
    assert(WechatInputMethodHotkey("ctrl+shift+w").KeyCount() == 3);
    assert(WechatInputMethodHotkey("alt+f4").KeyCount() == 2);
    assert(WechatInputMethodHotkey("command+1").KeyCount() == 2);
    // 右ALT 单键：Typeless 等点按式第三方输入法靠右ALT触发。
    // 右ALT 是扩展键（VK_RMENU），需单独解析名 ralt，不能复用 alt(VK_MENU)。
    assert(WechatInputMethodHotkey("ralt").KeyCount() == 1);
    assert(WechatInputMethodHotkey("lalt").KeyCount() == 1);
    assert(WechatInputMethodHotkey("ralt+r").KeyCount() == 2);
    assert(WechatInputMethodHotkey("unknown+key").KeyCount() == 0);
}

// WeType（微信输入法）的长按检测依赖持续的键盘事件流：物理长按时操作系统
// auto-repeat 持续产生 keydown，仅注入一次 keydown 它不认为是长按、不弹语音
// 面板（2026-09-11 实证：单次注入 2.5s 无面板；40ms 周期重复注入面板弹出）。
// SendDown 必须启动重复注入线程，SendUp 停止。
void TestWechatHotkeySendDownRepeatsWhileHeld() {
    struct SendInputRecorder {
        std::atomic<int> keydown_batches{0};
        std::atomic<int> keyup_batches{0};
        UINT WINAPI Record(UINT count, LPINPUT inputs, int) {
            bool has_keyup = false;
            for (UINT i = 0; i < count; ++i) {
                if (inputs[i].type == INPUT_KEYBOARD &&
                    (inputs[i].ki.dwFlags & KEYEVENTF_KEYUP)) {
                    has_keyup = true;
                }
            }
            if (has_keyup) {
                keyup_batches.fetch_add(1);
            } else {
                keydown_batches.fetch_add(1);
            }
            return count;
        }
    };
    SendInputRecorder recorder;
    WechatInputMethodHotkey::SetSendInputForTest(
        [&recorder](UINT count, LPINPUT inputs, int size) -> UINT {
            return recorder.Record(count, inputs, size);
        });

    {
        WechatInputMethodHotkey hotkey("ctrl+win");
        assert(hotkey.SendDown());
        // 40ms 周期重复：250ms 内应产生多批 keydown（首拍 + 若干重复拍）。
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int during_hold = recorder.keydown_batches.load();
        assert(during_hold >= 3);

        assert(hotkey.SendUp());
        assert(recorder.keyup_batches.load() == 1);
        const int after_up = recorder.keydown_batches.load();
        // SendUp 后重复注入必须停止：150ms 内不再新增 keydown 批次。
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(recorder.keydown_batches.load() == after_up);
    }

    // 只 SendDown 不 SendUp 直接析构：必须停止线程安全析构，不死锁不崩溃。
    {
        WechatInputMethodHotkey hotkey("ctrl+win");
        assert(hotkey.SendDown());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    WechatInputMethodHotkey::SetSendInputForTest(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

// 小米遥控器 2 Pro 的语音键上报为 F5 HID 键。曾定案「F5 按住的异步键状态阻塞
// WeType 会话、注入 F5 keyup 可中和」（f20dcb72 NeutralizeHeldF5）——2026-09-12
// 证伪并回滚：真实机制是 WeType 要求「普通键活动静默期」，物理 F5 按住期间的
// 30ms 重复流持续刷新活动时间戳，注入 keyup 清不掉（注入本身也是活动）。
// 终版定案与方案 A（点按触发）见 Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md。

void TestCoordinatorWechatInputMethodButtonDownSendsHotkey() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.virtual_mic_playback_name = "DefinitelyNotARealDeviceXYZ";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 100));

    // 虚拟麦克风不存在，应触发错误 UI。
    assert(!ui.errors.empty());
    assert(ui.errors.back().find("Virtual microphone") != std::string::npos);
    // wechat 模式不应弹出 VoiceStick 录音悬浮窗（第三方输入法自带语音面板），
    // 启动失败时只显示错误，不弹录音浮窗，避免松开时浮窗残留。
    assert(ui.show_listening_count == 0);
    // 会话被清理后，后续状态应为 ready。
    assert(!asr_ptr->started);
}

void TestCoordinatorWechatInputMethodWritesDebugAudio() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.debug_audio_cache = true;
    const auto debug_dir =
        std::filesystem::temp_directory_path() / "voicestick_wechat_debug_audio_test";
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
    // 跨过 0.5s 最小录音时长阈值，避免被短录音过滤丢弃（与 focused_app 路径测试一致）。
    // sleep 须在音频帧之前：audio_end 帧到达即触发落盘判断，此时 duration 须已过阈值。
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 1));
    ble_ptr->on_audio_frame("5A74", AudioDataFrame(7, 2, true));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    // 勾选调试音频后，wechat 模式录音结束应在目录下落盘非空 .ogg 文件。
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
        if (entry.path().extension() == ".ogg" && std::filesystem::file_size(entry) > 0) {
            found = true;
            break;
        }
    }
    assert(found);

    std::filesystem::remove_all(debug_dir);
}

void TestDebugAudioRecorderInvalidDirectoryDoesNotCrash() {
    // 复现闪退：debug_audio_dir 指向无法创建的目录（父级是文件而非目录）时，
    // 旧实现 create_directories(directory_) 抛 filesystem_error 未捕获 -> std::terminate。
    // 调试音频是可选功能，目录无效应降级放弃落盘，不拖垮录音会话。
    const auto blocker =
        std::filesystem::temp_directory_path() / "vs_debug_audio_blocker.txt";
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "x";
    }
    assert(std::filesystem::exists(blocker));
    // blocker 是文件，在其下创建 subdir 必失败。
    const auto bad_dir = blocker / "subdir";

    DebugAudioRecorder rec(true, bad_dir);
    rec.Start("VS-TEST", 42);
    const std::uint8_t data[] = {0x01, 0x02, 0x03};
    rec.Append(data);
    rec.Finish();  // 修复前：抛异常 -> 进程 abort。修复后：降级返回。

    std::error_code ec;
    std::filesystem::remove(blocker, ec);
}

void TestAppConfigDebugAudioDirUtf8RoundTrip() {
    // 复现导入 config 含非 ASCII 路径：旧实现 path(utf8_string) 按 ACP(GBK) 解析致乱码，
    // path.string() 按 ACP 输出时若含 ACP 无法表示字符抛 system_error -> config.Save 抛
    // -> SaveSettings 无 try-catch -> std::terminate 闪退。修复后 Save 用 Utf8FromUtf16(wstring)、
    // Load 用 path(Utf16FromUtf8(value))，非 ASCII 路径 roundtrip 应保持一致且不抛。
    AppConfig config = AppConfig::Defaults();
    // "调试音频" 用 \u 转义避免源码编码依赖。
    const auto dir = std::filesystem::temp_directory_path() /
        std::filesystem::path(std::wstring(L"voicestick_调试音频"));
    config.debug_audio_directory = dir;
    const auto path = std::filesystem::temp_directory_path() / "voicestick_debug_dir_utf8_test.toml";
    config.Save(path);
    const auto loaded = AppConfig::Load(path);
    std::filesystem::remove(path);
    assert(loaded.debug_audio_directory == dir);
}

void TestWechatClickTriggerDoesNotLeakToFocusedApp() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.trigger_mode = InteractionMode::kClickToTalk;
    config.interaction_mode = InteractionMode::kHoldToTalk;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // wechat 点按式：下发 click_to_talk。
    assert(ble_ptr->sent_interaction_modes.back().first == InteractionMode::kClickToTalk);

    // 切到 focused_app：全局 interaction_mode=hold，下发 hold_to_talk（不被 wechat 点按式污染）。
    config.default_output_profile.target = OutputTarget::kFocusedApp;
    coordinator.UpdateConfig(config);
    assert(ble_ptr->sent_interaction_modes.back().first == InteractionMode::kHoldToTalk);

    // focused_app 长按主键应进录音态（固件 hold 模式下发 button_down -> HandlePrimaryButtonDown）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 1));
    assert(ui.show_listening_count >= 1);
}

// button_down 进 recording 后，若 button_up 与 audio_end 都丢失，recording 硬超时兜底
// 必须回 ready，避免永久卡 listening（focused_app 模式无 wechat 的残留自愈）。
void TestSavePairedDeviceInfoUnknownDeviceNoEntry() {
    AppConfig config = AppConfig::Defaults();
    config.SavePairedDeviceInfo("9999", "xiaomi_remote_2_pro", "");
    assert(config.paired_devices.empty());
    assert(config.paired_device_ids.empty());
}

// 方案 A 组合下 StickS3（非小米）：click 启动立即 SendDown 同样生效，但不切换
// 直连麦克风语义——音频仍来自设备 BLE 流，经首帧解码进 ring buffer + CABLE 渲染
// 的既有链路不变。
void TestCoordinatorUpdateConfigDestroysOldAsrOffThread() {
    auto ble = std::make_unique<FakeBleCentral>();
    FakeUi ui;
    FakeInputInjector input;
    // 旧客户端析构慢（模拟 ShutdownConnection join 阻塞的 worker），析构完成置信号。
    class SlowDestructAsr : public AsrClient {
    public:
        bool Start(AsrSessionOptions = {}) override { return true; }
        void SendOggOpusChunk(std::span<const std::uint8_t>, bool) override {}
        void Cancel() override {}
        ~SlowDestructAsr() override {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            destroyed->set_value();
        }
        std::shared_ptr<std::promise<void>> destroyed =
            std::make_shared<std::promise<void>>();
    };
    auto old_asr = std::make_unique<SlowDestructAsr>();
    auto destroyed_future = old_asr->destroyed->get_future().share();
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                      std::move(old_asr), &ui, &input,
                                      [](const AppConfig&) {
                                          return std::make_unique<FakeAsrClient>();
                                      });
    coordinator.Start();

    const auto begin = std::chrono::steady_clock::now();
    coordinator.UpdateConfig(AppConfig::Defaults());
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count();
    // 旧实现：调用线程同步析构旧客户端，至少耗时 300ms → 红灯。
    assert(elapsed_ms < 150);
    // 后台析构最终完成（含慢析构），2 秒兜底。
    assert(destroyed_future.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
}

// ===== AirMouseStep 纯函数测试（速度控制）=====
// 验证 v 跟随 omega×gain + 速度环低通 + 分轴 gain + 亚像素累积的核心运动学。

// omega 恒定，v 跟随 v_target=omega×gain（速度环收敛，v 单调趋向 v_target）。
void TestAirMouseStepVelocityFollowsOmega() {
    AirMouseKinState s;
    AirMouseParams p;
    AirMouseStep(s, AirMouseInput{10, 0, false}, 0.016, false, p);
    const double v1 = s.vx;
    AirMouseStep(s, AirMouseInput{10, 0, false}, 0.016, false, p);
    const double v2 = s.vx;
    assert(v1 > 0.0);
    assert(v2 > v1);  // 速度环收敛，v 增长
}

// 速度控制：stale（手停）后 v_target=0，v 经 tau 快速归零（手停即停）。
// 旧角度模型 decay_tau 慢衰减致 v 不归零，此测试约束速度模型。
void TestAirMouseStepStopsWhenStale() {
    AirMouseKinState s;
    AirMouseParams p;
    p.tau = 0.05;
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, false}, 0.016, false, p);  // 转动积累 v
    assert(s.vx > 1.0);  // 转动中有速度
    for (int i = 0; i < 100; ++i) AirMouseStep(s, AirMouseInput{10, 0, false}, 0.016, true, p);  // stale 1.6s
    assert(std::fabs(s.vx) < 1.0);  // v 快速归零（手停即停）
}

// ===== 三段线性增益曲线测试 =====
// v_target = omega × gain × factor(|omega|, curve)，factor 三段线性（curve 见 air_mouse_kin.h）：
//   平滑 sigmoid：|omega|→0 趋近 low_factor，|omega|→∞ 趋近 high_factor，全程无折角（P2）。
// 测试引用 p.curve.*（运行期参数），约束曲线形状、连续性、curve 注入与 clamp。

// 微调段：omega=low_thresh/2，稳态 vx ≈ omega×gain×factor(omega)（factor 走 sigmoid，非硬等于 low_factor）。
void TestAirMouseStepGainCurveLowRange() {
    AirMouseKinState s;
    AirMouseParams p;  // 默认 gain_x=16, tau=0.05, curve={100,333,0.25,4.0}
    const int omega = static_cast<int>(p.curve.low_thresh / 2.0);  // 微调段内
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    const double factor = AirMouseGainFactor(static_cast<double>(omega), p.curve);
    const double v_target = omega * p.gain_x * factor;
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
}

// 甩动段：omega=high_thresh×2，稳态 vx ≈ omega×gain×high_factor。
void TestAirMouseStepGainCurveHighRange() {
    AirMouseKinState s;
    AirMouseParams p;
    const int omega = static_cast<int>(p.curve.high_thresh * 2.0);  // 甩动段内
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    const double v_target = omega * p.gain_x * p.curve.high_factor;
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
}

// 中段：omega=中点(mid)，sigmoid 在 mid 处恰为 (low+high)/2，稳态 vx ≈ omega×gain×factor。
void TestAirMouseStepGainCurveMidRange() {
    AirMouseKinState s;
    AirMouseParams p;
    const int omega = static_cast<int>((p.curve.low_thresh + p.curve.high_thresh) / 2.0);
    const double factor = AirMouseGainFactor(static_cast<double>(omega), p.curve);
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    const double v_target = omega * p.gain_x * factor;
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
}

// 曲线形状：单调 + 微调段单位增益 < 甩动段单位增益（慢稳快猛）。
void TestAirMouseStepGainCurveShape() {
    AirMouseParams p;
    auto steady_v = [&](int omega) {
        AirMouseKinState s;
        for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
        return s.vx;
    };
    const int w_low = static_cast<int>(p.curve.low_thresh / 2.0);
    const int w_high = static_cast<int>(p.curve.high_thresh * 2.0);
    assert(steady_v(w_low) < steady_v(w_high));                   // 单调
    assert(steady_v(w_low) / w_low < steady_v(w_high) / w_high);  // 微调段斜率 < 甩动段
}

// 连续无折角（P2 sigmoid）：omega 跨越 low_thresh 时 factor 平滑过渡、无跳变；
// 拐点处 factor 落在 (low_factor, high_factor) 之间（非硬等于 low_factor）。
void TestAirMouseStepGainCurveContinuousAtLowThreshold() {
    AirMouseKinState s;
    AirMouseParams p;
    const int omega = static_cast<int>(p.curve.low_thresh);  // 特征点
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    const double f_at = AirMouseGainFactor(static_cast<double>(omega), p.curve);
    // 无上跳：拐点值严格在 (low_factor, high_factor) 内
    assert(f_at > p.curve.low_factor);
    assert(f_at < p.curve.high_factor);
    // 跨拐点连续：±1 单位内 factor 变化很小（无折角跳变）
    const double f_below = AirMouseGainFactor(static_cast<double>(omega - 1), p.curve);
    const double f_above = AirMouseGainFactor(static_cast<double>(omega + 1), p.curve);
    assert(std::fabs(f_above - f_below) < 0.1);
}

// 负向对称：omega=-high_thresh×2 稳态 |vx| ≈ omega×gain×high_factor（factor 用 |omega|）。
void TestAirMouseStepGainCurveNegative() {
    AirMouseParams p;
    const int omega = static_cast<int>(p.curve.high_thresh * 2.0);
    AirMouseKinState sp, sn;
    for (int i = 0; i < 200; ++i) {
        AirMouseStep(sp, AirMouseInput{omega, 0, false}, 0.016, false, p);
        AirMouseStep(sn, AirMouseInput{-omega, 0, false}, 0.016, false, p);
    }
    const double v_target = omega * p.gain_x * p.curve.high_factor;
    assert(std::fabs(sp.vx - v_target) < std::fabs(v_target) * 0.05);
    assert(std::fabs(sn.vx + v_target) < std::fabs(v_target) * 0.05);  // 负向对称
    assert(sn.vx < 0.0);
}

// ===== 曲线参数运行期化（热调参）测试 =====

// curve 注入：自定义 curve 下 factor 与默认 curve 不同，证明 curve 参数生效。
// 红灯：stub 忽略 curve，f_custom==f_default，断言失败；绿灯：实现用 curve.* 后通过。
void TestAirMouseGainFactorAcceptsCurveParams() {
    AirMouseCurveParams c;
    c.low_thresh = 10.0;
    c.high_thresh = 30.0;
    c.low_factor = 0.2;
    c.high_factor = 5.0;
    // 自定义曲线 omega=20 = 中点(mid)：sigmoid 在中点恰为 (0.2+5.0)/2 = 2.6
    // 默认曲线 omega=20 落低区：sigmoid 平滑地板，factor≈0.377（> low_factor 0.25，无硬等于）
    const double f_custom = AirMouseGainFactor(20.0, c);
    const double f_default = AirMouseGainFactor(20.0, AirMouseCurveParams{});
    assert(std::fabs(f_custom - 2.6) < 0.02);
    assert(std::fabs(f_default - 0.377) < 0.02);
    assert(std::fabs(f_custom - f_default) > 1.0);  // curve 注入确实改变 factor
}

// 默认曲线 sigmoid 性质（回归保护）：单调、有界、中点对称、低区地板 > low_factor。
void TestAirMouseGainFactorDefaultCurveMatchesLegacy() {
    AirMouseCurveParams c;  // 默认 {100, 333, 0.25, 4.0}（阈值单位=固件缩放角速率 dps×4）
    // 中点 (100+333)/2 = 216.5：tanh(0)=0 → factor = 0.25 + 3.75*0.5 = 2.125（精确）
    assert(std::fabs(AirMouseGainFactor(216.5, c) - 2.125) < 1e-9);
    // 甩动段外（≥333）：趋近 high_factor=4.0
    assert(std::fabs(AirMouseGainFactor(1000.0, c) - 4.0) < 0.01);
    // 低区（<100）：平滑地板，略高于 low_factor（0.25）但远低于高段
    const double f_low = AirMouseGainFactor(5.0, c);
    assert(f_low > 0.25);
    assert(f_low < 0.5);
    // 单调性：低区 < 特征点 < 中段 < 高特征点 < 外段
    assert(AirMouseGainFactor(5.0, c) < AirMouseGainFactor(100.0, c));
    assert(AirMouseGainFactor(100.0, c) < AirMouseGainFactor(216.5, c));
    assert(AirMouseGainFactor(216.5, c) < AirMouseGainFactor(333.0, c));
    assert(AirMouseGainFactor(333.0, c) < AirMouseGainFactor(1000.0, c));
}

// step 用 params.curve：同 omega、不同 curve → 不同稳态 vx。
void TestAirMouseStepUsesCurveParams() {
    AirMouseParams p_low;   // 默认 curve low_factor=0.25
    AirMouseParams p_high;  // 高 low_factor
    p_high.curve.low_factor = 0.5;
    const int omega = 5;  // 微调段内
    AirMouseKinState s_low, s_high;
    for (int i = 0; i < 200; ++i) {
        AirMouseStep(s_low, AirMouseInput{omega, 0, false}, 0.016, false, p_low);
        AirMouseStep(s_high, AirMouseInput{omega, 0, false}, 0.016, false, p_high);
    }
    // 同 omega 不同 low_factor：higher low_factor 抬高低区地板 → 更跟手，vx 更大（sigmoid 下约 1.7×）
    assert(s_high.vx > s_low.vx * 1.4);
}

// clamp：越界值钳位到合法范围，且保证 low_thresh < high_thresh。
void TestAirMouseCurveClamp() {
    AirMouseCurveParams c;
    c.low_thresh = 0.0;       // 低于下限 1.0
    c.high_thresh = 1000.0;   // 高于上限 800.0
    c.low_factor = -1.0;      // 低于下限 0.05
    c.high_factor = 100.0;    // 高于上限 6.0
    const auto clamped = AirMouseCurveClamp(c);
    assert(clamped.low_thresh >= 1.0);
    assert(clamped.high_thresh <= 800.0);
    assert(clamped.low_factor >= 0.05);
    assert(clamped.high_factor <= 6.0);
    assert(clamped.low_thresh < clamped.high_thresh);  // 不变式
}

// clamp：low_thresh ≥ high_thresh 时强制 low < high（防中段除零）。
void TestAirMouseCurveClampLowBelowHigh() {
    AirMouseCurveParams c;
    c.low_thresh = 60.0;   // 高于 high_thresh 默认 50
    c.high_thresh = 50.0;
    const auto clamped = AirMouseCurveClamp(c);
    assert(clamped.low_thresh < clamped.high_thresh);
}

// 分轴 gain：gain_x ≠ gain_y 时，同 omega 下 vx ≠ vy。
void TestAirMouseStepAxisGain() {
    AirMouseParams p;
    p.gain_x = 10.0;
    p.gain_y = 4.0;
    AirMouseKinState sx, sy;
    for (int i = 0; i < 50; ++i) {
        AirMouseStep(sx, AirMouseInput{10, 0, false}, 0.016, false, p);
        AirMouseStep(sy, AirMouseInput{0, 10, false}, 0.016, false, p);
    }
    assert(sx.vx > sy.vy);  // gain_x > gain_y → vx > vy
}

// invert_y：vy 反向。
void TestAirMouseStepInvertY() {
    AirMouseKinState s;
    AirMouseParams p;
    p.invert_y = true;
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{0, 10, false}, 0.016, false, p);
    assert(s.vy < 0.0);  // 反向
}

// dt 抖动下 v 平滑有限。
void TestAirMouseStepDtJitterRobust() {
    AirMouseKinState s;
    AirMouseParams p;
    const double dts[] = {0.016, 0.024, 0.008, 0.020, 0.012};
    for (int i = 0; i < 50; ++i) {
        AirMouseStep(s, AirMouseInput{80, 0, false}, dts[i % 5], false, p);
        assert(std::isfinite(s.vx));
    }
}

// 亚像素累积：小 gain 下 v 小，单帧 v×dt<1，累积多帧才输出 1px。
void TestAirMouseStepSubPixelAccumulation() {
    AirMouseKinState s;
    AirMouseParams p;
    p.gain_x = 1.0;  // 小 gain → 小 v
    p.neutral_deadzone = 0.0;  // 关闭方向锁，避免死区吃掉小输入
    int total_dx = 0;
    for (int i = 0; i < 200; ++i) {
        const auto r = AirMouseStep(s, AirMouseInput{3, 0, false}, 0.016, false, p);
        total_dx += r.dx;
    }
    assert(total_dx > 0);   // 亚像素累积后有小位移
    assert(total_dx < 200); // 非每帧 1px
}

// ===== 角度控制模型测试 =====
// AirMouseStep 本身不区分 omega/theta 语义，is_angle 仅作标记；这里验证传入固定 theta 时
// 光标速度持续非零，theta=0 时归零。

// 固定 theta，v 收敛到 theta×gain×factor(|theta|)，光标可持续移动。
void TestAirMouseStepAngleModeFollowsTheta() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kAngle;
    p.gain_x = 320.0;  // 角度模型典型增益
    const std::int16_t theta = 10;
    for (int i = 0; i < 200; ++i) {
        AirMouseStep(s, AirMouseInput{theta, 0, true}, 0.016, false, p);
    }
    const double v_target = theta * p.gain_x * AirMouseGainFactor(theta, p.curve);
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
    assert(s.vx > 0.0);
}

// theta=0 时 v_target=0，v 经 tau 衰减归零。
void TestAirMouseStepAngleModeStopsOnZeroTheta() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kAngle;
    p.gain_x = 320.0;
    // 先给非零 theta 让 v 起来
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.vx > 1.0);
    // 然后 theta=0
    for (int i = 0; i < 100; ++i) AirMouseStep(s, AirMouseInput{0, 0, true}, 0.016, false, p);
    assert(std::fabs(s.vx) < 1.0);
}

// ===== 方向锁测试 =====
// 中立区死区内不移动，方向锁释放。
void TestAirMouseStepDirectionLockNeutralStops() {
    AirMouseKinState s;
    AirMouseParams p;
    p.gain_x = 320.0;
    p.neutral_deadzone = 3.0;
    for (int i = 0; i < 50; ++i) {
        const auto r = AirMouseStep(s, AirMouseInput{2, 0, true}, 0.016, false, p);
        assert(r.dx == 0);
    }
    assert(s.lock_x == AirMouseDirectionLock::kNone);
}

// 越过死区后锁定方向并持续移动。
void TestAirMouseStepDirectionLockEngagesAfterCrossingDeadzone() {
    AirMouseKinState s;
    AirMouseParams p;
    p.gain_x = 320.0;
    p.neutral_deadzone = 3.0;
    for (int i = 0; i < 100; ++i) {
        AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    }
    assert(s.lock_x == AirMouseDirectionLock::kPositive);
    assert(s.vx > 0.0);
}

// 锁定正向后回到中立区死区内，光标应停下、方向锁释放。
void TestAirMouseStepDirectionLockStopsWhenReturningToNeutral() {
    AirMouseKinState s;
    AirMouseParams p;
    p.gain_x = 320.0;
    p.neutral_deadzone = 3.0;
    // 先锁定正向
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kPositive);
    // 回到死区内
    for (int i = 0; i < 100; ++i) AirMouseStep(s, AirMouseInput{1, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kNone);
    assert(std::fabs(s.vx) < 1.0);
}

// 从正向连续回到中立区再出发到反向：死区内光标停，只有重新越过死区才锁定反向。
void TestAirMouseStepDirectionLockRequiresReturnToNeutralBeforeReverse() {
    AirMouseKinState s;
    AirMouseParams p;
    p.gain_x = 320.0;
    p.neutral_deadzone = 3.0;
    // 锁定正向并建立速度
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kPositive);

    // 连续回中：10 → 5 → 2（死区内）
    AirMouseStep(s, AirMouseInput{5, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kPositive);  // 5 仍大于死区，保持锁定
    int dx_while_returning = 0;
    for (int i = 0; i < 20; ++i) {
        dx_while_returning += AirMouseStep(s, AirMouseInput{2, 0, true}, 0.016, false, p).dx;
    }
    assert(s.lock_x == AirMouseDirectionLock::kNone);      // 死区内释放
    assert(dx_while_returning >= 0);                       // 不回退

    // 经过死区到反向：-2（死区内） → -5
    for (int i = 0; i < 5; ++i) AirMouseStep(s, AirMouseInput{-2, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kNone);      // 仍在死区，不锁定
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{-5, 0, true}, 0.016, false, p);
    assert(s.lock_x == AirMouseDirectionLock::kNegative);  // 重新锁定反向
    assert(s.vx < 0.0);
}

// ===== 飞行摇杆/变化率控制测试 =====
// theta 控制光标速度变化率（加速度），回中后速度保持。

// 固定 theta，kRate 模式下速度持续增加。
void TestAirMouseStepRateModeAccelerates() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kRate;
    p.rate_gain = 100.0;
    p.rate_friction = 0.0;  // 关闭摩擦，便于观察纯加速
    for (int i = 0; i < 50; ++i) {
        AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    }
    assert(s.vx > 0.0);
    const double v_mid = s.vx;
    for (int i = 0; i < 50; ++i) {
        AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    }
    assert(s.vx > v_mid);  // 继续加速
}

// theta 回 0 后，kRate 模式下速度保持（摩擦为 0 时）。
void TestAirMouseStepRateModeCoastsAtZeroTheta() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kRate;
    p.rate_gain = 100.0;
    p.rate_friction = 0.0;
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.vx > 100.0);
    const double v_before = s.vx;
    // theta=0，摩擦=0，速度应保持
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{0, 0, true}, 0.016, false, p);
    assert(std::fabs(s.vx - v_before) < 1.0);
}

// theta=0 且摩擦 >0 时，速度逐渐衰减。
void TestAirMouseStepRateModeFrictionSlowsDown() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kRate;
    p.rate_gain = 100.0;
    p.rate_friction = 0.1;
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.vx > 100.0);
    const double v_before = s.vx;
    for (int i = 0; i < 100; ++i) AirMouseStep(s, AirMouseInput{0, 0, true}, 0.016, false, p);
    assert(s.vx < v_before);
    assert(s.vx > 0.0);  // 未完全停
}

// 反向 theta 使速度减速、停止并反向。
void TestAirMouseStepRateModeReversesByOpposingTheta() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kRate;
    p.rate_gain = 100.0;
    p.rate_friction = 0.0;
    // 先正向加速
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.vx > 100.0);
    // 反向 theta 减速
    int steps_to_reverse = 0;
    for (int i = 0; i < 200 && s.vx >= 0.0; ++i) {
        AirMouseStep(s, AirMouseInput{-10, 0, true}, 0.016, false, p);
        ++steps_to_reverse;
    }
    assert(steps_to_reverse < 200);  // 应在有限步内反向
    assert(s.vx < 0.0);
}

// 速度上限生效。
void TestAirMouseStepRateModeMaxSpeedCap() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kRate;
    p.rate_gain = 500.0;
    p.rate_friction = 0.0;
    p.rate_max_speed = 1000.0;
    for (int i = 0; i < 500; ++i) {
        AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    }
    assert(std::fabs(s.vx) <= p.rate_max_speed + 1.0);
}

// 切换回 kAngle 模式，原有角度控制行为不变。
void TestAirMouseStepAngleModeStillWorks() {
    AirMouseKinState s;
    AirMouseParams p;
    p.control_mode = AirMouseControlMode::kAngle;
    p.gain_x = 320.0;
    for (int i = 0; i < 200; ++i) {
        AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    }
    const double v_target = 10.0 * p.gain_x * AirMouseGainFactor(10.0, p.curve);
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
}

// 体感鼠标配置项 Save/Load 往返 + Clamp 边界。
void TestAppConfigAirMouseRoundTrip() {
    AppConfig config;
    config.default_interaction_settings.air_mouse_sensitivity_x = 7;
    config.default_interaction_settings.air_mouse_sensitivity_y = 6;
    config.air_mouse_tau = 0.15;
    config.air_mouse_invert_y = true;
    config.air_mouse_curve_low_thresh = 12.0;
    config.air_mouse_curve_high_thresh = 45.0;
    config.air_mouse_curve_low_factor = 0.2;
    config.air_mouse_curve_high_factor = 3.5;
    config.air_mouse_neutral_deadzone = 6.0;
    config.air_mouse_control_mode = "angle";
    config.air_mouse_rate_gain = 120.0;
    config.air_mouse_rate_friction = 0.08;
    config.air_mouse_rate_max_speed = 2500.0;
    const auto path = std::filesystem::temp_directory_path() / "voicestick_air_mouse_test.toml";
    config.Save(path);
    const auto loaded = AppConfig::Load(path);
    std::filesystem::remove(path);
    assert(loaded.default_interaction_settings.air_mouse_sensitivity_x == 7);
    assert(loaded.default_interaction_settings.air_mouse_sensitivity_y == 6);
    assert(std::fabs(loaded.air_mouse_tau - 0.15) < 1e-9);
    assert(loaded.air_mouse_invert_y == true);
    assert(std::fabs(loaded.air_mouse_curve_low_thresh - 12.0) < 1e-9);
    assert(std::fabs(loaded.air_mouse_curve_high_thresh - 45.0) < 1e-9);
    assert(std::fabs(loaded.air_mouse_curve_low_factor - 0.2) < 1e-9);
    assert(std::fabs(loaded.air_mouse_curve_high_factor - 3.5) < 1e-9);
    assert(std::fabs(loaded.air_mouse_neutral_deadzone - 6.0) < 1e-9);
    assert(loaded.air_mouse_control_mode == "angle");
    assert(std::fabs(loaded.air_mouse_rate_gain - 120.0) < 1e-9);
    assert(std::fabs(loaded.air_mouse_rate_friction - 0.08) < 1e-9);
    assert(std::fabs(loaded.air_mouse_rate_max_speed - 2500.0) < 1e-9);

    // Clamp 边界：越界回落默认值。
    assert(AirMouseSensitivityClamp(0) == 5);
    assert(AirMouseSensitivityClamp(11) == 5);
    assert(AirMouseTauClamp(0.005) == 0.05);
    assert(AirMouseTauClamp(1.0) == 0.05);
    assert(std::fabs(AirMouseNeutralDeadzoneClamp(0.5) - 3.0) < 1e-9);
    assert(std::fabs(AirMouseNeutralDeadzoneClamp(12.0) - 3.0) < 1e-9);
    assert(AirMouseControlModeFromName("angle") == AirMouseControlMode::kAngle);
    assert(AirMouseControlModeFromName("rate") == AirMouseControlMode::kRate);
    assert(AirMouseControlModeFromName("invalid") == AirMouseControlMode::kRate);
    assert(AirMouseControlModeName(AirMouseControlMode::kAngle) == "angle");
    assert(AirMouseControlModeName(AirMouseControlMode::kRate) == "rate");
    assert(std::fabs(AirMouseRateGainClamp(5.0) - 80.0) < 1e-9);
    assert(std::fabs(AirMouseRateGainClamp(600.0) - 80.0) < 1e-9);
    assert(std::fabs(AirMouseRateFrictionClamp(-0.1) - 0.05) < 1e-9);
    assert(std::fabs(AirMouseRateFrictionClamp(0.8) - 0.05) < 1e-9);
    assert(std::fabs(AirMouseRateMaxSpeedClamp(200.0) - 4000.0) < 1e-9);
    assert(std::fabs(AirMouseRateMaxSpeedClamp(9000.0) - 4000.0) < 1e-9);
}

void TestConfigTemplateSeeding() {
    const auto base = std::filesystem::temp_directory_path() / "voicestick_template_seed_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    auto write_file = [](const std::filesystem::path& path, const std::string& content) {
        std::ofstream out(path, std::ios::binary);
        out << content;
    };
    auto read_file = [](const std::filesystem::path& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // 场景1：目标不存在 + 模板存在 → 复制，返回 true，内容一致。
    {
        const auto tmpl = base / "template1.toml";
        const auto target = base / "dir1" / "config.toml";
        write_file(tmpl, "asr_provider = \"voicestick_cloud\"\n");
        assert(!std::filesystem::exists(target));
        const bool copied = AppConfig::SeedConfigFromTemplate(tmpl, target);
        assert(copied == true);
        assert(std::filesystem::exists(target));
        assert(read_file(target) == "asr_provider = \"voicestick_cloud\"\n");
    }

    // 场景2：目标已存在 → 不覆盖，返回 false，原内容保留。
    {
        const auto tmpl = base / "template2.toml";
        const auto target = base / "dir2" / "config.toml";
        std::filesystem::create_directories(target.parent_path());
        write_file(target, "existing\n");
        write_file(tmpl, "new\n");
        const bool copied = AppConfig::SeedConfigFromTemplate(tmpl, target);
        assert(copied == false);
        assert(read_file(target) == "existing\n");
    }

    // 场景3：模板不存在 → 跳过，返回 false，不创建目标。
    {
        const auto tmpl = base / "missing_template.toml";
        const auto target = base / "dir3" / "config.toml";
        const bool copied = AppConfig::SeedConfigFromTemplate(tmpl, target);
        assert(copied == false);
        assert(!std::filesystem::exists(target));
    }

    // 场景4：目标父目录多层不存在 → 自动创建后复制，返回 true。
    {
        const auto tmpl = base / "template4.toml";
        const auto target = base / "deep" / "nested" / "config.toml";
        write_file(tmpl, "llm_model = \"gpt\"\n");
        const bool copied = AppConfig::SeedConfigFromTemplate(tmpl, target);
        assert(copied == true);
        assert(std::filesystem::exists(target));
        assert(read_file(target) == "llm_model = \"gpt\"\n");
    }

    std::filesystem::remove_all(base);
}

// 内置 key 时向导应跳过 kAsr 步：NeedsAsrStep 依据 ActiveApiKey 判断。
void TestNeedsAsrStep() {
    // 有 ActiveApiKey（火山）-> 不需要 kAsr 步（内置 key 跳过）
    {
        AppConfig config;
        config.asr_provider = AsrProvider::kVolcengine;
        config.volcengine_api_key = "test_key";
        assert(!NeedsAsrStep(config));
    }
    // 无 ActiveApiKey -> 需要 kAsr 步（让用户填）
    // volcengine 模式 config key 空时 ActiveApiKey 回退编译期内置 key：
    // 公开构建（内置空）-> 需要 kAsr；开发/MSI 构建（内置非空）-> 不需要。
    {
        AppConfig config;
        config.asr_provider = AsrProvider::kVolcengine;
        config.volcengine_api_key = "";
        assert(NeedsAsrStep(config) == BuiltinApiKey().empty());
    }
    // 腾讯用 tencent_secret_id 作 ActiveApiKey
    {
        AppConfig config;
        config.asr_provider = AsrProvider::kTencent;
        config.tencent_secret_id = "secret_id_value";
        assert(!NeedsAsrStep(config));
    }
}

// ActiveApiKey 在 volcengine 模式下配置 key 为空时回退编译期内置 key；
// 配置 key 非空时优先用配置 key。tencent/cloud 不回退（内置 key 是 volcengine 的）。
void TestActiveApiKeyBuiltinFallback() {
    // volcengine + 空 key + 内置非空 -> 回退内置 key
    assert(ResolveActiveApiKey(AsrProvider::kVolcengine, "", "", "", "BUILTIN_KEY", "BUILTIN_TENCENT") == "BUILTIN_KEY");
    // volcengine + 配置 key 非空 -> 配置 key 优先（不回退）
    assert(ResolveActiveApiKey(AsrProvider::kVolcengine, "", "USER_KEY", "", "BUILTIN_KEY", "BUILTIN_TENCENT") == "USER_KEY");
    // volcengine + 空 key + 空内置 -> 空（公开构建无内置 key）
    assert(ResolveActiveApiKey(AsrProvider::kVolcengine, "", "", "", "", "") == "");
    // tencent -> 返回 tencent_secret_id（不回退内置 key）
    assert(ResolveActiveApiKey(AsrProvider::kTencent, "", "", "", "BUILTIN_KEY", "BUILTIN_TENCENT") == "BUILTIN_TENCENT");
    assert(ResolveActiveApiKey(AsrProvider::kTencent, "", "", "USER_TENCENT", "BUILTIN_KEY", "BUILTIN_TENCENT") == "USER_TENCENT");
    // cloud -> 返回 voicestick_api_key（不回退内置 key）
    assert(ResolveActiveApiKey(AsrProvider::kVoiceStickCloud, "CLOUD_KEY", "", "", "BUILTIN_KEY", "BUILTIN_TENCENT") == "CLOUD_KEY");
}

// 通用回退纯函数：配置值优先，空则回退内置值。供腾讯云 SecretKey/AppId 与 LLM
// base_url/api_key/model 字段复用（这些字段不参与 ActiveApiKey 的 provider 分发）。
void TestResolveActiveString() {
    assert(ResolveActiveString("USER_VAL", "BUILTIN_VAL") == "USER_VAL");
    assert(ResolveActiveString("", "BUILTIN_VAL") == "BUILTIN_VAL");
    assert(ResolveActiveString("", "") == "");
    assert(ResolveActiveString("USER_VAL", "") == "USER_VAL");
}

// ActiveResourceId 在 resource_id 为空时回退 SupportedResourceIds().front()
// （volc.seedasr.sauc.duration），非空时优先用配置值。修复首启 config.template.toml
// resource_id="" 覆盖成员默认值，致 volcengine ASR 缺 X-Api-Resource-Id 失败、
// 需进设置切换一次供应商才可用的问题。
void TestActiveResourceId() {
    AppConfig config;
    config.resource_id = "";
    assert(config.ActiveResourceId() == "volc.seedasr.sauc.duration");
    config.resource_id = "volc.bigasr.sauc.duration";
    assert(config.ActiveResourceId() == "volc.bigasr.sauc.duration");
}

// 运行时 Save 不得用内存过期凭据覆盖磁盘真实凭据（路径 A 根因修复）。
void TestSavePreservingDiskCredentials() {
    const auto base = std::filesystem::temp_directory_path() / "voicestick_preserve_cred_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const auto path = base / "config.toml";

    auto read_file = [](const std::filesystem::path& p) -> std::string {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // 磁盘先写入含真实凭据的 config（模拟用户手动替换的 config.toml）
    {
        AppConfig disk;
        disk.asr_provider = AsrProvider::kVolcengine;
        disk.volcengine_api_key = "REAL_VOLCENGINE_KEY";
        disk.tencent_secret_id = "REAL_TENCENT_ID";
        disk.tencent_secret_key = "REAL_TENCENT_KEY";
        disk.tencent_appid = "REAL_APPID";
        disk.llm_base_url = "https://api.deepseek.com/v1";
        disk.llm_api_key = "REAL_DEEPSEEK_KEY";
        disk.llm_model = "deepseek-chat";
        disk.auto_enter = false;
        disk.Save(path);
    }
    assert(read_file(path).find("REAL_VOLCENGINE_KEY") != std::string::npos);

    // 内存 config：凭据过期（空，模拟启动时无 key 的旧快照），但改了非凭据字段 auto_enter
    {
        AppConfig mem = AppConfig::Load(path);
        mem.volcengine_api_key = "";   // 模拟内存过期
        mem.llm_api_key = "";
        mem.auto_enter = true;         // UI 改动（非凭据字段）
        mem.SavePreservingDiskCredentials(path);
    }

    // 重读验证：凭据字段保留磁盘值，非凭据字段用内存值
    AppConfig after = AppConfig::Load(path);
    assert(after.volcengine_api_key == "REAL_VOLCENGINE_KEY");
    assert(after.llm_api_key == "REAL_DEEPSEEK_KEY");
    assert(after.tencent_secret_id == "REAL_TENCENT_ID");
    assert(after.tencent_secret_key == "REAL_TENCENT_KEY");
    assert(after.tencent_appid == "REAL_APPID");
    assert(after.llm_base_url == "https://api.deepseek.com/v1");
    assert(after.llm_model == "deepseek-chat");
    assert(after.auto_enter == true);

    std::filesystem::remove_all(base);
}

// 设置/Onboarding 对话框专用保存：用户刚输入的非空凭据优先，空字段用磁盘值兜底。
// 修复路径 B：切 provider 时普通 Save() 会用内存空/旧凭据覆盖磁盘 key（如腾讯密钥丢失）。
void TestSaveSettingsDialog() {
    const auto base = std::filesystem::temp_directory_path() / "voicestick_settings_dialog_save_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const auto path = base / "config.toml";

    auto read_file = [](const std::filesystem::path& p) -> std::string {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // 磁盘先写入含真实凭据的 config（模拟用户手动替换的 config.toml）
    {
        AppConfig disk;
        disk.asr_provider = AsrProvider::kTencent;
        disk.volcengine_api_key = "REAL_VOLCENGINE_KEY";
        disk.tencent_secret_id = "REAL_TENCENT_ID";
        disk.tencent_secret_key = "REAL_TENCENT_SECRET_KEY";
        disk.tencent_appid = "REAL_APPID";
        disk.tencent_engine_model_type = "16k_zh";
        disk.llm_base_url = "https://api.deepseek.com/v1";
        disk.llm_api_key = "REAL_DEEPSEEK_KEY";
        disk.llm_model = "deepseek-chat";
        disk.auto_enter = false;
        disk.Save(path);
    }

    // 内存 config：模拟用户在设置对话框切到 volcengine 并输入新 key，内存中其余凭据
    // 为启动时的过期快照（空）。SaveSettingsDialog 应写入新 key、保留磁盘其他凭据。
    {
        AppConfig mem = AppConfig::Load(path);
        mem.asr_provider = AsrProvider::kVolcengine;  // 用户切换 provider
        mem.volcengine_api_key = "NEW_VOLCENGINE_KEY";  // 用户新输入
        mem.llm_base_url = "https://api.openai.com/v1";  // 用户新输入
        mem.tencent_secret_id = "";  // 内存过期（空），应保留磁盘值
        mem.tencent_secret_key = "";  // 同上
        mem.tencent_appid = "";
        mem.llm_api_key = "";
        mem.llm_model = "";
        mem.auto_enter = true;  // 非凭据字段用内存值
        mem.SaveSettingsDialog(path);
    }

    // 重读验证：用户新输入 + 非凭据字段用内存值；内存空字段保留磁盘值
    AppConfig after = AppConfig::Load(path);
    assert(after.asr_provider == AsrProvider::kVolcengine);
    assert(after.volcengine_api_key == "NEW_VOLCENGINE_KEY");
    assert(after.llm_base_url == "https://api.openai.com/v1");
    assert(after.auto_enter == true);
    assert(after.tencent_secret_id == "REAL_TENCENT_ID");
    assert(after.tencent_secret_key == "REAL_TENCENT_SECRET_KEY");
    assert(after.tencent_appid == "REAL_APPID");
    assert(after.tencent_engine_model_type == "16k_zh");
    assert(after.llm_api_key == "REAL_DEEPSEEK_KEY");
    assert(after.llm_model == "deepseek-chat");

    // 无磁盘 config 时退化为普通 Save（不崩溃）
    const auto fresh_path = base / "fresh" / "config.toml";
    AppConfig fresh;
    fresh.volcengine_api_key = "FRESH_KEY";
    fresh.SaveSettingsDialog(fresh_path);
    AppConfig fresh_after = AppConfig::Load(fresh_path);
    assert(fresh_after.volcengine_api_key == "FRESH_KEY");

    std::filesystem::remove_all(base);
}

// 角色分离：SetDefaultCapture 的 roles 恒为 {kConsole}，不碰 eCommunications。
void TestCoordinatorWechatInputMethodAutoSwitchesDefaultDevice() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.auto_switch_default_recording_device = true;
    config.wechat_input_method.virtual_mic_capture_name = "CABLE Output";
    auto state_path = std::filesystem::temp_directory_path() / "voicestick_auto_switch_state.json";
    std::filesystem::remove(state_path);

    auto fake_switcher = std::make_unique<FakeDefaultAudioDeviceController>();
    fake_switcher->default_capture = AudioDeviceInfo{L"{real-mic}", L"Realtek Mic"};
    fake_switcher->capture_devices = {
        AudioDeviceInfo{L"{real-mic}", L"Realtek Mic"},
        AudioDeviceInfo{L"{cable-out}", L"CABLE Output (VB-Audio Virtual Cable)"},
    };
    FakeDefaultAudioDeviceController* switcher_ptr = fake_switcher.get();

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        },
        [&]() { return std::move(fake_switcher); },
        state_path);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));

    // Start：切到 CABLE Output，角色恒 eConsole。
    assert(std::filesystem::exists(state_path));
    assert(switcher_ptr->set_call_count >= 1);
    assert(switcher_ptr->set_calls.back().device_id == L"{cable-out}");
    assert(switcher_ptr->set_calls.back().roles ==
           std::vector<DeviceRole>{DeviceRole::kConsole});

    // 松开：切回原设备，角色仍 eConsole。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    assert(switcher_ptr->set_call_count >= 2);
    assert(switcher_ptr->set_calls.back().device_id == L"{real-mic}");
    assert(switcher_ptr->set_calls.back().roles ==
           std::vector<DeviceRole>{DeviceRole::kConsole});
    // Stop 后状态文件清除。
    assert(!std::filesystem::exists(state_path));
    std::filesystem::remove(state_path);
}

// 残留自愈：上次崩溃未切回（状态文件 switched=true），Start 检测并 Restore + 清文件。
void TestCoordinatorAutoSwitchRecoversStaleState() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.auto_switch_default_recording_device = true;

    auto state_path = std::filesystem::temp_directory_path() / "voicestick_auto_switch_recover.json";
    std::filesystem::remove(state_path);
    DeviceSwitchState stale{true, "{real-mic}", "Realtek Mic"};
    assert(SaveDeviceSwitchState(state_path, stale));

    auto fake_switcher = std::make_unique<FakeDefaultAudioDeviceController>();
    fake_switcher->capture_devices = {
        AudioDeviceInfo{L"{real-mic}", L"Realtek Mic"},
        AudioDeviceInfo{L"{cable-out}", L"CABLE Output"},
    };
    FakeDefaultAudioDeviceController* switcher_ptr = fake_switcher.get();

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        },
        [&]() { return std::move(fake_switcher); },
        state_path);
    coordinator.Start();

    // Start 检测残留 -> Restore {real-mic} (eConsole) + 清状态文件。
    assert(switcher_ptr->set_call_count >= 1);
    assert(switcher_ptr->set_calls.back().device_id == L"{real-mic}");
    assert(switcher_ptr->set_calls.back().roles ==
           std::vector<DeviceRole>{DeviceRole::kConsole});
    assert(!std::filesystem::exists(state_path));

    std::filesystem::remove(state_path);
}

// auto_switch=false 时不触碰默认录音设备。
void TestCoordinatorWechatInputMethodNoSwitchWhenDisabled() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    // auto_switch 默认 false。

    auto fake_switcher = std::make_unique<FakeDefaultAudioDeviceController>();
    FakeDefaultAudioDeviceController* switcher_ptr = fake_switcher.get();

    VoiceStickCoordinator coordinator(
        config, std::move(ble), std::move(asr), &ui, &input, {},
        [](const IVirtualMicRenderer::Options&) {
            return std::make_unique<FakeVirtualMicRenderer>(true);
        },
        [](const std::string&) {
            return std::make_unique<FakeWechatInputMethodHotkey>();
        },
        [&]() { return std::move(fake_switcher); });
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    assert(switcher_ptr->set_call_count == 0);
    assert(switcher_ptr->get_call_count == 0);
}

// 前台为高权限程序时，按下设备键应气泡提醒提权、不启动会话（不发快捷键）、设备置 ready。
void TestWechatWarnsWhenForegroundElevated() {
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
    coordinator.SetForegroundProbe(std::make_unique<FakeForegroundProcessProbe>(true, L"Weixin.exe"));
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));

    assert(!ui.notifications.empty());
    assert(ui.notifications.back().find("Weixin") != std::string::npos);
    // MaybeWalk 跳过 StartWechatInputMethodSession，wechat_hotkey_factory 未被调用，fake_hotkey 仍为 nullptr。
    assert(fake_hotkey == nullptr);
    assert(!ble_ptr->sent_ui_states.empty());
    assert(ble_ptr->sent_ui_states.back().state == "ready");
}

// 同一高权限程序本次运行只提醒一次（按进程名去重）。
void TestWechatNoDuplicateElevationWarnForSameProcess() {
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
    coordinator.SetForegroundProbe(std::make_unique<FakeForegroundProcessProbe>(true, L"Weixin.exe"));
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    const auto count_after_first = ui.notifications.size();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 8));

    assert(ui.notifications.size() == count_after_first);
}

// 换一个高权限程序再次提醒。
void TestWechatWarnsAgainForDifferentElevatedProcess() {
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
    coordinator.SetForegroundProbe(std::make_unique<FakeForegroundProcessProbe>(
        std::vector<std::wstring>{L"Weixin.exe", L"WorkGrid.exe"}));
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));
    assert(ui.notifications.size() == 1);
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 8));

    assert(ui.notifications.size() == 2);
}

// 前台为同级 Medium 程序时不提醒、正常启动会话。
void TestWechatNoWarnWhenForegroundNormal() {
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
    coordinator.SetForegroundProbe(std::make_unique<FakeForegroundProcessProbe>(false));
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 新时序：首帧 Opus 解码成功才 SendDown，注入有效首帧触发弹框。
    AudioFrame first;
    first.session_id = 7;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    assert(ui.notifications.empty());
    assert(fake_hotkey->send_down_count == 1);
}

// 未注入 probe（nullptr）时不检测、正常启动会话。
void TestWechatNoProbeNoWarn() {
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
    // 不注入 probe（nullptr）：保持旧行为，不检测 UIPI。
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 7));
    // 新时序：首帧 Opus 解码成功才 SendDown，注入有效首帧触发弹框。
    AudioFrame first;
    first.session_id = 7;
    first.seq = 1;
    first.payload = EncodeOpusPacket(MakeSinePcm(440));
    ble_ptr->on_audio_frame("5A74", first);
    ble_ptr->on_state_event("5A74", ButtonEvent("button_up", "primary", 7));

    assert(ui.notifications.empty());
    assert(fake_hotkey->send_down_count == 1);
}

// 状态文件往返：Save -> Load 一致，含中文 UTF-8 friendly name。
void TestDeviceSwitchStateRoundTrip() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_device_switch_state_rt.json";
    std::filesystem::remove(temp);
    DeviceSwitchState state{true, "{0.0.1.00000000}.{abc}", "麦克风(Realtek)"};
    assert(SaveDeviceSwitchState(temp, state));
    DeviceSwitchState loaded{};
    assert(LoadDeviceSwitchState(temp, loaded));
    assert(loaded.switched == true);
    assert(loaded.saved_default_capture_id == "{0.0.1.00000000}.{abc}");
    assert(loaded.saved_default_capture_name == "麦克风(Realtek)");
    std::filesystem::remove(temp);
}

// Clear 删除文件，文件不存在亦成功。
void TestDeviceSwitchStateClear() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_device_switch_state_clear.json";
    std::filesystem::remove(temp);
    DeviceSwitchState state{true, "id", "name"};
    assert(SaveDeviceSwitchState(temp, state));
    assert(std::filesystem::exists(temp));
    assert(ClearDeviceSwitchState(temp));
    assert(!std::filesystem::exists(temp));
    assert(ClearDeviceSwitchState(temp));  // 再次 Clear（文件不存在）成功。
}

// Load 文件不存在：返回未切换空状态（switched=false），非错误。
void TestDeviceSwitchStateLoadMissingFile() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_device_switch_state_missing.json";
    std::filesystem::remove(temp);
    DeviceSwitchState loaded{true, "stale", "stale"};
    assert(LoadDeviceSwitchState(temp, loaded));
    assert(loaded.switched == false);
    assert(loaded.saved_default_capture_id.empty());
    assert(loaded.saved_default_capture_name.empty());
}

// wstring ↔ UTF-8 转换往返（含中文与 ASCII）。
void TestWStringUtf8Conversion() {
    assert(WStringToUtf8(L"") == "");
    assert(Utf8ToWString("") == L"");
    assert(WStringToUtf8(L"ASCII") == "ASCII");
    assert(Utf8ToWString("ASCII") == L"ASCII");
    const std::wstring zh = L"麦克风(Realtek)";
    const std::string u8 = WStringToUtf8(zh);
    assert(Utf8ToWString(u8) == zh);
}

// ===== VoiceStickFlash：COM 口烧录工具（Doc/Plan/windows-com-flash-tool.md §7.1）=====

void TestComPortScoring() {
    ComPortInfo esp32s3{L"COM5", L"USB JTAG/serial debug unit (COM5)",
                        L"Espressif Systems", L"USB\\VID_303A&PID_1001\\ABC123"};
    ComPortInfo ch340{L"COM3", L"USB-SERIAL CH340 (COM3)", L"wch.cn",
                      L"USB\\VID_1A86&PID_7523\\XYZ"};
    ComPortInfo unrelated{L"COM7", L"Some Random Modem", L"ACME Corp",
                          L"USB\\VID_9999&PID_9999\\1"};

    // 单项评分：desc 关键字累加、mfr、hwid、preferred。
    const int esp32_score = ScoreComPort(esp32s3);
    assert(esp32_score == 30 + 20 + 40);  // jtag + espressif + 303a:
    const int ch340_score = ScoreComPort(ch340);
    assert(ch340_score == 30 + 30 + 20 + 40);  // usb-serial + ch340 + wch + 1a86:
    assert(ScoreComPort(unrelated) == 0);

    // preferred_vid_pid +160：303A 原生 USB 反超 CH340。
    const auto& preferred = DefaultPreferredVidPid();
    assert(ScoreComPort(esp32s3, preferred) == esp32_score + 160);

    std::vector<ComPortInfo> ports{ch340, esp32s3, unrelated};
    // 无 preferred 时 CH340（120）高于 ESP32-S3（90）。
    assert(SelectBestComPort(ports)->device == L"COM3");
    // 有 preferred 时 ESP32-S3（250）胜出。
    assert(SelectBestComPort(ports, preferred)->device == L"COM5");
    // 全部 0 分返回 nullptr（调用方兜底）。
    std::vector<ComPortInfo> only_unrelated{unrelated};
    assert(SelectBestComPort(only_unrelated, preferred) == nullptr);

    // 同分取 COM 编号最小。
    ComPortInfo esp32s3_high = esp32s3;
    esp32s3_high.device = L"COM12";
    std::vector<ComPortInfo> tie{esp32s3_high, esp32s3};
    assert(SelectBestComPort(tie, preferred)->device == L"COM5");

    // ComPortNumber：非 COM 名/无编号排最后。
    assert(ComPortNumber(L"COM5") == 5);
    assert(ComPortNumber(L"COM12") == 12);
    assert(ComPortNumber(L"/dev/ttyUSB0") == 0x7fffffff);
}

namespace {


} // namespace

void TestEsptoolCommandBuilder() {
    FlashOptions options;
    options.serial_port = L"COM5";
    options.firmware_path = L"D:\\固件 目录\\voice stick merged.bin";  // 中文 + 空格
    options.baud = 460800;

    const std::filesystem::path python_exe(L"C:\\payload\\python\\python.exe");

    // 整包：单条命令，write_flash @ 0x0，复位策略与全局选项齐全。
    options.mode = FlashMode::kFullMerged;
    auto full = BuildEsptoolCommandSequence(options, python_exe);
    assert(full.size() == 1);
    const auto& argv = full[0];
    assert(argv[0] == python_exe.wstring());
    assert(argv[1] == L"-m" && argv[2] == L"esptool");
    assert(ArgvContains(argv, L"--chip") && ArgvContains(argv, L"esp32s3"));
    assert(ArgvContains(argv, L"--port") && ArgvContains(argv, L"COM5"));
    assert(ArgvContains(argv, L"--baud") && ArgvContains(argv, L"460800"));
    assert(ArgvContains(argv, L"--before") && ArgvContains(argv, L"default_reset"));
    // 关键：本板 hard_reset 无效，必须 no_reset（烧完手动重启）。
    assert(ArgvContains(argv, L"--after") && ArgvContains(argv, L"no_reset"));
    assert(ArgvContains(argv, L"write_flash") && ArgvContains(argv, L"0x0"));
    // 中文路径参数不被截断（宽字符原样传递）。
    assert(ArgvContains(argv, options.firmware_path));
    assert(!ArgvContains(argv, L"erase_flash"));

    // 仅应用分区：write_flash @ 0x10000。
    options.mode = FlashMode::kAppOnly;
    auto app = BuildEsptoolCommandSequence(options, python_exe);
    assert(app.size() == 1);
    assert(ArgvContains(app[0], L"write_flash") && ArgvContains(app[0], L"0x10000"));
    assert(!ArgvContains(app[0], L"0x0"));

    // 先擦除再整包：两条命令，erase_flash 在前，整包写在后。
    options.mode = FlashMode::kEraseThenFull;
    auto erase_full = BuildEsptoolCommandSequence(options, python_exe);
    assert(erase_full.size() == 2);
    assert(ArgvContains(erase_full[0], L"erase_flash"));
    assert(!ArgvContains(erase_full[0], L"write_flash"));
    assert(ArgvContains(erase_full[1], L"write_flash") && ArgvContains(erase_full[1], L"0x0"));

    // JoinCommandLine：含空格/中文参数加引号，无空格参数不引。
    const std::wstring cmd = JoinCommandLine(erase_full[1]);
    assert(cmd.find(L"\"D:\\固件 目录\\voice stick merged.bin\"") != std::wstring::npos);
    assert(cmd.find(L"\"--chip\"") == std::wstring::npos);
}

void TestEsptoolProgressParser() {
    std::vector<FlashEvent> events;
    EsptoolProgressParser parser([&](const FlashEvent& e) { events.push_back(e); });

    parser.FeedLine("esptool.py v5.2.0");
    assert(events.size() == 1 && events[0].kind == FlashEvent::kLogLine);

    // 阶段：连接中 → 检测芯片（Chip ID 行不重复发同名阶段）。
    parser.FeedLine("Stub running...");
    parser.FeedLine("Detected chip type: ESP32-S3");
    parser.FeedLine("Chip ID: 9");
    std::size_t stage_count = 0;
    for (const auto& e : events) if (e.kind == FlashEvent::kStage) ++stage_count;
    assert(stage_count == 2);
    assert(events[1].kind == FlashEvent::kStage && events[1].text == L"连接中");
    assert(events[2].kind == FlashEvent::kStage && events[2].text == L"检测芯片");

    // 写入进度：兼容 esptool 4.x "(X %)" 形式。
    events.clear();
    parser.FeedLine("Writing at 0x00000000... (10 %)");
    parser.FeedLine("Writing at 0x00040000... (50 %)");
    assert(events.size() == 3);
    assert(events[0].kind == FlashEvent::kStage && events[0].text == L"写入");
    assert(events[1].kind == FlashEvent::kProgress && events[1].percent == 10);
    assert(events[2].kind == FlashEvent::kProgress && events[2].percent == 50);

    // esptool 5.x 进度条格式（管道非 TTY 时实际产出，烧录工具内嵌 esptool 5.2.0）。
    {
        std::vector<FlashEvent> ev52;
        EsptoolProgressParser p52([&](const FlashEvent& e) { ev52.push_back(e); });
        p52.FeedLine("Stub flasher running.");
        p52.FeedLine("Detecting chip type... ESP32-S3");
        assert(ev52.size() == 2);
        assert(ev52[0].kind == FlashEvent::kStage && ev52[0].text == L"连接中");
        assert(ev52[1].kind == FlashEvent::kStage && ev52[1].text == L"检测芯片");

        ev52.clear();
        p52.FeedLine("Writing at 0x00010000 [=====>                    ]  45.7% 1077248/2359296 bytes... ");
        assert(ev52.size() == 2);
        assert(ev52[0].kind == FlashEvent::kStage && ev52[0].text == L"写入");
        assert(ev52[1].kind == FlashEvent::kProgress && ev52[1].percent == 46);
    }

    // 黑名单行不产进度事件（擦除/校验/Hash/Connecting）。
    events.clear();
    parser.FeedLine("Erasing flash (this may take a while)...");
    parser.FeedLine("Hash of data verified.");
    parser.FeedLine("Connecting...");
    for (const auto& e : events) assert(e.kind != FlashEvent::kProgress);
    assert(events[0].kind == FlashEvent::kStage && events[0].text == L"擦除");
    assert(events[1].kind == FlashEvent::kStage && events[1].text == L"校验");

    // 错误：发 kError 且该行不重复发 LogLine。
    events.clear();
    parser.FeedLine("A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.");
    assert(events.size() == 1 && events[0].kind == FlashEvent::kError);
    assert(events[0].text.find(L"Failed to connect") != std::wstring::npos);
}

namespace {

// 可编程假运行器：按 exit_codes 队列返回退出码，记录全部调用。
class FakeFlashRunner : public IFlashProcessRunner {
public:
    std::vector<std::vector<std::wstring>> calls;
    std::vector<int> exit_codes;

    int Run(const std::vector<std::wstring>& argv,
            const std::function<void(const std::string& line)>& on_line) override {
        calls.push_back(argv);
        const int code = calls.size() <= exit_codes.size()
                             ? exit_codes[calls.size() - 1]
                             : 0;
        if (on_line) on_line("Writing at 0x00000000... (100 %)");
        return code;
    }
    void Cancel() override { cancel_called = true; }

    bool cancel_called = false;
};

struct FlashTestPaths {
    std::filesystem::path dir;
    std::filesystem::path firmware;
    std::filesystem::path python;

    FlashTestPaths() {
        dir = std::filesystem::temp_directory_path() /
              L"voicestick_flash_core_tests";
        std::filesystem::create_directories(dir);
        firmware = dir / L"测试固件.bin";
        python = dir / L"python.exe";
        { std::ofstream(firmware, std::ios::binary) << "fake-firmware"; }
        { std::ofstream(python, std::ios::binary) << "fake-python"; }
    }
    ~FlashTestPaths() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};


} // namespace


// Suite entry: core_tests.cc main() calls this once.
void RunCodecFixtureSerialBatchTests() {
    TestImaAdpcmDecoderGoldenFixtures();
    TestFrameAccumulator();
    TestPcmPostprocessor();
    TestAudioOpusEncoderRoundTrip();
    TestCoordinatorXiaomiHoldToTalkStreamsOggToAsr();
    TestCoordinatorXiaomiCancelSemantics();
    TestCoordinatorXiaomiWechatInputMethodDecodesToVirtualMic();
    TestCoordinatorXiaomiSubtitlePath();
    TestDeviceIdRcPrefix();
    TestAppConfigXiaomiTable();
    TestAppConfigXiaomiKeyMap();
    TestXiaomiUsageTapParsing();
    TestXiaomiUsageTapButtonTable();
    TestUsageTapManagerStopBounded();
    TestCoordinatorXiaomiCapabilityGating();
    TestPcmRingBufferWriteRead();
    TestPcmRingBufferOverwrite();
    TestPcmRingBufferUnderrunSilence();
    TestPcmRingBufferClear();
    TestPcmRingBufferWrapAround();
    TestWasapiRendererFailsOnMissingDevice();
    TestWasapiRendererRejectsEmptyDeviceName();
    TestRenderPumpSubmitsFullAvailableNoCap();
    TestRenderPumpFillsSilenceWhenRingEmpty();
    TestRenderPumpZeroWhenBufferFull();
    TestWasapiRendererStopsCleanlyWakingBlockedThread();
    TestWasapiRendererRestartsAfterStop();
    TestOggOpusDemuxerParsesOpusHead();
    TestOggOpusDemuxerMultiplePackets();
    TestOggOpusDemuxerRoundTripWithFinish();
    TestOggOpusDemuxerRejectsBadMagic();
    TestOggOpusDemuxerRejectsTruncatedStream();
    TestWechatPipelineSteadyStateLatency();
    TestWechatPipelineBufferDurationPareto();
    TestWechatPipelineSmallBufferDeviceUnderrun();
    TestRingBurstBacklogAmplifiesLatency();
    TestRingBacklogUpperBoundByCapacity();
    TestOutputTargetWechatInputMethod();
    TestWechatInputMethodConfigRoundTrip();
    TestWechatInputMethodPerModeHotkeyRoundTrip();
    TestWechatInputMethodLegacyHotkeyFallback();
    TestWechatInputMethodActiveHotkeyByMode();
    TestWechatTriggerModeMigratedFromLegacyInteractionMode();
    TestWechatTriggerModeRoundTrip();
    TestWechatSessionModelConfig();
    TestWechatInputMethodHotkeyParsing();
    TestWechatHotkeySendDownRepeatsWhileHeld();
    TestCoordinatorWechatInputMethodButtonDownSendsHotkey();
    TestCoordinatorWechatInputMethodWritesDebugAudio();
    TestDebugAudioRecorderInvalidDirectoryDoesNotCrash();
    TestAppConfigDebugAudioDirUtf8RoundTrip();
    TestWechatClickTriggerDoesNotLeakToFocusedApp();
    TestSavePairedDeviceInfoUnknownDeviceNoEntry();
    TestCoordinatorUpdateConfigDestroysOldAsrOffThread();
    TestAirMouseStepVelocityFollowsOmega();
    TestAirMouseStepStopsWhenStale();
    TestAirMouseStepGainCurveLowRange();
    TestAirMouseStepGainCurveHighRange();
    TestAirMouseStepGainCurveMidRange();
    TestAirMouseStepGainCurveShape();
    TestAirMouseStepGainCurveContinuousAtLowThreshold();
    TestAirMouseStepGainCurveNegative();
    TestAirMouseGainFactorAcceptsCurveParams();
    TestAirMouseGainFactorDefaultCurveMatchesLegacy();
    TestAirMouseStepUsesCurveParams();
    TestAirMouseCurveClamp();
    TestAirMouseCurveClampLowBelowHigh();
    TestAirMouseStepAxisGain();
    TestAirMouseStepInvertY();
    TestAirMouseStepDtJitterRobust();
    TestAirMouseStepSubPixelAccumulation();
    TestAirMouseStepAngleModeFollowsTheta();
    TestAirMouseStepAngleModeStopsOnZeroTheta();
    TestAirMouseStepDirectionLockNeutralStops();
    TestAirMouseStepDirectionLockEngagesAfterCrossingDeadzone();
    TestAirMouseStepDirectionLockStopsWhenReturningToNeutral();
    TestAirMouseStepDirectionLockRequiresReturnToNeutralBeforeReverse();
    TestAirMouseStepRateModeAccelerates();
    TestAirMouseStepRateModeCoastsAtZeroTheta();
    TestAirMouseStepRateModeFrictionSlowsDown();
    TestAirMouseStepRateModeReversesByOpposingTheta();
    TestAirMouseStepRateModeMaxSpeedCap();
    TestAirMouseStepAngleModeStillWorks();
    TestAppConfigAirMouseRoundTrip();
    TestConfigTemplateSeeding();
    TestNeedsAsrStep();
    TestActiveApiKeyBuiltinFallback();
    TestResolveActiveString();
    TestActiveResourceId();
    TestSavePreservingDiskCredentials();
    TestSaveSettingsDialog();
    TestCoordinatorWechatInputMethodAutoSwitchesDefaultDevice();
    TestCoordinatorAutoSwitchRecoversStaleState();
    TestCoordinatorWechatInputMethodNoSwitchWhenDisabled();
    TestWechatWarnsWhenForegroundElevated();
    TestWechatNoDuplicateElevationWarnForSameProcess();
    TestWechatWarnsAgainForDifferentElevatedProcess();
    TestWechatNoWarnWhenForegroundNormal();
    TestWechatNoProbeNoWarn();
    TestDeviceSwitchStateRoundTrip();
    TestDeviceSwitchStateClear();
    TestDeviceSwitchStateLoadMissingFile();
    TestWStringUtf8Conversion();
    TestComPortScoring();
    TestEsptoolCommandBuilder();
    TestEsptoolProgressParser();
}

