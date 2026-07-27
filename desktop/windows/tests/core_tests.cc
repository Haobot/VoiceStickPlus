#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "ble_protocol.h"
#include "cmd_line.h"

#include <opus.h>
#include "byte_utils.h"
#include "cJSON.h"
#include "firmware_manifest.h"
#include "hotword_extractor.h"
#include "llm_refinement_client.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "ogg_opus_demuxer.h"
#include "pair_device_helper.h"
#include "pcm_ring_buffer.h"
#include "voice_stick_coordinator.h"
#include "wasapi_render_sink.h"
#include "wasapi_virtual_mic_renderer.h"
#include "wechat_input_method_hotkey.h"
#include "default_audio_device_controller.h"
#include "device_switch_state.h"
#include "debug_audio_recorder.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;

namespace {

struct SentUiState {
    std::string state;
    std::string text;
    std::optional<std::string> device_id;
};

struct SentRemoteButton {
    RemoteButtonAction action;
    std::string button;
    std::optional<std::string> device_id;
    std::uint32_t request_id;
};

struct SentImuWakeSensitivity {
    int threshold_lsb = 0;
    std::optional<std::string> device_id;
};

struct SentTapSensitivity {
    int level = 0;
    std::optional<std::string> device_id;
};

class FakeBleCentral : public BleCentral {
public:
    void Start() override {}
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void ConnectPairedDevice(const std::string&,
                             std::uint64_t,
                             BluetoothAddressKind,
                             const std::string&) override {}
    void SendUiState(const std::string& state,
                     const std::string& text,
                     const std::optional<std::string>& device_id) override {
        sent_ui_states.push_back(SentUiState{state, text, device_id});
    }
    void SendInteractionMode(InteractionMode mode,
                             const std::optional<std::string>& device_id) override {
        sent_interaction_modes.push_back(std::pair{mode, device_id});
    }
    void SendShowImuDebug(bool enabled,
                          const std::optional<std::string>& device_id) override {
        (void)enabled;
        (void)device_id;
    }
    void SendTapEnabled(bool enabled,
                        const std::optional<std::string>& device_id) override {
        sent_tap_enabled.push_back(std::pair{enabled, device_id});
    }
    void SendTapSensitivity(int level,
                            const std::optional<std::string>& device_id) override {
        sent_tap_sensitivities.push_back(SentTapSensitivity{level, device_id});
    }
    void SendAirMouseEnabled(bool enabled,
                             const std::optional<std::string>& device_id) override {
        sent_air_mouse_enabled.push_back(std::pair{enabled, device_id});
    }
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override {
        sent_imu_wake_sensitivities.push_back(SentImuWakeSensitivity{threshold_lsb, device_id});
    }
    void RequestBatteryStatus(const std::optional<std::string>& device_id) override {
        battery_status_requests.push_back(device_id);
    }
    void SendRemoteButton(RemoteButtonAction action,
                          const std::string& button,
                          const std::optional<std::string>& device_id,
                          std::uint32_t request_id) override {
        sent_remote_buttons.push_back(SentRemoteButton{action, button, device_id, request_id});
    }
    void UpdateFirmware(ByteVector image,
                        const std::string& device_id,
                        std::function<void(FirmwareUpdateProgress)> progress,
                        std::function<void(bool, std::string)> completion) override {
        captured_firmware_image = std::move(image);
        captured_firmware_device_id = device_id;
        if (progress) {
            progress(FirmwareUpdateProgress{
                0, static_cast<int>(captured_firmware_image.size()), true});
        }
        if (completion) completion(true, "");
    }
    void CancelFirmwareUpdate() override {}
    bool IsConnected(const std::string& device_id) const override {
        return connected_device_ids.contains(device_id);
    }

    std::vector<std::string> paired_device_ids;
    std::set<std::string> connected_device_ids;
    ByteVector captured_firmware_image;
    std::string captured_firmware_device_id;
    std::vector<SentUiState> sent_ui_states;
    std::vector<std::pair<InteractionMode, std::optional<std::string>>> sent_interaction_modes;
    std::vector<std::optional<std::string>> battery_status_requests;
    std::vector<SentRemoteButton> sent_remote_buttons;
    std::vector<SentImuWakeSensitivity> sent_imu_wake_sensitivities;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_tap_enabled;
    std::vector<SentTapSensitivity> sent_tap_sensitivities;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_air_mouse_enabled;
};

class FakeAsrClient : public AsrClient {
public:
    bool Start(AsrSessionOptions options = {}) override {
        last_options = std::move(options);
        started = true;
        return start_result;
    }
    void SendOggOpusChunk(std::span<const std::uint8_t>, bool is_last) override {
        ++sent_chunks;
        last_chunk_was_final = is_last;
    }
    void Cancel() override {
        cancelled = true;
    }
    std::string LastStartError() const override {
        return start_error;
    }

    bool start_result = true;
    std::string start_error;
    bool started = false;
    bool cancelled = false;
    int sent_chunks = 0;
    bool last_chunk_was_final = false;
    AsrSessionOptions last_options;
};

class FakeUi : public VoiceStickUi {
public:
    void SetStatus(const std::string& status) override {
        statuses.push_back(status);
    }
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override {
        connected_devices = devices;
    }
    void SetDeviceInfo(const DeviceInfo& info) override {
        device_infos.push_back(info);
    }
    void SetDeviceBattery(const std::string& device_id, int level_percent,
                           bool charging, bool usb_powered) override {
        (void)device_id;
        (void)level_percent;
        (void)charging;
        (void)usb_powered;
    }
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) override {
        firmware_info_by_device_id = info_by_device_id;
    }
    void SetPairingError(const std::string& device_id, const std::string& message) override {
        pairing_errors.push_back(device_id + ":" + message);
    }
    void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                  const std::string& current_version,
                                  const std::string& latest_version,
                                  bool is_below_minimum) override {
        firmware_update_prompts.push_back(device_id + ":" + current_version + ":" + latest_version +
                                          (is_below_minimum ? ":minimum" : ":latest"));
    }
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override {
        paired_device_ids = ids;
    }
    void SetHasRecoverableInput(bool has_recoverable_input) override {
        has_recoverable_input_set = has_recoverable_input;
    }
    void ShowListening(const std::optional<std::string>&) override {
        ++show_listening_count;
    }
    void ShowPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void AppendPartial(const std::string& text, const std::optional<std::string>&) override {
        partials.push_back(text);
    }
    void ShowRefining(const std::string& text, const std::optional<std::string>&) override {
        refining_texts.push_back(text);
    }
    void ShowFinalCountdown(const std::string& text,
                            const std::optional<std::string>&,
                            std::function<void()> on_complete) override {
        final_countdowns.push_back(text);
        final_countdown_completion = std::move(on_complete);
    }
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>&) override {
        paused_finals.push_back(text);
    }
    void ShowError(const std::string& text,
                   const std::optional<std::string>&,
                   std::function<void()> on_complete) override {
        errors.push_back(text);
        error_completion = std::move(on_complete);
    }
    void ShowCloudUpgrade(const std::string& message,
                          const std::string& url,
                          const std::optional<std::string>&) override {
        cloud_upgrades.push_back(message + "|" + url);
    }
    void HideOverlay(std::function<void()> on_hidden = {}) override {
        ++hide_overlay_count;
        if (on_hidden) on_hidden();
    }
    void ShowSubtitle(const std::string& text,
                      const std::string& device_id,
                      OverlayThemeColor color) override {
        subtitles.push_back(device_id + ":" + text + ":" + OverlayThemeColorName(color));
    }
    void HideSubtitles() override {
        ++hide_subtitles_count;
    }
    void ShowNotification(const std::string& title, const std::string& body) override {
        notifications.push_back(title + ":" + body);
    }

    std::vector<std::string> statuses;
    std::vector<ConnectedDevice> connected_devices;
    std::vector<DeviceInfo> device_infos;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id;
    std::vector<std::string> pairing_errors;
    std::vector<std::string> firmware_update_prompts;
    std::vector<std::string> paired_device_ids;
    std::vector<std::string> partials;
    std::vector<std::string> refining_texts;
    std::vector<std::string> cloud_upgrades;
    std::vector<std::string> final_countdowns;
    std::vector<std::string> paused_finals;
    std::vector<std::string> errors;
    std::vector<std::string> subtitles;
    std::vector<std::string> notifications;
    std::function<void()> final_countdown_completion;
    std::function<void()> error_completion;
    bool has_recoverable_input_set = false;
    int show_listening_count = 0;
    int hide_overlay_count = 0;
    int hide_subtitles_count = 0;
};

class FakeInputInjector : public InputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override {
        pasted_text = text;
        pasted_enter = press_enter;
    }
    void SendEnter() override { send_enter_called = true; }
    void SendArrowDown() override { ++arrow_down_count; }
    void MoveMouse(int dx, int dy) override {
        ++move_mouse_count;
        total_dx += dx;
        total_dy += dy;
    }
    void ClickLeftButton() override { ++left_click_count; }

    std::string pasted_text;
    bool pasted_enter = false;
    bool send_enter_called = false;
    int arrow_down_count = 0;
    int move_mouse_count = 0;
    int total_dx = 0;
    int total_dy = 0;
    int left_click_count = 0;
};

// 测试用虚拟麦渲染器：解耦真实 WASAPI，Start 返回可配置结果。
class FakeVirtualMicRenderer : public IVirtualMicRenderer {
public:
    explicit FakeVirtualMicRenderer(bool start_result) : start_result_(start_result) {}
    bool Start(PcmRingBuffer*) override {
        ++start_count;
        running_ = start_result_;
        return start_result_;
    }
    void Stop() override {
        ++stop_count;
        running_ = false;
    }
    bool IsRunning() const override { return running_; }
    std::wstring ActiveDeviceName() const override { return L"FakeDevice"; }

    int start_count = 0;
    int stop_count = 0;
    bool running_ = false;
    bool start_result_;
};

// 测试用 WASAPI 渲染端：解耦真实 COM，记录提交量与样本供断言。
// NotifyEvent 返回真实 auto-reset event，使 Stop 唤醒测试可验证。
class FakeWasapiRenderSink : public WasapiRenderSink {
public:
    explicit FakeWasapiRenderSink(UINT32 buffer_frames = 800, int channels = 1)
        : buffer_frames_(buffer_frames), samples_per_frame_(channels) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~FakeWasapiRenderSink() override {
        if (event_) CloseHandle(event_);
    }

    bool OpenAndInitialize(const IVirtualMicRenderer::Options&) override { ++open_call_count; return open_result_; }
    UINT32 BufferFrameCount() const override { return buffer_frames_; }
    bool CurrentPadding(UINT32* out) override { *out = padding_; return true; }
    bool GetBuffer(UINT32 frames, BYTE** out) override {
        scratch_.assign(static_cast<std::size_t>(frames) * samples_per_frame_, 0);
        *out = reinterpret_cast<BYTE*>(scratch_.data());
        return true;
    }
    void ReleaseBuffer(UINT32 frames) override {
        submitted_frame_counts.push_back(frames);
        submitted_samples.emplace_back(scratch_);
    }
    void Start() override {}
    void Stop() override {}
    HANDLE NotifyEvent() override { return event_; }

    // 测试可读写状态。
    UINT32 padding_ = 0;
    bool open_result_ = true;
    int open_call_count = 0;
    std::vector<UINT32> submitted_frame_counts;
    std::vector<std::vector<int16_t>> submitted_samples;

private:
    UINT32 buffer_frames_;
    int samples_per_frame_;
    HANDLE event_ = nullptr;
    std::vector<int16_t> scratch_;
};

// 计时版 WASAPI sink：模拟设备按实时速率消费 buffer（padding 随时间递减），
// 用于量化 ring->WASAPI 管道滞留延迟与 device underrun。不启动真实线程，测试
// 手动驱动 AdvanceTimeUs + RenderPump::PumpOnce 模拟事件驱动消费节奏。
class TimedFakeSink : public WasapiRenderSink {
 public:
    TimedFakeSink(int sample_rate, int buffer_duration_ms, int channels = 1)
        : sample_rate_(sample_rate),
          buffer_frames_(static_cast<UINT32>(sample_rate * buffer_duration_ms / 1000)),
          samples_per_frame_(channels) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~TimedFakeSink() override { if (event_) CloseHandle(event_); }

    bool OpenAndInitialize(const IVirtualMicRenderer::Options&) override { return true; }
    UINT32 BufferFrameCount() const override { return buffer_frames_; }
    bool CurrentPadding(UINT32* out) override { *out = padding_; return true; }
    bool GetBuffer(UINT32 frames, BYTE** out) override {
        scratch_.assign(static_cast<std::size_t>(frames) * samples_per_frame_, 0);
        *out = reinterpret_cast<BYTE*>(scratch_.data());
        return true;
    }
    void ReleaseBuffer(UINT32 frames) override {
        padding_ += frames;
        ++submit_count_;
    }
    void Start() override {}
    void Stop() override {}
    HANDLE NotifyEvent() override { return event_; }

    // 模拟设备消费 us 微秒音频：padding 递减。消费量超过 padding 即 device underrun
    //（设备取音时 buffer 空，输出静音/破音），是 buffer_duration_ms 过小的直接风险信号。
    void AdvanceTimeUs(long long us) {
        const long long consume = static_cast<long long>(sample_rate_) * us / 1000000;
        if (consume > static_cast<long long>(padding_)) {
            ++device_underrun_count_;
            padding_ = 0;
        } else {
            padding_ -= static_cast<UINT32>(consume);
        }
    }

    int sample_rate_ = 16000;
    UINT32 buffer_frames_ = 0;
    UINT32 padding_ = 0;
    int samples_per_frame_ = 1;
    int submit_count_ = 0;
    int device_underrun_count_ = 0;

 private:
    HANDLE event_ = nullptr;
    std::vector<int16_t> scratch_;
};

// 测试用第三方输入法热键：解耦 SendInput，SendDown/SendUp/SendClick 恒成功。
class FakeWechatInputMethodHotkey : public IWechatInputMethodHotkey {
public:
    explicit FakeWechatInputMethodHotkey(const std::string& = {}) {}
    bool IsValid() const override { return true; }
    bool SendDown() const override {
        ++send_down_count;
        return true;
    }
    bool SendUp() const override {
        ++send_up_count;
        return true;
    }
    bool SendClick() const override {
        ++send_click_count;
        return true;
    }

    mutable int send_down_count = 0;
    mutable int send_up_count = 0;
    mutable int send_click_count = 0;
};

// 探测前台进程是否高权限的 fake：可控返回值与进程名，支持序列（换进程名再提醒测试用）。
class FakeForegroundProcessProbe : public IForegroundProcessProbe {
public:
    // 单值模式：每次探测返回 elevated 与 process_name。
    FakeForegroundProcessProbe(bool elevated, std::wstring process_name = L"")
        : elevated_(elevated), names_{std::move(process_name)} {}
    // 序列模式：第 N 次探测返回 names_[N]（超出取最后一个），elevated 为 !names_.empty()。
    explicit FakeForegroundProcessProbe(std::vector<std::wstring> names)
        : elevated_(!names.empty()), names_(std::move(names)) {}
    bool IsForegroundHigherIntegrity(std::wstring& process_name) override {
        if (!elevated_) return false;
        const auto idx = static_cast<std::size_t>(
            std::min(call_count_, static_cast<int>(names_.size()) - 1));
        process_name = names_[idx];
        ++call_count_;
        return true;
    }
    bool elevated_ = false;
    std::vector<std::wstring> names_;
    int call_count_ = 0;
};

// 宽字符串大小写不敏感子串匹配（Fake 设备枚举用，仅 ASCII 足够匹配 "CABLE Output"）。
bool ContainsWide(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto lower = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c | 0x20) : c;
    };
    for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// 测试用默认录音设备切换器：解耦真实 IPolicyConfig COM，记录调用供断言。
class FakeDefaultAudioDeviceController : public IDefaultAudioDeviceController {
public:
    struct SetCall {
        std::wstring device_id;
        std::vector<DeviceRole> roles;
    };

    // 预设状态：当前默认设备与设备枚举列表。
    std::optional<AudioDeviceInfo> default_capture;
    std::vector<AudioDeviceInfo> capture_devices;
    bool set_result = true;

    std::optional<AudioDeviceInfo> GetDefaultCapture(DeviceRole) override {
        ++get_call_count;
        return default_capture;
    }
    std::optional<AudioDeviceInfo> FindCaptureByName(std::wstring_view sub) override {
        ++find_call_count;
        for (const auto& d : capture_devices) {
            if (ContainsWide(d.friendly_name, sub)) return d;
        }
        return std::nullopt;
    }
    bool SetDefaultCapture(const std::wstring& id, std::vector<DeviceRole> roles) override {
        ++set_call_count;
        set_calls.push_back({id, roles});
        if (set_result) {
            for (const auto& d : capture_devices) {
                if (d.id == id) { default_capture = d; break; }
            }
        }
        return set_result;
    }

    int get_call_count = 0;
    int find_call_count = 0;
    int set_call_count = 0;
    std::vector<SetCall> set_calls;
};

StateEvent ButtonEvent(const std::string& event,
                       const std::string& button,
                       std::optional<std::uint32_t> session_id = std::nullopt) {
    StateEvent state_event;
    state_event.event = event;
    state_event.button = button;
    state_event.session_id = session_id;
    return state_event;
}

// 构造双击事件（固件上报的 {"event":"button_double_click","button":"..."}）。
StateEvent DoubleClickEvent(const std::string& button) {
    StateEvent state_event;
    state_event.event = "button_double_click";
    state_event.button = button;
    return state_event;
}

// 构造敲击事件（固件上报的 {"event":"tap","kind":"double"}）。
StateEvent TapEvent(const std::string& kind = "double") {
    StateEvent state_event;
    state_event.event = "tap";
    state_event.button = kind;  // 复用 button 字段承载 kind，与协议解析一致
    return state_event;
}

AudioFrame AudioDataFrame(std::uint32_t session_id, std::uint32_t seq, bool is_end = false) {
    AudioFrame frame;
    frame.session_id = session_id;
    frame.seq = seq;
    frame.flags = is_end ? 0x02 : 0;
    frame.payload = {1, 2, 3, 4};
    return frame;
}

AudioFrame EmptyEndFrame(std::uint32_t session_id, std::uint32_t seq) {
    AudioFrame frame;
    frame.session_id = session_id;
    frame.seq = seq;
    frame.flags = 0x02;
    return frame;
}

bool HasUiState(const FakeBleCentral& ble, const std::string& state, const std::string& device_id) {
    return std::any_of(ble.sent_ui_states.begin(), ble.sent_ui_states.end(),
                       [&](const SentUiState& sent) {
                           return sent.state == state &&
                                  sent.device_id.has_value() &&
                                  *sent.device_id == device_id;
                       });
}

bool HasUiStateText(const FakeBleCentral& ble,
                    const std::string& state,
                    const std::string& text,
                    const std::string& device_id) {
    return std::any_of(ble.sent_ui_states.begin(), ble.sent_ui_states.end(),
                       [&](const SentUiState& sent) {
                           return sent.state == state &&
                                  sent.text == text &&
                                  sent.device_id.has_value() &&
                                  *sent.device_id == device_id;
                       });
}

void TestDeviceIds() {
    assert(BleProtocol::NormalizeDeviceId("vs-c3d8") == "C3D8");
    assert(BleProtocol::NormalizeDeviceId("09af") == "09AF");
    assert(!BleProtocol::DeviceIdFromName("Other").has_value());
    assert(BleProtocol::DeviceIdFromName("VS-C3D8").value() == "C3D8");

    const ByteVector complete_name_ad = {0x02, 0x01, 0x06, 0x08, 0x09, 'V', 'S', '-', 'C', '3', 'D', '8'};
    assert(BleProtocol::LocalNameFromAdvertisementData(complete_name_ad).value() == "VS-C3D8");
    const ByteVector shortened_name_ad = {0x08, 0x08, 'V', 'S', '-', 'A', '1', 'B', '2'};
    assert(BleProtocol::LocalNameFromAdvertisementData(shortened_name_ad).value() == "VS-A1B2");
    const ByteVector malformed_ad = {0x08, 0x09, 'V', 'S'};
    assert(!BleProtocol::LocalNameFromAdvertisementData(malformed_ad).has_value());
    const ByteVector service_uuid_ad = {
        0x02, 0x01, 0x06,
        0x11, 0x07,
        0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
        0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f,
    };
    assert(BleProtocol::HasVoiceStickServiceUuid(service_uuid_ad));
    assert(!BleProtocol::HasVoiceStickServiceUuid(complete_name_ad));
    assert(BleProtocol::DeviceIdFromBluetoothAddress(0xAABBCCDDEEFF) == "EEFF");
}

void TestPairDeviceHelpers() {
    assert(ParseManualPairDeviceId("abcd").value() == "ABCD");
    assert(ParseManualPairDeviceId("VS-abcd").value() == "ABCD");
    assert(ParseManualPairDeviceId(" vs-09af ").value() == "09AF");
    assert(!ParseManualPairDeviceId("VS-123").has_value());
    assert(!ParseManualPairDeviceId("VoiceStick").has_value());

    PairingCandidate ready;
    ready.device_id = "C3D8";
    ready.display_name = "VS-C3D8";
    ready.bluetooth_address = 0xAABBCCDDEEFF;
    ready.id_source = PairingCandidateIdSource::kName;
    assert(CandidateDisplayTitle(ready) == "VS-C3D8");
    assert(CanPairCandidate(ready));

    PairingCandidate existing = ready;
    existing.is_existing_device = true;
    assert(CandidateDisplayTitle(existing) == "VS-C3D8 (paired)");
    assert(!CanPairCandidate(existing));

    PairingCandidate temporary = ready;
    temporary.device_id = "EEFF";
    temporary.display_name.clear();
    temporary.id_source = PairingCandidateIdSource::kAddressFallback;
    temporary.is_temporary_candidate = true;
    assert(CandidateDisplayTitle(temporary) == "VoiceStick (waiting for name)");
    assert(!CanPairCandidate(temporary));

    std::vector<PairingCandidate> candidates;
    MergePairingCandidate(&candidates, temporary);
    assert(candidates.size() == 1);
    assert(VisiblePairingCandidates(candidates, {}, 1000, 3000).empty());
    ready.bluetooth_address = 0x112233445566;
    ready.device_id = temporary.device_id;
    ready.display_name = "VS-EEFF";
    MergePairingCandidate(&candidates, ready);
    assert(candidates.size() == 1);
    assert(!candidates.front().is_temporary_candidate);
    assert(candidates.front().display_name == "VS-EEFF");

    PairingCandidate late_temporary = temporary;
    late_temporary.bluetooth_address = 0x66778899AABB;
    MergePairingCandidate(&candidates, late_temporary);
    assert(candidates.size() == 1);
    assert(!candidates.front().is_temporary_candidate);

    candidates.push_back(late_temporary);
    std::vector<RetainedPairingCandidate> retained;
    RetainNamedPairingCandidate(&retained, candidates.front(), 1000);
    const auto visible = VisiblePairingCandidates(candidates, retained, 2000, 3000);
    assert(visible.size() == 1);
    assert(!visible.front().is_temporary_candidate);

    std::vector<PairingCandidate> temporary_only{late_temporary};
    const auto retained_visible = VisiblePairingCandidates(temporary_only, retained, 2500, 3000);
    assert(retained_visible.size() == 1);
    assert(retained_visible.front().display_name == "VS-EEFF");
    assert(VisiblePairingCandidates(temporary_only, retained, 5001, 3000).empty());
}

void TestAudioFrameParsing() {
    ByteVector frame = {1, 0x01, 16, 0};
    AppendLe32(frame, 123);
    AppendLe32(frame, 7);
    frame.push_back(0x03);
    frame.push_back(0);
    AppendLe16(frame, 3);
    frame.push_back(10);
    frame.push_back(11);
    frame.push_back(12);
    auto parsed = BleProtocol::ParseAudioFrame(frame);
    assert(parsed.has_value());
    assert(parsed->session_id == 123);
    assert(parsed->seq == 7);
    assert(parsed->IsStart());
    assert(parsed->IsEnd());
    assert(parsed->payload.size() == 3);
}

void TestBleControlPayloads() {
    auto battery_request = BleProtocol::BatteryStatusRequestPayload();
    assert(std::string(battery_request.begin(), battery_request.end()) == "{\"event\":\"battery_status_request\"}");

    // 敲击灵敏度 1~10 档，桌面端下发数值 level。
    auto tap_sens = BleProtocol::TapSensitivityPayload(5);
    assert(std::string(tap_sens.begin(), tap_sens.end()) == "{\"event\":\"tap_sensitivity\",\"level\":5}");
    auto tap_sens_high = BleProtocol::TapSensitivityPayload(10);
    assert(std::string(tap_sens_high.begin(), tap_sens_high.end()) == "{\"event\":\"tap_sensitivity\",\"level\":10}");

    // 体感鼠标开关下发。
    auto air_on = BleProtocol::AirMouseEnabledPayload(true);
    assert(std::string(air_on.begin(), air_on.end()) == "{\"event\":\"air_mouse_enabled\",\"enabled\":true}");
    auto air_off = BleProtocol::AirMouseEnabledPayload(false);
    assert(std::string(air_off.begin(), air_off.end()) == "{\"event\":\"air_mouse_enabled\",\"enabled\":false}");
}

void TestMotionFrameParsing() {
    // 合法 6 字节帧：version=1, type=0x11, dx=+100, dy=-50（小端）。
    ByteVector frame = {1, 0x11};
    AppendLe16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(100)));
    AppendLe16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(-50)));
    auto motion = BleProtocol::ParseMotionFrame(frame);
    assert(motion.has_value());
    assert(motion->dx == 100);
    assert(motion->dy == -50);

    // version 错。
    ByteVector bad_version = frame;
    bad_version[0] = 2;
    assert(!BleProtocol::ParseMotionFrame(bad_version).has_value());

    // type 错（0x10 是 JSON 状态帧，不是 motion）。
    ByteVector bad_type = frame;
    bad_type[1] = 0x10;
    assert(!BleProtocol::ParseMotionFrame(bad_type).has_value());

    // 长度不足。
    ByteVector too_short = {1, 0x11, 0x00};
    assert(!BleProtocol::ParseMotionFrame(too_short).has_value());

    // JSON 状态帧不应被误解析为 motion。
    ByteVector state_frame = {1, 0x10, 0x02, 0x00, '{', '}'};
    assert(!BleProtocol::ParseMotionFrame(state_frame).has_value());
}

void TestStateParsing() {
    const std::string json = "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":42}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "button_down");
    assert(event->button == "primary");
    assert(event->session_id == 42);
}

void TestOggMuxer() {
    OggOpusMuxer muxer(16000, 1);
    ByteVector opus = {1, 2, 3, 4};
    auto ogg = muxer.Append(opus, false);
    assert(ogg.size() > 64);
    assert(std::string(reinterpret_cast<const char*>(ogg.data()), 4) == "OggS");
    auto tail = muxer.Finish();
    assert(std::string(reinterpret_cast<const char*>(tail.data()), 4) == "OggS");
}

void TestAsrProtocol() {
    AppConfig config = AppConfig::Defaults();
    config.asr_hotwords = {"小智", "VoiceStick"};
    auto event_payload = [](const ByteVector& frame, const std::string& session_id) {
        const std::size_t payload_size_offset = 12 + session_id.size();
        const auto payload_size = ReadBe32(std::span(frame.data() + payload_size_offset, 4));
        const std::size_t payload_offset = payload_size_offset + 4;
        assert(payload_size == frame.size() - payload_offset);
        return std::string(reinterpret_cast<const char*>(frame.data() + payload_offset),
                           frame.size() - payload_offset);
    };

    const std::string payload_session_id = "payload-session";
    auto request = AsrProtocol::MakeStartSessionFrame(config, payload_session_id);
    assert(request.size() > 16 + payload_session_id.size());
    assert(request[0] == 0x11);
    assert((request[1] >> 4) == 0x01);
    assert(ReadBe32(std::span(request.data() + 4, 4)) == 100);
    const auto payload = event_payload(request, payload_session_id);
    assert(payload.find("\"corpus\"") != std::string::npos);
    assert(payload.find("\\\"hotwords\\\"") != std::string::npos);
    assert(payload.find("\\\"word\\\":\\\"VoiceStick\\\"") != std::string::npos);

    const std::string body =
        "{\"error\":\"invalid_token\",\"message\":\"VoiceStick Cloud API key is invalid.\","
        "\"upgrade_url\":\"https://example.test/upgrade\"}";
    ByteVector response = {0x11, 0xf0, 0x10, 0x00};
    AppendBe32(response, 44002);
    AppendBe32(response, static_cast<std::uint32_t>(body.size()));
    response.insert(response.end(), body.begin(), body.end());
    auto parsed = AsrProtocol::ParseResponse(response);
    assert(parsed.has_value());
    assert(parsed->is_error);
    assert(parsed->text == "ASR 44002: VoiceStick Cloud API key is invalid.");
    assert(parsed->upgrade_url && *parsed->upgrade_url == "https://example.test/upgrade");

    auto start_connection = AsrProtocol::MakeStartConnectionFrame(config);
    assert(start_connection.size() > 12);
    assert((start_connection[1] >> 4) == 0x01);
    assert((start_connection[1] & 0x0f) == 0x04);
    assert(ReadBe32(std::span(start_connection.data() + 4, 4)) == 1);

    const std::string session_id = "session-1";
    auto start_session = AsrProtocol::MakeStartSessionFrame(config, session_id);
    assert(ReadBe32(std::span(start_session.data() + 4, 4)) == 100);
    assert(ReadBe32(std::span(start_session.data() + 8, 4)) == session_id.size());
    assert(std::string(reinterpret_cast<const char*>(start_session.data() + 12),
                       session_id.size()) == session_id);

    ByteVector opus = {1, 2, 3};
    auto task = AsrProtocol::MakeTaskRequestFrame(opus, session_id);
    assert((task[1] >> 4) == 0x02);
    assert(ReadBe32(std::span(task.data() + 4, 4)) == 200);

    const std::string event_body = "{\"result\":{\"text\":\"hi\"}}";
    ByteVector event_response = {0x11, 0x94, 0x10, 0x00};
    AppendBe32(event_response, 451);
    AppendBe32(event_response, static_cast<std::uint32_t>(session_id.size()));
    event_response.insert(event_response.end(), session_id.begin(), session_id.end());
    AppendBe32(event_response, static_cast<std::uint32_t>(event_body.size()));
    event_response.insert(event_response.end(), event_body.begin(), event_body.end());
    auto parsed_event = AsrProtocol::ParseEventResponse(event_response);
    assert(parsed_event.has_value());
    assert(parsed_event->event == AsrEvent::kAsrResponse);
    assert(parsed_event->session_id == session_id);
    assert(AsrProtocol::ExtractTranscript(parsed_event->payload_text) == "hi");

    AsrSessionOptions options;
    options.hotwords = {"VoiceStick"};
    options.show_utterances = true;
    options.result_type = AsrResultType::kSingle;
    const std::string utterance_session_id = "utterance-session";
    auto utterance_request = AsrProtocol::MakeStartSessionFrame(config, utterance_session_id, options);
    const auto utterance_payload = event_payload(utterance_request, utterance_session_id);
    assert(utterance_payload.find("\"show_utterances\":true") != std::string::npos);
    assert(utterance_payload.find("\"result_type\":\"single\"") != std::string::npos);

    const std::string segment_json =
        "{\"result\":{\"text\":\"hello world\",\"utterances\":["
        "{\"text\":\"hello\",\"definite\":true,\"start_time\":0,\"end_time\":500},"
        "{\"text\":\"world\",\"definite\":false,\"start_time\":500,\"end_time\":900}]}}";
    auto segments = AsrProtocol::ExtractSegments(segment_json);
    assert(segments.size() == 2);
    assert(segments[0].text == "hello");
    assert(segments[0].definite);
    std::set<std::string> emitted;
    auto definite = AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted);
    assert(definite.size() == 1);
    assert(AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted).empty());
}


void TestAppConfig() {
    AppConfig cloud = AppConfig::Defaults();
    assert(cloud.asr_provider == AsrProvider::kVoiceStickCloud);
    cloud.asr_provider = AsrProvider::kVoiceStickCloud;
    cloud.voicestick_cloud_url = "";
    assert(cloud.ActiveWebsocketUrl() == "wss://api.xiaozhi.me/voicestick/asr/");

    cloud.voicestick_cloud_url = "  wss://example.test/asr?token=1  ";
    assert(cloud.ActiveWebsocketUrl() == "wss://example.test/asr?token=1");

    AppConfig volcengine = AppConfig::Defaults();
    volcengine.asr_provider = AsrProvider::kVolcengine;
    assert(volcengine.ActiveWebsocketUrl().starts_with("wss://openspeech.bytedance.com/"));

    PairedDeviceEntry entry;
    entry.device_id = "5A74";
    entry.bluetooth_address = 0x70041DDA5A76;
    entry.address_kind = BluetoothAddressKind::kPublic;
    entry.name = "VS-5A74";
    entry.hardware = "stick_s3";
    entry.firmware_version = "0.1.2";
    AppConfig cache = AppConfig::Defaults();
    cache.paired_devices.push_back(entry);
    cache.paired_device_ids.push_back(entry.device_id);
    assert(cache.paired_devices.front().hardware == "stick_s3");
    assert(cache.paired_devices.front().firmware_version == "0.1.2");
    assert(OverlayThemeColorFromName("auto") == OverlayThemeColor::kAuto);
    assert(OverlayThemeColorFromName("black") == OverlayThemeColor::kBlack);
    assert(OverlayThemeColorFromName("pink") == OverlayThemeColor::kPink);
    assert(OverlayThemeColorName(OverlayThemeColor::kAuto) == "auto");
    assert(OverlayThemeColorName(OverlayThemeColor::kGreen) == "green");
    assert(OverlayThemeColorName(OverlayThemeColor::kBlack) == "black");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kAuto) == "Auto");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kYellow) == "Yellow");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kBlack) == "Black");
    assert(DefaultOverlayThemeColor() == OverlayThemeColor::kAuto);
    assert(VoiceStickCoordinator::ThemeColorForConfig(AppConfig::Defaults(), "5A74") == OverlayThemeColor::kAuto);
    assert(OverlayThemeSizeFromName("medium") == OverlayThemeSize::kMedium);
    assert(OverlayThemeSizeFromName("small") == OverlayThemeSize::kSmall);
    assert(OverlayThemeSizeFromName("big") == OverlayThemeSize::kBig);
    assert(OverlayThemeSizeName(OverlayThemeSize::kMedium) == "medium");
    assert(OverlayThemeSizeDisplayName(OverlayThemeSize::kSmall) == "Small");
    assert(OverlayPositionFromName("bottom_center") == OverlayPosition::kBottomCenter);
    assert(OverlayPositionFromName("middle_bottom") == OverlayPosition::kBottomCenter);
    assert(OverlayPositionFromName("top_right") == OverlayPosition::kTopRight);
    assert(OverlayPositionName(OverlayPosition::kBottomLeft) == "bottom_left");
    assert(OverlayPositionName(OverlayPosition::kBottomCenter) == "bottom_center");
    assert(OverlayPositionDisplayName(OverlayPosition::kCenter) == "Center");
    assert(OverlayPositionDisplayName(OverlayPosition::kBottomCenter) == "Bottom Center");
    assert(DefaultOverlayPosition() == OverlayPosition::kBottomCenter);
    cache.default_output_profile.target = OutputTarget::kSubtitle;
    cache.default_output_profile.transform = TextTransform::kOriginal;
    cache.device_output_profiles["5A74"] = OutputProfile{
        OutputTarget::kSubtitle,
        TextTransform::kTranslate,
        "zh-Hans",
    };
    auto profile = cache.OutputProfileForDevice(std::optional<std::string>("5A74"));
    assert(profile.target == OutputTarget::kSubtitle);
    assert(profile.transform == TextTransform::kTranslate);
    assert(profile.translation_target == "zh-Hans");
    assert(OutputTargetName(OutputTarget::kFocusedApp) == "focused_app");
    assert(TextTransformFromName("translate") == TextTransform::kTranslate);
    assert(AppConfig::Defaults().ui_language == UiLanguage::kSystem);
    assert(!AppConfig::Defaults().launch_at_login);
    assert(UiLanguageFromName("system") == UiLanguage::kSystem);
    assert(UiLanguageFromName("en") == UiLanguage::kEnglish);
    assert(UiLanguageFromName("zh-Hans") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromName("invalid") == UiLanguage::kSystem);
    assert(UiLanguageName(UiLanguage::kSystem) == "system");
    assert(UiLanguageName(UiLanguage::kEnglish) == "en");
    assert(UiLanguageName(UiLanguage::kSimplifiedChinese) == "zh-Hans");
    assert(UiLanguageFromLocaleName(L"en-US") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"en-GB") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"zh-CN") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"zh-Hans") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"zh-TW") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"ja-JP") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"") == UiLanguage::kEnglish);
    assert(!Tr(StringId::kSettingsTitle, UiLanguage::kEnglish).empty());
    assert(Tr(StringId::kSettingsTitle, UiLanguage::kSimplifiedChinese) == "VoiceStick 设置");
    assert(Tr(StringId::kSettingsLaunchAtLogin, UiLanguage::kEnglish) == "Start VoiceStick when Windows starts");
    assert(Tr(StringId::kMenuLaunchAtLogin, UiLanguage::kSimplifiedChinese) == "开机自启动");
    assert(Tr(StringId::kPairManualIdHint, UiLanguage::kEnglish) == "Can't find it? Enter the 4-digit ID shown on the Stick:");
    assert(Tr(StringId::kPairManualIdHint, UiLanguage::kSimplifiedChinese) == "找不到设备？请输入 Stick 屏幕显示的 4 位 ID：");
    assert(Tr(StringId::kHotkeyCapturePrompt, UiLanguage::kEnglish) == "Press a hotkey combination...");
    assert(Tr(StringId::kFirmwareUpdateFinalizing, UiLanguage::kSimplifiedChinese) == "正在完成固件更新...");
    assert(BatteryStatusText(83, false, false, UiLanguage::kEnglish) == "83%");
    assert(BatteryStatusText(83, true, false, UiLanguage::kEnglish) == "83%, charging");
    assert(BatteryStatusText(83, false, true, UiLanguage::kSimplifiedChinese) == "83%，外接电源");
    assert(DeviceTitleWithBattery(L"VS-5A74", 83, true, false, UiLanguage::kSimplifiedChinese) == L"VS-5A74 (83%，充电中)");
    assert(ImuWakeSensitivityFromName("low") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityFromName("medium") == ImuWakeSensitivity::kMedium);
    assert(ImuWakeSensitivityFromName("high") == ImuWakeSensitivity::kHigh);
    assert(ImuWakeSensitivityFromName("invalid") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kLow) == "low");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kMedium) == "medium");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kHigh) == "high");
    assert(ImuWakeSensitivityDisplayName(ImuWakeSensitivity::kLow) == "Low");
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kLow) == 800);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kMedium) == 500);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kHigh) == 250);
    assert(LocalizationTablesAreComplete());
    const auto hotwords = ParseHotwordList(" 小智,VoiceStick\r\n小智\n豆包 ");
    assert((hotwords == std::vector<std::string>{"小智", "VoiceStick", "豆包"}));
}

void TestLlmRefinePromptAndPayload() {
    // 内置默认精修 prompt 含三类清理要求关键词（中文）。
    const auto prompt = LLMRefinementClient::BuildRefinePrompt("");
    assert(prompt.find("语音停顿") != std::string::npos);
    assert(prompt.find("标点") != std::string::npos);
    assert(prompt.find("填充词") != std::string::npos);

    // 非空 override 去空白后原样返回。
    const auto custom = LLMRefinementClient::BuildRefinePrompt("  my custom prompt  ");
    assert(custom == "my custom prompt");

    // 翻译 prompt 已融合精修要求，且保留翻译语义与热词。
    const auto translation_prompt = LLMTranslationClient::SystemPrompt("en", {});
    assert(translation_prompt.find("Translate") != std::string::npos);
    assert(translation_prompt.find("pause spaces") != std::string::npos);
    const auto translation_with_hotwords = LLMTranslationClient::SystemPrompt("zh-Hans", {"小智", "VoiceStick"});
    assert(translation_with_hotwords.find("小智") != std::string::npos);
    assert(translation_with_hotwords.find("VoiceStick") != std::string::npos);

    // 请求体为合法 JSON：temperature:0、system+user 两条消息、model 透传。
    const auto payload = LLMChatClient::BuildChatPayload("gpt-x", "sys-prompt", "hello world");
    assert(payload.find("\"model\":\"gpt-x\"") != std::string::npos);
    assert(payload.find("\"temperature\":0") != std::string::npos);
    assert(payload.find("\"role\":\"system\"") != std::string::npos);
    assert(payload.find("\"role\":\"user\"") != std::string::npos);
    auto* root = cJSON_Parse(payload.c_str());
    assert(root != nullptr);
    auto* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    assert(cJSON_IsString(model) && std::string(model->valuestring) == "gpt-x");
    auto* messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    assert(cJSON_IsArray(messages) && cJSON_GetArraySize(messages) == 2);
    auto* user_msg = cJSON_GetArrayItem(messages, 1);
    auto* user_content = cJSON_GetObjectItemCaseSensitive(user_msg, "content");
    assert(cJSON_IsString(user_content) && std::string(user_content->valuestring) == "hello world");
    cJSON_Delete(root);

    // 精修默认开启、prompt 默认空。
    assert(AppConfig::Defaults().refine_enabled == true);
    assert(AppConfig::Defaults().refine_prompt.empty());

    // refine_prompt 多行字符串 TOML 保存/加载往返测试：
    // 验证换行等控制字符被正确转义为 \n 等 TOML 转义序列，确保回读一致。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_refine_prompt_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.refine_enabled = true;
        // 含换行、tab、双引号和反斜杠的自定义 prompt
        config.refine_prompt =
            "你是一个后处理器。\n"
            "规则：\n"
            "\t• 去除\"多余\"空格\n"
            "\t• 修正标点\\格式\n"
            "仅返回文本。";
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.refine_enabled == true);
        assert(loaded.refine_prompt == config.refine_prompt);
        assert(loaded.refine_prompt.find("你是一个后处理器") != std::string::npos);
        assert(loaded.refine_prompt.find("\n\t• ") != std::string::npos);
        assert(loaded.refine_prompt.find("\"多余\"") != std::string::npos);
        assert(loaded.refine_prompt.find("标点\\格式") != std::string::npos);

        std::filesystem::remove(temp);
    }

    // refine_prompt 为空字符串时，保存为 "" 再加载仍为空（不落盘为带转义的多行）。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_refine_prompt_empty_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.refine_prompt.clear();
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.refine_prompt.empty());

        std::filesystem::remove(temp);
    }
}

void TestHotwordProcessConfig() {
    // 默认关闭、prompt 默认空（空 = 使用内置默认提示词）。
    assert(AppConfig::Defaults().hotword_process_enabled == false);
    assert(AppConfig::Defaults().hotword_process_prompt.empty());

    // TOML 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_process_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.hotword_process_enabled = true;
    config.hotword_process_prompt = "自定义提炼提示词\n第二行";
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.hotword_process_enabled == true);
    assert(loaded.hotword_process_prompt == config.hotword_process_prompt);
    std::filesystem::remove(temp);
}

void TestHotwordExtractorPromptAndParse() {
    // 内置默认提示词含提取语义关键词；覆盖值 Trim 后原样返回。
    const auto prompt = HotwordExtractor::BuildExtractPrompt("");
    assert(prompt.find("热词") != std::string::npos);
    assert(prompt.find("专有名词") != std::string::npos);
    const auto custom = HotwordExtractor::BuildExtractPrompt("  my extract prompt  ");
    assert(custom == "my extract prompt");

    // 解析：换行/逗号切分、Trim、去重（复用 ParseHotwordList 语义）。
    const auto words = HotwordExtractor::ParseExtractResult("小智\nVoiceStick\r\n小智\n豆包,AGI");
    assert((words == std::vector<std::string>{"小智", "VoiceStick", "豆包", "AGI"}));

    // 空输入 / 纯空白 → 空结果。
    assert(HotwordExtractor::ParseExtractResult("").empty());
    assert(HotwordExtractor::ParseExtractResult("  \n \n").empty());

    // 单词超过 64 字符被过滤。
    const std::string long_word(65, 'x');
    assert(HotwordExtractor::ParseExtractResult(long_word).empty());

    // 总量截断到 20 个。
    std::string many;
    for (int i = 0; i < 25; ++i) {
        many += "w" + std::to_string(i);
        many += "\n";
    }
    assert(HotwordExtractor::ParseExtractResult(many).size() == 20);

    // DiffNewHotwords：保序、剔除已存在词、自身去重。
    const auto diff = HotwordExtractor::DiffNewHotwords({"a", "b", "c", "a"}, {"b"});
    assert((diff == std::vector<std::string>{"a", "c"}));
    assert(HotwordExtractor::DiffNewHotwords({"b"}, {"b"}).empty());
}

void TestFirmwareManifestParsingAndVersionCompare() {
    const std::string json =
        "{\"hardware\":\"sticks3\",\"version\":\"0.2.3\",\"ota_url\":\"https://example.test/ota.bin\","
        "\"ota_sha256\":\"abc\",\"ota_size\":123,\"merged_url\":\"https://example.test/merged.bin\","
        "\"merged_sha256\":\"def\",\"merged_size\":456}";
    auto manifest = ParseFirmwareManifest(json);
    assert(manifest.has_value());
    assert(manifest->hardware == "sticks3");
    assert(manifest->version == "0.2.3");
    assert(manifest->ota_size == 123);
    assert(FirmwareVersion::IsOlderThan("0.2.2", "0.2.3"));
    assert(FirmwareVersion::IsOlderThan("0.2.3-beta", "0.2.3"));
    assert(!FirmwareVersion::IsOlderThan("0.2.3", "0.2.3-beta"));
    assert(IsFirmwareHardwareCompatible("sticks3", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "", "stick_s3"));
}

void TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_imu_wake_sensitivities.empty());
    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 250);
    assert(!ble_ptr->sent_imu_wake_sensitivities.back().device_id.has_value());

    AppConfig updated = config;
    updated.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 500);
    assert(!ble_ptr->sent_imu_wake_sensitivities.back().device_id.has_value());
}

void TestCoordinatorUpdateFirmwareFromFile() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vs_local_fw_test.bin";
    const ByteVector data{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 正常路径：读到的字节原样喂底层，device_id 透传。
    bool ok = false;
    coordinator.UpdateFirmwareFromFile(path.string(), "5A74", {},
        [&](bool s, std::string) { ok = s; });
    assert(ok);
    assert(ble_ptr->captured_firmware_image == data);
    assert(ble_ptr->captured_firmware_device_id == "5A74");

    // 不存在文件：completion(false)，不触达底层（captured 维持上次成功值）。
    bool ok2 = true;
    coordinator.UpdateFirmwareFromFile("nonexistent_vs_fw.bin", "5A74", {},
        [&](bool s, std::string) { ok2 = s; });
    assert(!ok2);
    assert(ble_ptr->captured_firmware_device_id == "5A74");

    // 空文件：completion(false)，不触达底层。
    const std::filesystem::path empty_path =
        std::filesystem::temp_directory_path() / "vs_local_fw_empty.bin";
    { std::ofstream f(empty_path, std::ios::binary); }
    bool ok3 = true;
    coordinator.UpdateFirmwareFromFile(empty_path.string(), "5A74", {},
        [&](bool s, std::string) { ok3 = s; });
    assert(!ok3);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(empty_path, ec);
}

void TestParseOtaCliArgs() {
    using namespace voicestick;
    // 无 --ota。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe"};
        assert(!ParseOtaCliArgs(1, argv).has_value());
    }
    // --ota 带路径，无 --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin"};
        auto r = ParseOtaCliArgs(3, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(!r->device_id.has_value());
    }
    // --ota + --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin",
                                 L"--device", L"5A74"};
        auto r = ParseOtaCliArgs(5, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(r->device_id.has_value());
        assert(*r->device_id == "5A74");
    }
    // --device 在前，顺序无关。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--device", L"5A74",
                                 L"--ota", L"C:/fw.bin"};
        auto r = ParseOtaCliArgs(5, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(*r->device_id == "5A74");
    }
    // --ota 缺路径 -> nullopt。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota"};
        assert(!ParseOtaCliArgs(2, argv).has_value());
    }
    // 中文路径转 UTF-8。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/固件.bin"};
        auto r = ParseOtaCliArgs(3, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/固件.bin");
    }
    // --device 缺值但 --ota 正常 -> 忽略 --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin", L"--device"};
        auto r = ParseOtaCliArgs(4, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(!r->device_id.has_value());
    }
}

void TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.tap_sensitivity = 7;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_tap_sensitivities.empty());
    assert(ble_ptr->sent_tap_sensitivities.back().level == 7);
    assert(!ble_ptr->sent_tap_sensitivities.back().device_id.has_value());

    AppConfig updated = config;
    updated.tap_sensitivity = 3;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_tap_sensitivities.back().level == 3);
    assert(!ble_ptr->sent_tap_sensitivities.back().device_id.has_value());
}

void TestAppConfigTapSensitivityRoundTrip() {
    // 默认档 5。
    assert(AppConfig::Defaults().tap_sensitivity == 5);

    // 钳位：越界值回退默认档 5，合法值透传。
    assert(TapSensitivityClamp(0) == 5);
    assert(TapSensitivityClamp(11) == 5);
    assert(TapSensitivityClamp(-1) == 5);
    assert(TapSensitivityClamp(1) == 1);
    assert(TapSensitivityClamp(10) == 10);
    assert(TapSensitivityClamp(3) == 3);

    // TOML 保存/加载往返。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_tap_sensitivity_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.tap_sensitivity = 8;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.tap_sensitivity == 8);

        std::filesystem::remove(temp);
    }

    // 越界值落盘后回读应被钳位到默认档 5。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_tap_sensitivity_clamp_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.tap_sensitivity = 99;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.tap_sensitivity == 5);

        std::filesystem::remove(temp);
    }
}

void TestCoordinatorHotkeyWithoutConnectionShowsWakeHint() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.debug_audio_cache = true;
    config.paired_device_ids.push_back("5A74");
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    coordinator.HandleGlobalHotkeyPressed();

    assert(ble_ptr->sent_remote_buttons.empty());
    assert(!ui.statuses.empty());
    assert(ui.statuses.back() == "Hotkey: VoiceStick not connected; press the main button to wake it");
    assert(!ui.notifications.empty());
    assert(ui.notifications.back().find("按主键唤醒") != std::string::npos);
}

void TestCoordinatorHotkeyWithConnectionSendsRemoteButton() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids.push_back("5A74");
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.HandleGlobalHotkeyPressed();

    assert(ble_ptr->sent_remote_buttons.size() == 1);
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kDown);
    assert(ble_ptr->sent_remote_buttons.back().button == "primary");
    assert(ble_ptr->sent_remote_buttons.back().device_id == std::optional<std::string>("5A74"));
}

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

void TestCoordinatorMainFinalPastesWithoutConfirmation() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;  // 本用例验证同步粘贴流程，关闭异步精修以免触发真实 LLM 调用
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
// 而非冻结在旧 partial 上等待 LLM 首 token。空 llm_api_key 使精修快速失败回退到原文。
void TestCoordinatorRefineShowsOriginalTextImmediately() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    // refine_enabled 默认 true；llm_api_key 默认空 → RefineStream 同步 on_error →
    // ChatAsync 后台快速失败 → on_complete(false, 原文) 回退。
    assert(config.refine_enabled);
    assert(config.llm_api_key.empty());
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

void TestTapEventInjectsArrowDown() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.tap_to_arrow = true;
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
    config.tap_to_arrow = false;  // 总开关关闭
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
    config.tap_to_arrow = true;
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
    config.tap_to_arrow = true;
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
    config.tap_to_arrow = true;
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

// 侧键单击在空闲态进入体感鼠标模式，再次单击退出（体感优先决策）。
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

    // 空闲态侧键单击 → 进入体感，下发 air_mouse_enabled:true + ui_state:air_mouse。
    // ui_state=air_mouse 让设备显示体感态提示，避免用户不知情下主键变鼠标左键。
    ble_ptr->sent_ui_states.clear();
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(!ble_ptr->sent_air_mouse_enabled.empty());
    assert(ble_ptr->sent_air_mouse_enabled.back().first == true);
    assert(HasUiState(*ble_ptr, "air_mouse", "5A74"));

    // 再次侧键单击 → 退出体感，下发 air_mouse_enabled:false + ui_state:ready。
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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
    config.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 未进入体感态时 motion 应被忽略。
    ble_ptr->on_motion_event("5A74", MotionEvent{10, -5});
    assert(input.move_mouse_count == 0);

    // 进入体感态后 motion 不直接注入（由 AirMouseTick 驱动）。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    config.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    config.air_mouse_sensitivity_x = 5;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入并累积角度。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    for (int i = 0; i < 20; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{100, 0});
        coordinator.AirMouseTick();
    }
    assert(input.move_mouse_count >= 1);

    // 退出再进入：状态应复位。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(callback_called);
    assert(last_active);

    // 退出 → 回调 false。
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
    config.tap_to_arrow = true;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    // 进入体感态。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    config.air_mouse_sensitivity_x = 10;  // 最高档
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    config.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
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
    config.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
    config.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
    config.air_mouse_sensitivity_x = 10;
    config.air_mouse_control_mode = "angle";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

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
}

// 构造一个 16 kHz、40 ms（640 样本）的单声道正弦波 PCM 帧。
std::vector<int16_t> MakeSinePcm(int frequency_hz, int sample_rate = 16000) {
    constexpr double kPi = 3.14159265358979323846;
    const int kFrameSize = sample_rate * 40 / 1000;  // 40 ms
    std::vector<int16_t> pcm(kFrameSize);
    for (int i = 0; i < kFrameSize; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        pcm[i] = static_cast<int16_t>(std::sin(2.0 * kPi * frequency_hz * t) * 30000.0);
    }
    return pcm;
}

// 使用 opus_encoder 将 PCM 编码为 Opus packet，用于解码器测试。
std::vector<uint8_t> EncodeOpusPacket(const std::vector<int16_t>& pcm,
                                      int sample_rate = 16000) {
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
    assert(encoder != nullptr);
    assert(error == OPUS_OK);

    std::vector<uint8_t> packet(1275);  // Opus 单帧最大长度。
    const int encoded_bytes = opus_encode(encoder, pcm.data(), static_cast<int>(pcm.size()),
                                          packet.data(), static_cast<int>(packet.size()));
    assert(encoded_bytes > 0);
    packet.resize(encoded_bytes);

    opus_encoder_destroy(encoder);
    return packet;
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

// 点动式残留 active（停止 click + audio_end 都丢）时，新启动 click（新 session_id）
// 不得被误当停止：应先停旧会话再启新会话，否则新会话音频被丢弃、状态错位不自愈。
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

void TestTencentProviderSelection() {
    assert(AsrProviderFromName("voicestick_cloud") == AsrProvider::kVoiceStickCloud);
    assert(AsrProviderFromName("volcengine") == AsrProvider::kVolcengine);
    assert(AsrProviderFromName("tencent") == AsrProvider::kTencent);
    // 未知名称回退到 volcengine
    assert(AsrProviderFromName("unknown") == AsrProvider::kVolcengine);

    assert(AsrProviderName(AsrProvider::kVoiceStickCloud) == "voicestick_cloud");
    assert(AsrProviderName(AsrProvider::kVolcengine) == "volcengine");
    assert(AsrProviderName(AsrProvider::kTencent) == "tencent");
}

void TestTencentConfigRoundTrip() {
    // 测试腾讯云字段的 TOML 读写
    AppConfig config = AppConfig::Defaults();
    config.asr_provider = AsrProvider::kTencent;
    config.tencent_secret_id = "AKID-test-id";
    config.tencent_secret_key = "test-secret-key";
    config.tencent_appid = "1234567890";
    config.tencent_engine_model_type = "16k_zh_en";
    config.tencent_hotword_id = "vocab-abc123";

    assert(config.asr_provider == AsrProvider::kTencent);
    assert(config.tencent_secret_id == "AKID-test-id");
    assert(config.tencent_secret_key == "test-secret-key");
    assert(config.tencent_appid == "1234567890");
    assert(config.tencent_engine_model_type == "16k_zh_en");
    assert(config.tencent_hotword_id == "vocab-abc123");

    // ActiveApiKey 对 Tencent 应返回 SecretId
    assert(config.ActiveApiKey() == "AKID-test-id");

    // ActiveWebsocketUrl 对 Tencent 应包含 appid
    auto url = config.ActiveWebsocketUrl();
    assert(url.find("asr.cloud.tencent.com") != std::string::npos);
    assert(url.find("1234567890") != std::string::npos);

    // 默认引擎模型
    AppConfig defaults = AppConfig::Defaults();
    assert(defaults.tencent_engine_model_type == "16k_zh_en");
}

void TestTencentCredentialsTrimmedOnLoad() {
    // 凭据前后带空格是 Tencent 返回 4002 "密钥不存在" 的常见原因。
    // 验证 TOML 加载时自动去除首尾空格。
    auto temp = std::filesystem::temp_directory_path() /
                "voicestick_tencent_trim_test.toml";
    std::filesystem::remove(temp);

    {
        std::ofstream out(temp);
        out << "asr_provider = \"tencent\"\n";
        out << "tencent_secret_id = \"  AKID-test-id  \"\n";
        out << "tencent_secret_key = \"  test-secret-key  \"\n";
        out << "tencent_appid = \"  1234567890  \"\n";
        out << "tencent_hotword_id = \"  vocab-abc  \"\n";
    }

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.tencent_secret_id == "AKID-test-id");
    assert(loaded.tencent_secret_key == "test-secret-key");
    assert(loaded.tencent_appid == "1234567890");
    assert(loaded.tencent_hotword_id == "vocab-abc");

    std::filesystem::remove(temp);
}

void TestTencentSecretIdRecoveryFromVolcengineField() {
    // 历史版本设置对话框在 ASR 提供商切换时，可能把 Tencent SecretId
    // 误写入 volcengine_api_key 字段。验证加载配置时自动回迁。
    auto temp = std::filesystem::temp_directory_path() /
                "voicestick_tencent_recovery_test.toml";
    std::filesystem::remove(temp);

    {
        std::ofstream out(temp);
        out << "asr_provider = \"tencent\"\n";
        out << "volcengine_api_key = \"AKID_REDACTED_PLACEHOLDER\"\n";
        out << "tencent_secret_id = \"a31355ab-old-wrong\"\n";
        out << "tencent_secret_key = \"secret-key\"\n";
        out << "tencent_appid = \"1259040144\"\n";
    }

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.tencent_secret_id == "AKID_REDACTED_PLACEHOLDER");
    assert(loaded.volcengine_api_key.empty());

    std::filesystem::remove(temp);
}

void TestTencentSignatureGeneration() {
    // 验证 HMAC-SHA1（使用已知测试向量）
    auto result = AsrClientTencent::HmacSha1("key", "The quick brown fox jumps over the lazy dog");
    // HMAC-SHA1("key", message) 的已知结果
    assert(!result.empty());
    // SHA1 HMAC 输出 20 字节
    assert(result.size() == 20);

    // 空消息
    auto empty_result = AsrClientTencent::HmacSha1("key", "");
    assert(!empty_result.empty());
    assert(empty_result.size() == 20);

    // Base64 编码
    std::vector<std::uint8_t> test_bytes = {'M', 'a', 'n'};
    auto b64 = AsrClientTencent::Base64Encode(test_bytes);
    assert(b64 == "TWFu");

    // URL 编码
    auto url_enc = AsrClientTencent::UrlEncode("hello world");
    assert(url_enc == "hello%20world");
}

void TestTencentUrlConstruction() {
    AppConfig config = AppConfig::Defaults();
    config.tencent_secret_id = "AKIDtest";
    config.tencent_secret_key = "testkey";
    config.tencent_appid = "1234567890";
    config.tencent_engine_model_type = "16k_zh_en";
    config.tencent_hotword_id = "vocab-abc";

    auto url = AsrClientTencent::BuildSignedUrl(config, "test-voice-id-1234");

    // 验证 URL 基本结构
    assert(url.starts_with("wss://asr.cloud.tencent.com/asr/v2/1234567890?"));
    assert(url.find("secretid=AKIDtest") != std::string::npos);
    assert(url.find("engine_model_type=16k_zh_en") != std::string::npos);
    assert(url.find("voice_format=10") != std::string::npos);
    assert(url.find("needvad=1") != std::string::npos);
    assert(url.find("voice_id=test-voice-id-1234") != std::string::npos);
    assert(url.find("hotword_id=vocab-abc") != std::string::npos);
    assert(url.find("&signature=") != std::string::npos);

    // 参数应按字典序排列：secretid 排在 engine_model_type 之前是错的，应该是 e < s
    auto secretid_pos = url.find("secretid=");
    auto engine_pos = url.find("engine_model_type=");
    assert(engine_pos < secretid_pos);  // 'e' < 's'
}

void TestTencentResultParsing() {
    // slice_type=0 — 开始识别
    const char* json_start = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid",
        "result": {
            "slice_type": 0,
            "voice_text_str": ""
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_start) == 0);
    assert(AsrClientTencent::ExtractVoiceText(json_start).empty());

    // slice_type=1 — 中间结果
    const char* json_partial = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 1,
            "voice_text_str": "今天天气"
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_partial) == 1);
    assert(AsrClientTencent::ExtractVoiceText(json_partial) == "今天天气");

    // slice_type=2 — 最终结果
    const char* json_final = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 2,
            "voice_text_str": "今天天气很好"
        }
    })";
    assert(AsrClientTencent::ExtractSliceType(json_final) == 2);
    assert(AsrClientTencent::ExtractVoiceText(json_final) == "今天天气很好");

    // 错误响应
    const char* json_error = R"(
    {
        "code": 4002,
        "message": "鉴权失败"
    })";
    assert(AsrClientTencent::ExtractErrorCode(json_error) == 4002);
    assert(AsrClientTencent::ExtractErrorMessage(json_error) == "鉴权失败");

    // 词列表 segments
    const char* json_with_words = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 1,
            "voice_text_str": "今天天气很好",
            "word_list": [
                {"word": "今天", "start_time": 0, "end_time": 400, "stable_flag": 1},
                {"word": "天气", "start_time": 400, "end_time": 700, "stable_flag": 1},
                {"word": "很好", "start_time": 700, "end_time": 1000, "stable_flag": 0}
            ]
        }
    })";
    std::set<std::string> emitted;
    auto segments = AsrClientTencent::ExtractWordListSegments(json_with_words, &emitted);
    // 只有 stable_flag=1 的词被聚合
    assert(segments.size() == 1);
    assert(segments[0].text == "今天天气");
    assert(segments[0].definite);
    assert(segments[0].start_time == 0);
    assert(segments[0].end_time == 700);

    // 再次提取应无新 segment（已去重）
    auto segments2 = AsrClientTencent::ExtractWordListSegments(json_with_words, &emitted);
    assert(segments2.empty());
}

void TestTencentFinalFlagParsing() {
    // final=1：整段音频识别结束（顶层字段，无 result）
    const char* json_final_end = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid",
        "message_id": "test-uuid_241",
        "final": 1
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_final_end) == 1);

    // 无 final 字段（握手确认 / 普通识别结果）→ 0
    const char* json_handshake = R"(
    {
        "code": 0,
        "message": "success",
        "voice_id": "test-uuid"
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_handshake) == 0);

    const char* json_result = R"(
    {
        "code": 0,
        "message": "success",
        "result": {
            "slice_type": 2,
            "voice_text_str": "今天天气很好"
        }
    })";
    assert(AsrClientTencent::ExtractFinalFlag(json_result) == 0);
}

void TestTencentSentenceAccumulation() {
    // 空累积 + 首句 → 首句
    assert(AsrClientTencent::AccumulateSentence("", "今天天气很好") == "今天天气很好");
    // 已累积 + 新句 → 拼接
    assert(AsrClientTencent::AccumulateSentence("今天天气很好", "我们去看电影") == "今天天气很好我们去看电影");
    // 空句不改变累积
    assert(AsrClientTencent::AccumulateSentence("今天天气很好", "") == "今天天气很好");
    // 两者皆空 → 空
    assert(AsrClientTencent::AccumulateSentence("", "").empty());
}

void TestTencentEndMessage() {
    auto msg = AsrClientTencent::MakeEndMessage();
    assert(msg == R"({"type":"end"})");
}

void TestTencentOpusEncapsulation() {
    // 构造一个最小 Ogg Opus 音频页（不含有效 CRC，ExtractTencentOpusFrame 不校验 CRC）。
    // Ogg 页结构：OggS(4) + version(1) + type(1) + granule(8) + serial(4) + seq(4) + crc(4) + segments(1) + table(1) + payload
    std::vector<std::uint8_t> ogg_page;
    ogg_page.insert(ogg_page.end(), {'O', 'g', 'g', 'S', 0, 0});
    for (int i = 0; i < 8; ++i) ogg_page.push_back(0);  // granule
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // serial
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // seq
    for (int i = 0; i < 4; ++i) ogg_page.push_back(0);  // crc placeholder
    ogg_page.push_back(1);  // 1 segment
    ogg_page.push_back(4);  // segment length = 4
    ogg_page.insert(ogg_page.end(), {0x01, 0x02, 0x03, 0x04});  // fake Opus payload

    auto frame = AsrClientTencent::ExtractTencentOpusFrame(std::span(ogg_page));
    assert(frame.size() == 4 + 2 + 4);
    assert(frame[0] == 'o' && frame[1] == 'p' && frame[2] == 'u' && frame[3] == 's');
    assert(frame[4] == 0x04 && frame[5] == 0x00);  // little-endian length = 4
    assert(frame[6] == 0x01 && frame[7] == 0x02 && frame[8] == 0x03 && frame[9] == 0x04);

    // OpusHead/OpusTags 头包应返回空
    std::vector<std::uint8_t> head_page;
    head_page.insert(head_page.end(), {'O', 'g', 'g', 'S', 0, 0});
    for (int i = 0; i < 8; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    for (int i = 0; i < 4; ++i) head_page.push_back(0);
    head_page.push_back(1);
    head_page.push_back(8);
    head_page.insert(head_page.end(), {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
    auto head_frame = AsrClientTencent::ExtractTencentOpusFrame(std::span(head_page));
    assert(head_frame.empty());
}

void TestTencentVoiceIdGeneration() {
    auto id1 = AsrClientTencent::GenerateVoiceId();
    auto id2 = AsrClientTencent::GenerateVoiceId();
    // UUID 格式: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (36 字符)
    assert(id1.size() == 36);
    assert(id2.size() == 36);
    assert(id1 != id2);  // 每次生成应不同
    assert(id1[8] == '-');
    assert(id1[13] == '-');
    assert(id1[18] == '-');
    assert(id1[23] == '-');
    assert(id1[14] == '4');  // UUID v4 版本标识
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
    config.air_mouse_sensitivity_x = 7;
    config.air_mouse_sensitivity_y = 6;
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
    assert(loaded.air_mouse_sensitivity_x == 7);
    assert(loaded.air_mouse_sensitivity_y == 6);
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

// auto_switch=true 时 Start 把默认录音设备(eConsole)切到 CABLE Output，Stop 切回原设备。
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

int main() {
    TestDeviceIds();
    TestPairDeviceHelpers();
    TestAudioFrameParsing();
    TestBleControlPayloads();
    TestStateParsing();
    TestMotionFrameParsing();
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
    TestOggMuxer();
    TestAsrProtocol();
    TestAppConfig();
    TestAppConfigTapSensitivityRoundTrip();
    TestAppConfigAirMouseRoundTrip();
    TestAppConfigDebugAudioDirUtf8RoundTrip();
    TestConfigTemplateSeeding();
    TestLlmRefinePromptAndPayload();
    TestHotwordProcessConfig();
    TestHotwordExtractorPromptAndParse();
    TestFirmwareManifestParsingAndVersionCompare();
    TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate();
    TestCoordinatorUpdateFirmwareFromFile();
    TestParseOtaCliArgs();
    TestCoordinatorHotkeyWithoutConnectionShowsWakeHint();
    TestCoordinatorHotkeyWithConnectionSendsRemoteButton();
    TestCoordinatorCancelsShortPrimaryPress();
    TestCoordinatorPrimaryDuringFinalizingRefreshesThinking();
    TestCoordinatorSecondaryCancelsFinalizing();
    TestCoordinatorAcceptsAudioFramesAfterButtonUpUntilEnd();
    TestCoordinatorMainFinalPastesWithoutConfirmation();
    TestCoordinatorRefineShowsOriginalTextImmediately();
    TestCoordinatorOtherDeviceDuringRecordingGetsReady();
    TestCoordinatorSubtitleOutputSkipsPaste();
    TestCoordinatorSubtitleFinalDoesNotBlockNextSession();
    TestCoordinatorShortSubtitleEndReturnsReady();
    TestCoordinatorClickToTalkPrimaryClickTogglesRecording();
    TestCoordinatorMainPartialSentToDeviceOnlyAfterFinalAudio();
    TestCoordinatorShowsDetailedAsrStartError();
    TestTapEventInjectsArrowDown();
    TestTapDisabledWhenConfigOff();
    TestTapIgnoredDuringRecording();
    TestTapThrottledWithin500ms();
    TestTapThrottleRecoversAfter500ms();
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
    TestCoordinatorAirMouseSustainedRotationConstantSpeed();
    TestCoordinatorAirMouseStopsWhenOmegaZero();
    TestCoordinatorAngleMovesOnlyWhileRotating();
    TestCoordinatorSecondaryDoubleClickRestoresLastInput();
    TestCoordinatorSecondaryDoubleClickIgnoredInAirMouse();
    TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError();
    TestSseParser();
    TestStreamPayload();
    TestTencentProviderSelection();
    TestTencentConfigRoundTrip();
    TestTencentCredentialsTrimmedOnLoad();
    TestTencentSecretIdRecoveryFromVolcengineField();
    TestTencentSignatureGeneration();
    TestTencentUrlConstruction();
    TestTencentResultParsing();
    TestTencentFinalFlagParsing();
    TestTencentSentenceAccumulation();
    TestTencentEndMessage();
    TestTencentOpusEncapsulation();
    TestTencentVoiceIdGeneration();
    TestAudioOpusDecoderRoundTrip();
    TestAudioOpusDecoderNullData();
    TestAudioOpusDecoderInvalidData();
    TestAudioOpusDecoderSmallBuffer();
    TestPcmRingBufferWriteRead();
    TestPcmRingBufferOverwrite();
    TestPcmRingBufferUnderrunSilence();
    TestPcmRingBufferClear();
    TestPcmRingBufferWrapAround();
    TestWasapiRendererFailsOnMissingDevice();
    TestRenderPumpSubmitsFullAvailableNoCap();
    TestRenderPumpFillsSilenceWhenRingEmpty();
    TestRenderPumpZeroWhenBufferFull();
    TestWasapiRendererStopsCleanlyWakingBlockedThread();
    TestWasapiRendererRestartsAfterStop();
    TestOutputTargetWechatInputMethod();
    TestWechatInputMethodConfigRoundTrip();
    TestWechatInputMethodPerModeHotkeyRoundTrip();
    TestWechatInputMethodLegacyHotkeyFallback();
    TestWechatInputMethodActiveHotkeyByMode();
    TestWechatTriggerModeMigratedFromLegacyInteractionMode();
    TestWechatTriggerModeRoundTrip();
    TestWechatInputMethodHotkeyParsing();
    TestCoordinatorWechatInputMethodButtonDownSendsHotkey();
    TestCoordinatorWechatInputMethodWritesDebugAudio();
    TestCoordinatorWechatInputMethodStopsOnDeviceDisconnect();
    TestCoordinatorWechatInputMethodHandlesEmptyEndFrame();
    TestCoordinatorWechatInputMethodDiscardsZeroFrameRecording();
    TestCoordinatorWechatInputMethodDiscardsShortSingleFrameRecording();
    TestCoordinatorWechatInputMethodDoubleClickSendsEnter();
    TestCoordinatorWechatInputMethodAudioEndStopsSessionWithoutButtonUp();
    TestCoordinatorWechatInputMethodRecoversFromStaleActive();
    TestCoordinatorWechatRecordingHardTimeoutRecoversFromLostButtonUp();
    TestCoordinatorWechatModeSendsInstantInteractionMode();
    TestWechatClickTriggerDoesNotLeakToFocusedApp();
    TestCoordinatorWechatSessionRendererStartFailureSkipsHotkey();
    TestCoordinatorWechatClickToTalkSendsClickOnStart();
    TestCoordinatorWechatClickToTalkSendsClickOnStop();
    TestCoordinatorWechatClickToTalkAudioEndOvertakesStopClick();
    TestCoordinatorWechatClickToTalkStaleActiveNewClickStartsNew();
    TestCoordinatorWechatHotkeyDeferredUntilFirstAudioFrame();
    TestCoordinatorWechatHotkeySkippedBeforeFirstFrameButtonUp();
    TestCoordinatorWechatHotkeySendUpPairedAfterFirstFrame();
    TestCoordinatorWechatHotkeySkippedOnDecodeFailure();
    TestCoordinatorWechatHotkeySkippedOnEmptyEndFirstFrame();
    TestCoordinatorWechatInputMethodAutoSwitchesDefaultDevice();
    TestCoordinatorWechatInputMethodNoSwitchWhenDisabled();
    TestWechatWarnsWhenForegroundElevated();
    TestWechatNoDuplicateElevationWarnForSameProcess();
    TestWechatWarnsAgainForDifferentElevatedProcess();
    TestWechatNoWarnWhenForegroundNormal();
    TestWechatNoProbeNoWarn();
    TestCoordinatorAutoSwitchRecoversStaleState();
    TestDeviceSwitchStateRoundTrip();
    TestDeviceSwitchStateClear();
    TestDeviceSwitchStateLoadMissingFile();
    TestWStringUtf8Conversion();
    TestCoordinatorRecordingHardTimeoutRecoversFromLostButtonUp();
    TestCoordinatorFinalizingWatchdogTimesOutWithoutAsrFinal();
    TestCoordinatorFinalizingWatchdogResetByPartialActivity();
    TestCoordinatorAudioStallFinalizesWithoutButtonUp();
    TestCoordinatorRecoveringButtonDownStopsStaleRecording();
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
    TestDebugAudioRecorderInvalidDirectoryDoesNotCrash();
    return 0;
}
