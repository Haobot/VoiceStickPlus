#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "audio_opus_encoder.h"
#include "ble_protocol.h"
#include "ima_adpcm_decoder.h"
#include "pcm_postprocessor.h"
#include "xiaomi_atvv_protocol.h"
#include "xiaomi_atvv_session.h"
#include "cmd_line.h"
#include "com_port_selector.h"

#include <opus.h>
#include "byte_utils.h"
#include "cJSON.h"
#include "esptool_flash_command.h"
#include "esptool_progress.h"
#include "firmware_manifest.h"
#include "hotword_extractor.h"
#include "hotword_candidate_miner.h"
#include "hotword_selector.h"
#include "tencent_asr_vocab_client.h"
#include "key_spec.h"
#include "llm_refinement_client.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "ogg_opus_demuxer.h"
#include "onboarding_dialog.h"
#include "pair_device_helper.h"
#include "pcm_ring_buffer.h"
#include "power_log_monitor.h"
#include "voice_stick_coordinator.h"
#include "voice_stick_flash_tool.h"
#include "wasapi_render_sink.h"
#include "wasapi_virtual_mic_renderer.h"
#include "wechat_input_method_hotkey.h"
#include "default_audio_device_controller.h"
#include "device_switch_state.h"
#include "debug_audio_recorder.h"
#include "encoder_speed.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
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
                             const std::string&,
                             DeviceClass) override {}
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
    void SendEncoderLedColor(const std::string& color,
                             const std::optional<std::string>& device_id) override {
        sent_encoder_led_colors.push_back(std::pair{color, device_id});
    }
    void SendEncoderRecordingGate(bool enabled,
                                  const std::optional<std::string>& device_id) override {
        sent_encoder_recording_gates.push_back(std::pair{enabled, device_id});
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
    std::vector<std::pair<std::string, std::optional<std::string>>> sent_encoder_led_colors;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_encoder_recording_gates;
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
    void InvalidateConnection() override {
        ++invalidate_call_count;
    }

    bool start_result = true;
    std::string start_error;
    bool started = false;
    bool cancelled = false;
    int sent_chunks = 0;
    bool last_chunk_was_final = false;
    int invalidate_call_count = 0;
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
    void SetDeviceEncoderPresent(const std::string& device_id, bool present) override {
        encoder_present_by_device_id[device_id] = present;
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
    void ShowTimedMessage(const std::string& message, int duration_ms) override {
        timed_messages.push_back(message + ":" + std::to_string(duration_ms));
    }

    std::vector<std::string> statuses;
    std::vector<ConnectedDevice> connected_devices;
    std::vector<DeviceInfo> device_infos;
    std::map<std::string, bool> encoder_present_by_device_id;
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
    std::vector<std::string> timed_messages;
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
    void SendArrowUp() override { ++arrow_up_count; }
    void SendKeyCombo(const KeySpec& spec) override {
        sent_key_combos.push_back(spec.display_text);
    }
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
    int arrow_up_count = 0;
    std::vector<std::string> sent_key_combos;
    int move_mouse_count = 0;
    int total_dx = 0;
    int total_dy = 0;
    int left_click_count = 0;
};

// 测试用虚拟麦渲染器：解耦真实 WASAPI，Start 返回可配置结果。
class FakeVirtualMicRenderer : public IVirtualMicRenderer {
public:
    explicit FakeVirtualMicRenderer(bool start_result) : start_result_(start_result) {}
    bool Start(PcmRingBuffer* ring) override {
        ++start_count;
        last_ring = ring;
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
    PcmRingBuffer* last_ring = nullptr;  // 最近一次 Start 的 PCM 源（协调器持有，测试只读）
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
                       std::optional<std::uint32_t> session_id = std::nullopt,
                       std::optional<std::uint32_t> duration_ms = std::nullopt) {
    StateEvent state_event;
    state_event.event = event;
    state_event.button = button;
    state_event.session_id = session_id;
    state_event.duration_ms = duration_ms;
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

// 构造编码器旋转事件（固件上报的 {"event":"encoder_rotate","direction":"cw","steps":2}）。
StateEvent EncoderRotateEvent(const std::string& direction, std::uint32_t steps) {
    StateEvent state_event;
    state_event.event = "encoder_rotate";
    state_event.direction = direction;
    state_event.steps = steps;
    return state_event;
}

// 构造编码器按键事件（固件上报带 "source":"encoder"）。
StateEvent EncoderButtonEvent(const std::string& event,
                              std::optional<std::uint32_t> session_id = std::nullopt) {
    StateEvent state_event;
    state_event.event = event;
    state_event.button = "primary";
    state_event.session_id = session_id;
    state_event.source = "encoder";
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
    assert(ParseManualPairDeviceId("RC-3a7f").value() == "3A7F");
    assert(!ParseManualPairDeviceId("VS-123").has_value());
    assert(!ParseManualPairDeviceId("VoiceStick").has_value());

    // 异步配对消息的地址匹配：陈旧消息（上一目标迟到回调）地址不符即丢弃。
    assert(MatchesPendingPairAddress(0xAABBCCDDEEFF, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0x112233445566, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0xAABBCCDDEEFF, 0));

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

    // 回归用例：固件名称在 SCAN_RSP、ADV 只带 service UUID（见 voice_ble.c），
    // 同一物理地址会交替出现命名候选（广播名 VS-D63C）与临时候选（MAC 低位
    // VS-D63E）。同地址合并时命名候选必须优先，后到的临时包不得覆盖已确认的
    // 命名候选——否则用户看到列表是 D63C、点配对取到的却是临时候选 D63E，
    // 配对对话框命中 "waiting for name" 分支不发起连接，设备卡 Pairing。
    {
        std::vector<PairingCandidate> merged;
        PairingCandidate named;
        named.bluetooth_address = 0x70041DD5D63E;
        named.device_id = "D63C";
        named.display_name = "VS-D63C";
        named.id_source = PairingCandidateIdSource::kName;
        named.rssi = -68;
        MergePairingCandidate(&merged, named);

        PairingCandidate later_temporary = named;
        later_temporary.device_id = "D63E";
        later_temporary.display_name.clear();
        later_temporary.id_source = PairingCandidateIdSource::kAddressFallback;
        later_temporary.is_temporary_candidate = true;
        later_temporary.rssi = -66;
        MergePairingCandidate(&merged, later_temporary);

        assert(merged.size() == 1);
        assert(!merged.front().is_temporary_candidate);
        assert(merged.front().device_id == "D63C");
        assert(merged.front().display_name == "VS-D63C");

        // 反向到达顺序：先临时后命名，命名应正常替换临时。
        std::vector<PairingCandidate> merged_reverse;
        MergePairingCandidate(&merged_reverse, later_temporary);
        MergePairingCandidate(&merged_reverse, named);
        assert(merged_reverse.size() == 1);
        assert(!merged_reverse.front().is_temporary_candidate);
        assert(merged_reverse.front().device_id == "D63C");
    }
}

void TestPairingAdvertisementClassify() {
    // 名称命中 VS- 前缀 → StickS3 / kName
    auto match = ClassifyPairingAdvertisement("VS-C3D8", true, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "C3D8");
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->id_source == PairingCandidateIdSource::kName);
    assert(!match->is_temporary);

    // 名称命中 RC- 前缀 → 小米遥控器 / kName
    match = ClassifyPairingAdvertisement("RC-3A7F", false, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "3A7F");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->id_source == PairingCandidateIdSource::kName);
    assert(!match->is_temporary);

    // 小米名称白名单（中文名，无内嵌 ID）→ 地址低 16 位 / 非临时
    match = ClassifyPairingAdvertisement("小米蓝牙语音遥控器", false, false, 0xAABBCCDD3A7F);
    assert(match.has_value());
    assert(match->device_id == "3A7F");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);
    assert(!match->is_temporary);

    // 小米名称白名单（英文名）
    match = ClassifyPairingAdvertisement("Xiaomi Bluetooth Remote 2 Pro", false, false,
                                         0xAABBCCDD3A7F);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->device_id == "3A7F");

    // 白名单名 "RC001"（非 RC-XXXX 前缀模式）→ 不误中名内 ID，走地址兜底
    match = ClassifyPairingAdvertisement("RC001", false, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->device_id == "EEFF");
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);

    // 仅 ATVV service UUID（无名称）→ 小米地址兜底候选
    match = ClassifyPairingAdvertisement("", false, true, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "EEFF");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(!match->is_temporary);

    // 仅 VoiceStick service UUID（无名称）→ StickS3 临时候选
    match = ClassifyPairingAdvertisement("", true, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "EEFF");
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);
    assert(match->is_temporary);

    // VS service 与 ATVV 同时存在且无名称：VoiceStick service 优先（固件 SCAN_RSP 拆分场景）
    match = ClassifyPairingAdvertisement("", true, true, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->is_temporary);

    // 无关广告 → 不识别
    assert(!ClassifyPairingAdvertisement("", false, false, 0xAABBCCDDEEFF).has_value());
    assert(!ClassifyPairingAdvertisement("Some Headphones", false, false, 0xAABBCCDDEEFF).has_value());
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

void TestEncoderRotateStateParsing() {
    const std::string json = "{\"event\":\"encoder_rotate\",\"direction\":\"ccw\",\"steps\":3}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "encoder_rotate");
    assert(event->direction == "ccw");
    assert(event->steps.has_value());
    assert(event->steps.value() == 3);

    // 缺字段容错：direction 为空串、steps 为 nullopt，不影响整体解析。
    const std::string sparse = "{\"event\":\"encoder_rotate\"}";
    ByteVector sparse_frame = {1, 0x10};
    AppendLe16(sparse_frame, static_cast<std::uint16_t>(sparse.size()));
    sparse_frame.insert(sparse_frame.end(), sparse.begin(), sparse.end());
    auto sparse_event = BleProtocol::ParseStateEvent(sparse_frame);
    assert(sparse_event.has_value());
    assert(sparse_event->event == "encoder_rotate");
    assert(sparse_event->direction.empty());
    assert(!sparse_event->steps.has_value());
}

void TestEncoderStatusParsing() {
    // encoder_status 独立小帧：{"event":"encoder_status","present":true}。
    const std::string json = "{\"event\":\"encoder_status\",\"present\":true}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "encoder_status");
    assert(event->encoder_present.has_value());
    assert(event->encoder_present.value() == true);

    // present=false（未装编码器）。
    const std::string absent = "{\"event\":\"encoder_status\",\"present\":false}";
    ByteVector absent_frame = {1, 0x10};
    AppendLe16(absent_frame, static_cast<std::uint16_t>(absent.size()));
    absent_frame.insert(absent_frame.end(), absent.begin(), absent.end());
    auto absent_event = BleProtocol::ParseStateEvent(absent_frame);
    assert(absent_event.has_value());
    assert(absent_event->encoder_present.has_value());
    assert(absent_event->encoder_present.value() == false);

    // 缺 present 字段容错：解析为 nullopt，不影响整体解析。
    const std::string sparse = "{\"event\":\"encoder_status\"}";
    ByteVector sparse_frame = {1, 0x10};
    AppendLe16(sparse_frame, static_cast<std::uint16_t>(sparse.size()));
    sparse_frame.insert(sparse_frame.end(), sparse.begin(), sparse.end());
    auto sparse_event = BleProtocol::ParseStateEvent(sparse_frame);
    assert(sparse_event.has_value());
    assert(!sparse_event->encoder_present.has_value());

    // 其它事件不携带该字段（老固件无 encoder_status 事件，消费端按「在线」处理）。
    const std::string legacy =
        "{\"event\":\"device_info\",\"hardware\":\"stick_s3\",\"firmware_version\":\"2.2.0\"}";
    ByteVector legacy_frame = {1, 0x10};
    AppendLe16(legacy_frame, static_cast<std::uint16_t>(legacy.size()));
    legacy_frame.insert(legacy_frame.end(), legacy.begin(), legacy.end());
    auto legacy_event = BleProtocol::ParseStateEvent(legacy_frame);
    assert(legacy_event.has_value());
    assert(!legacy_event->encoder_present.has_value());

    // DeviceInfo 默认 encoder_present=true（未收到 encoder_status 时保持设置可见）。
    DeviceInfo default_info;
    assert(default_info.encoder_present);
}

void TestStateEventSourceParsing() {
    // 编码器按键事件带 source 字段。
    const std::string json = "{\"event\":\"button_click\",\"button\":\"primary\",\"duration_ms\":131,\"source\":\"encoder\"}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "button_click");
    assert(event->source == "encoder");

    // 物理键事件不带 source：解析为空串（缺省=物理键）。
    const std::string plain = "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":42}";
    ByteVector plain_frame = {1, 0x10};
    AppendLe16(plain_frame, static_cast<std::uint16_t>(plain.size()));
    plain_frame.insert(plain_frame.end(), plain.begin(), plain.end());
    auto plain_event = BleProtocol::ParseStateEvent(plain_frame);
    assert(plain_event.has_value());
    assert(plain_event->source.empty());
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

void TestAsrHotwordCorpusBudget() {
    // token 估算：CJK 每字 1，ASCII 每 3 字符 1。
    assert(AsrProtocol::EstimateHotwordTokens("") == 0);
    assert(AsrProtocol::EstimateHotwordTokens("小智") == 2);
    assert(AsrProtocol::EstimateHotwordTokens("AGENTS.md") == 3);
    assert(AsrProtocol::EstimateHotwordTokens("VoiceStick") == 4);
    assert(AsrProtocol::EstimateHotwordTokens("Expe 记忆") == 4);
    assert(AsrProtocol::EstimateHotwordTokens("a") == 1);

    // 单词超 kHotwordMaxWordTokens 的被丢弃，其余保持顺序。
    const std::string too_long(40, 'x');  // ceil(40/3)=14 tokens
    auto fitted = AsrProtocol::FitHotwordsToCorpusBudget({"小智", too_long, "VoiceStick"});
    assert((fitted == std::vector<std::string>{"小智", "VoiceStick"}));

    // 累计超预算的词被丢弃，预算内的保留且顺序不变。
    std::vector<std::string> many;
    for (int i = 0; i < 30; ++i) many.push_back("热词编号" + std::to_string(i));  // 每个 4+1~2 tokens
    auto trimmed = AsrProtocol::FitHotwordsToCorpusBudget(many);
    assert(trimmed.size() < many.size());
    int used = 0;
    for (const auto& word : trimmed) used += AsrProtocol::EstimateHotwordTokens(word);
    assert(used <= AsrProtocol::kHotwordCorpusTokenBudget);
    for (std::size_t i = 0; i < trimmed.size(); ++i) assert(trimmed[i] == many[i]);

    // payload 集成：超预算的词不进入 corpus，预算内的保留。
    AppConfig config = AppConfig::Defaults();
    config.asr_hotwords = {"AGENTS.md", too_long, "CLAUDE.md"};
    const std::string session_id = "budget-session";
    auto frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const std::size_t payload_size_offset = 12 + session_id.size();
    const auto payload_size = ReadBe32(std::span(frame.data() + payload_size_offset, 4));
    const std::string payload(reinterpret_cast<const char*>(frame.data() + payload_size_offset + 4),
                              payload_size);
    assert(payload.find("\\\"word\\\":\\\"AGENTS.md\\\"") != std::string::npos);
    assert(payload.find("\\\"word\\\":\\\"CLAUDE.md\\\"") != std::string::npos);
    assert(payload.find(too_long) == std::string::npos);

    // 全部超预算时不产出 corpus 字段。
    config.asr_hotwords = {too_long};
    auto empty_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto empty_size = ReadBe32(std::span(empty_frame.data() + payload_size_offset, 4));
    const std::string empty_payload(
        reinterpret_cast<const char*>(empty_frame.data() + payload_size_offset + 4), empty_size);
    assert(empty_payload.find("\"corpus\"") == std::string::npos);

    // 自学习平台词表 ID 进入 corpus，与热词 context 共存。
    config.volcengine_boosting_table_id = "boost-123";
    config.volcengine_correct_table_id = "correct-456";
    config.asr_hotwords = {"AGENTS.md"};
    auto table_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto table_size = ReadBe32(std::span(table_frame.data() + payload_size_offset, 4));
    const std::string table_payload(
        reinterpret_cast<const char*>(table_frame.data() + payload_size_offset + 4), table_size);
    assert(table_payload.find("\"boosting_table_id\":\"boost-123\"") != std::string::npos);
    assert(table_payload.find("\"correct_table_id\":\"correct-456\"") != std::string::npos);
    assert(table_payload.find("\\\"word\\\":\\\"AGENTS.md\\\"") != std::string::npos);

    // 仅词表无热词时也有 corpus，且无 context 字段；ID 为空则不出现字段。
    config.asr_hotwords = {};
    config.volcengine_correct_table_id = "";
    auto table_only_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto table_only_size = ReadBe32(std::span(table_only_frame.data() + payload_size_offset, 4));
    const std::string table_only_payload(
        reinterpret_cast<const char*>(table_only_frame.data() + payload_size_offset + 4),
        table_only_size);
    assert(table_only_payload.find("\"boosting_table_id\":\"boost-123\"") != std::string::npos);
    assert(table_only_payload.find("\"correct_table_id\"") == std::string::npos);
    assert(table_only_payload.find("\"context\"") == std::string::npos);
}

void TestTencentHotwordCharFilter() {
    // 腾讯词表 API 拒绝含 '.' 等字符的词（InvalidWordWeight），同步前必须过滤。
    assert(TencentAsrVocabClient::IsValidHotwordChars("Opus"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("覃海洋"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("ESP32-S3"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("VB-CABLE"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("win_sparkle"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("CLAUDE.md"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("AGENTS.md"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("带空格 的词"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars(""));
}

void TestVolcengineTableIdConfigRoundTrip() {
    assert(AppConfig::Defaults().volcengine_boosting_table_id.empty());
    assert(AppConfig::Defaults().volcengine_correct_table_id.empty());

    auto temp = std::filesystem::temp_directory_path() / "voicestick_volc_table_id_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.volcengine_boosting_table_id = "  boost-123  ";
    config.volcengine_correct_table_id = "correct-456";
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.volcengine_boosting_table_id == "boost-123");
    assert(loaded.volcengine_correct_table_id == "correct-456");

    std::filesystem::remove(temp);
}


void TestAppConfig() {
    AppConfig cloud = AppConfig::Defaults();
    assert(cloud.asr_provider == AsrProvider::kVolcengine);
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
    assert(AppConfig::Defaults().launch_at_login);
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

    // 热词非空时追加热词替换指引（二遍 ASR 不吃 corpus 热词，由 LLM 精修兜底）；
    // 默认 prompt 与用户 override 都追加；空热词不追加。
    const auto refine_with_hotwords =
        LLMRefinementClient::BuildRefinePrompt("", {"AGENTS.md", "CLAUDE.md"});
    assert(refine_with_hotwords.find("热词表") != std::string::npos);
    assert(refine_with_hotwords.find("AGENTS.md") != std::string::npos);
    assert(refine_with_hotwords.find("CLAUDE.md") != std::string::npos);
    assert(refine_with_hotwords.find("语音停顿") != std::string::npos);
    const auto custom_with_hotwords =
        LLMRefinementClient::BuildRefinePrompt("my custom prompt", {"AGENTS.md"});
    assert(custom_with_hotwords.starts_with("my custom prompt"));
    assert(custom_with_hotwords.find("AGENTS.md") != std::string::npos);
    assert(LLMRefinementClient::BuildRefinePrompt("my custom prompt", {}) == "my custom prompt");

    // few-shot 示例与「原文已正确的热词不得改写」约束在热词段中。
    assert(refine_with_hotwords.find("示例") != std::string::npos);
    assert(refine_with_hotwords.find("原样保留") != std::string::npos);

    // 精修守卫：原文已出现的热词在精修结果中必须保留。
    assert(LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 AGENTS.md 文件", "编辑 AGENTS.md 这个文件。", {"AGENTS.md"}));
    assert(!LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 AGENTS.md 文件", "编辑 CLDE.md 这个文件。", {"AGENTS.md"}));
    // 原文中没有的热词不受约束（精修纠错场景）；空热词表恒真。
    assert(LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 agentsdmd 文件", "编辑 AGENTS.md 文件。", {"AGENTS.md"}));
    assert(LLMRefinementClient::RefineResultKeepsHotwords("a", "b", {}));
    assert(LLMRefinementClient::RefineResultKeepsHotwords("a", "b", {""}));

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

    // disable_thinking=true 时注入两种风格的关思考参数，且 payload 仍为合法 JSON。
    const auto no_think_payload =
        LLMChatClient::BuildChatPayload("gpt-x", "sys-prompt", "hello world",
                                         /*stream=*/false, /*disable_thinking=*/true);
    assert(no_think_payload.find("\"enable_thinking\":false") != std::string::npos);
    assert(no_think_payload.find("\"chat_template_kwargs\":{\"enable_thinking\":false}") !=
           std::string::npos);
    auto* no_think_root = cJSON_Parse(no_think_payload.c_str());
    assert(no_think_root != nullptr);
    auto* enable_thinking = cJSON_GetObjectItemCaseSensitive(no_think_root, "enable_thinking");
    assert(cJSON_IsBool(enable_thinking) && !cJSON_IsTrue(enable_thinking));
    cJSON_Delete(no_think_root);

    // 精修默认关闭、prompt 默认空。llm_disable_thinking 默认开启（关闭模型深度思考）。
    assert(AppConfig::Defaults().refine_enabled == false);
    assert(AppConfig::Defaults().refine_prompt.empty());
    assert(AppConfig::Defaults().llm_disable_thinking == true);

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

void TestHotwordCandidateMiner() {
    // 标识符提取：字母开头 + 含大写/数字/._-；普通小写词、版本号、CJK 不算；
    // 尾部标点剥离。
    const auto tokens = ExtractIdentifierTokens(
        "编辑 AGENTS.md 和 hello world 文件，version 2.0 发布了 RamDisk、build_win.bat。");
    assert((tokens == std::vector<std::string>{"AGENTS.md", "RamDisk", "build_win.bat"}));
    assert(ExtractIdentifierTokens("plain english words only").empty());
    assert((ExtractIdentifierTokens("kAutoHideTimerId.") == std::vector<std::string>{"kAutoHideTimerId"}));

    // 挖掘：refined 新增且不在热词表的标识符。
    const auto mined = MineRefinementCandidates(
        "测试 agentsdmd 和 cloud dmd 文件", "测试 AGENTS.md 和 CLAUDE.md 文件。", {});
    assert((mined == std::vector<std::string>{"AGENTS.md", "CLAUDE.md"}));
    assert(MineRefinementCandidates("测试 agentsdmd 文件", "测试 AGENTS.md 文件。",
                                    {"AGENTS.md"}).empty());
    // 原文已有的词不重复挖掘。
    assert(MineRefinementCandidates("测试 RamDisk 文件", "测试 RamDisk 文件。", {}).empty());

    // 计数到阈值才建议；忽略词不建议；已通知词不重复建议。
    HotwordCandidateStore store;
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    auto suggested = RecordHotwordCandidates(store, {"AGENTS.md", "CLAUDE.md"});
    assert((suggested == std::vector<std::string>{"AGENTS.md"}));
    assert(store.counts["AGENTS.md"] == 3);
    assert(store.counts["CLAUDE.md"] == 1);
    store.notified.insert("AGENTS.md");
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    store.dismissed.insert("CLAUDE.md");
    assert(RecordHotwordCandidates(store, {"CLAUDE.md", "CLAUDE.md"}).empty());

    // 待确认列表：达阈值且未忽略。
    assert((PendingHotwordSuggestions(store) == std::vector<std::string>{"AGENTS.md"}));
    store.dismissed.insert("AGENTS.md");
    assert(PendingHotwordSuggestions(store).empty());

    // JSON 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_candidates_test.json";
    std::filesystem::remove(temp);
    HotwordCandidateStore round_trip;
    round_trip.counts["AGENTS.md"] = 5;
    round_trip.dismissed.insert("cloud dmd");
    round_trip.notified.insert("VoiceStick");
    SaveHotwordCandidates(temp, round_trip);
    const auto loaded = LoadHotwordCandidates(temp);
    assert(loaded.counts.at("AGENTS.md") == 5);
    assert(loaded.dismissed.contains("cloud dmd"));
    assert(loaded.notified.contains("VoiceStick"));
    std::filesystem::remove(temp);

    // 缺失文件返回空 store。
    assert(LoadHotwordCandidates(temp).counts.empty());
}

void TestHotwordSelector() {
    const std::int64_t now = 1760000000;  // 固定 now，保证确定性

    // 评分：同 count 下手动词（source=manual）胜过挖掘词。
    HotwordUsage manual{};
    manual.count = 3;
    manual.last_used_ts = now;
    manual.source = "manual";
    HotwordUsage mined{};
    mined.count = 3;
    mined.last_used_ts = now;
    assert(HotwordScore(manual, now) > HotwordScore(mined, now));
    // 高频词（count=30）能胜过低计数手动词——频率优先的设计意图。
    HotwordUsage high{};
    high.count = 30;
    high.last_used_ts = now;
    HotwordUsage low_manual{};
    low_manual.count = 0;
    low_manual.last_used_ts = now;
    low_manual.source = "manual";
    assert(HotwordScore(high, now) > HotwordScore(low_manual, now));
    // 未知时间戳（0）新近度记 0：只剩 count 与 manual 项。
    HotwordUsage unknown_ts{};
    unknown_ts.count = 0;
    unknown_ts.source = "manual";
    assert(HotwordScore(unknown_ts, now) == kHotwordWManual);

    // 合法性过滤：含空白 / 超长词被过滤。
    assert(IsValidHotword("Opus"));
    assert(IsValidHotword("覃海洋"));
    assert(IsValidHotword("ESP32-S3"));
    assert(!IsValidHotword(""));
    assert(!IsValidHotword("带空格 的词"));
    assert(!IsValidHotword(std::string(11, '热')));  // 11 个汉字超限
    assert(!IsValidHotword(std::string(31, 'a')));   // 31 个 ASCII 超限

    // 排序：评分降序；同分按字典序；库外词按 manual 处理。
    HotwordUsageStore store;
    HotwordUsage opus{};
    opus.count = 3;
    opus.last_used_ts = now;
    store["Opus"] = opus;
    HotwordUsage vs{};
    vs.count = 3;
    vs.last_used_ts = now;
    vs.source = "manual";
    store["VoiceStick"] = vs;
    const auto ranked = RankHotwords(store, {"Opus", "VoiceStick", "带空格 的词"}, now);
    assert((ranked == std::vector<std::string>{"VoiceStick", "Opus"}));
    // 库外词（CLAUDE.md）按 manual（score=2.0），高于同 count 的 mined 词。
    HotwordUsage mined3{};
    mined3.count = 3;
    mined3.last_used_ts = now;
    store["BLE"] = mined3;
    const auto ranked2 = RankHotwords(store, {"BLE", "CLAUDE.md"}, now);
    assert((ranked2 == std::vector<std::string>{"CLAUDE.md", "BLE"}));
    // 同分（count/时间戳/source 全同）按字典序稳定。
    HotwordUsageStore equal_store;
    HotwordUsage same{};
    same.count = 1;
    same.last_used_ts = now;
    equal_store["zeta"] = same;
    equal_store["alpha"] = same;
    const auto ranked3 = RankHotwords(equal_store, {"zeta", "alpha"}, now);
    assert((ranked3 == std::vector<std::string>{"alpha", "zeta"}));

    // 预算裁剪：按评分序装入，高分词在前且累计不超预算。
    std::vector<std::string> many;
    HotwordUsageStore big_store;
    for (int i = 0; i < 100; ++i) {
        const std::string word = "术语" + std::to_string(i);  // 每词 2 tokens
        many.push_back(word);
        HotwordUsage u{};
        u.count = 100 - i;  // 编号小的词频更高
        u.last_used_ts = now;
        big_store[word] = u;
    }
    const auto ranked_many = RankHotwords(big_store, many, now);
    const auto fitted = AsrProtocol::FitHotwordsToCorpusBudget(ranked_many);
    int used = 0;
    for (const auto& word : fitted) used += AsrProtocol::EstimateHotwordTokens(word);
    assert(used <= AsrProtocol::kHotwordCorpusTokenBudget);
    assert(fitted.size() < ranked_many.size());
    assert(ranked_many.front() == "术语0");
    assert(fitted.front() == "术语0");

    // prompt 段上限：top-N 且保持评分序。
    const auto prompt_words = TrimHotwordsForPrompt(big_store, many, kHotwordPromptMaxWords, now);
    assert(static_cast<int>(prompt_words.size()) == kHotwordPromptMaxWords);
    assert(prompt_words.front() == "术语0");

    // 使用统计记录：命中热词计数+刷新时间戳；未命中的不产生条目。
    HotwordUsageStore usage_store;
    RecordHotwordUsageInText(usage_store, "编辑 AGENTS.md 和 CLAUDE.md 文件", {"AGENTS.md", "BLE"}, now);
    assert(usage_store.at("AGENTS.md").count == 1);
    assert(usage_store.at("AGENTS.md").last_used_ts == now);
    assert(usage_store.at("AGENTS.md").source == "manual");
    assert(!usage_store.contains("BLE"));
    // 大小写不敏感。
    RecordHotwordUsageInText(usage_store, "编辑 agents.md 文件", {"AGENTS.md"}, now + 10);
    assert(usage_store.at("AGENTS.md").count == 2);
    assert(usage_store.at("AGENTS.md").last_used_ts == now + 10);

    // JSON 往返（与 hotword_select.py load_stats_json 列表形态兼容）。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_usage_test.json";
    std::filesystem::remove(temp);
    SaveHotwordUsage(temp, usage_store);
    const auto loaded = LoadHotwordUsage(temp);
    assert(loaded.at("AGENTS.md").count == 2);
    assert(loaded.at("AGENTS.md").last_used_ts == now + 10);
    assert(loaded.at("AGENTS.md").source == "manual");
    std::filesystem::remove(temp);
    // 缺失/损坏文件返回空 store。
    assert(LoadHotwordUsage(temp).empty());
    auto bad_temp = std::filesystem::temp_directory_path() / "voicestick_hotword_usage_bad.json";
    std::filesystem::remove(bad_temp);
    {
        std::ofstream f(bad_temp, std::ios::binary);
        f << "{not-json";
    }
    assert(LoadHotwordUsage(bad_temp).empty());
    std::filesystem::remove(bad_temp);
}

void TestHotwordExtractionPromptAndParse() {
    // prompt 附已知热词表（"已知热词表："列表行）；空表时不附加（注意基础规则文本里
    // 含「已知热词表中的条目」字样，断言要匹配带冒号的列表行）。
    const auto prompt = LLMRefinementClient::BuildHotwordExtractionPrompt({"AGENTS.md", "DeepSeek"});
    assert(prompt.find("AGENTS.md") != std::string::npos);
    assert(prompt.find("JSON") != std::string::npos);
    assert(prompt.find("已知热词表：") != std::string::npos);
    assert(LLMRefinementClient::BuildHotwordExtractionPrompt({}).find("已知热词表：") == std::string::npos);

    const std::string source = "我们研究一下 Stack Chain 是怎么使用的，顺便问问 DeepSeek。";
    const std::vector<std::string> hotwords = {"DeepSeek"};

    // 正常 JSON 数组：过滤已在热词表的 DeepSeek，保留 Stack Chain。
    auto words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "[\"Stack Chain\", \"DeepSeek\"]", source, hotwords);
    assert((words == std::vector<std::string>{"Stack Chain"}));

    // 前后裹解释文字也能容错解析。
    words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "候选如下：[\"Stack Chain\"] 以上。", source, hotwords);
    assert((words == std::vector<std::string>{"Stack Chain"}));

    // 无候选 / 垃圾输出 / 非字符串元素 → 空。
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[]", source, hotwords).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("没有候选", source, hotwords).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[1, true, null]", source, hotwords).empty());

    // 防臆造：不在原文出现的词被丢弃。
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"OpenAI\"]", source, {}).empty());

    // 忽略大小写：原文中的 "Stack Chain" 能匹配候选 "stack chain"；
    // 与热词仅大小写不同也算重复（"deepseek" 命中热词 "DeepSeek"）。
    words = LLMRefinementClient::ParseHotwordExtractionResponse("[\"stack chain\"]", source, hotwords);
    assert((words == std::vector<std::string>{"stack chain"}));
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"deepseek\"]", source, hotwords).empty());

    // 过短 / 超长 / 超 3 词被过滤；同词大小写去重只留第一个。
    const std::string words_source = "alpha beta gamma delta";
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"x\"]", words_source, {}).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse(
               "[\"alpha beta gamma delta\"]", words_source, {}).empty());
    assert((LLMRefinementClient::ParseHotwordExtractionResponse("[\"alpha beta\"]", words_source, {})
            == std::vector<std::string>{"alpha beta"}));
    const std::string long_source = "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz";
    assert(LLMRefinementClient::ParseHotwordExtractionResponse(
               "[\"abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz\"]", long_source, {}).empty());
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\", \"stack chain\"]", source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));

    // 空白容忍：ASR 英文空格形态不稳定（双空格 / 连写），LLM 规范化输出不应被
    // 防臆造误杀（生产环境 candidates=0 的已证实根因之一）。
    const std::string double_space_source = "我刚才讲了 Stack  Chain 这个新词";
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\"]", double_space_source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));
    const std::string concat_source = "我刚才讲了 StackChain 这个新词";
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\"]", concat_source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));

    // 统计回填：bracket/json/items/各拒绝原因计数（只计数不记文本）。
    LLMRefinementClient::HotwordExtractionStats stats;
    words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "[\"Stack Chain\", \"DeepSeek\", \"OpenAI\", \"x\"]", source, hotwords, &stats);
    assert((words == std::vector<std::string>{"Stack Chain"}));
    assert(stats.bracket_found && stats.json_ok && stats.items == 4);
    assert(stats.rejected_hotword == 1 && stats.rejected_not_in_text == 1 &&
           stats.rejected_len == 1);
    stats = {};
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("没有候选", source, hotwords, &stats)
               .empty());
    assert(!stats.bracket_found && !stats.json_ok);
    stats = {};
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[not json]", source, hotwords, &stats)
               .empty());
    assert(stats.bracket_found && !stats.json_ok);

    // hotword_mining_enabled 配置往返，默认关闭。
    assert(!AppConfig::Defaults().hotword_mining_enabled);
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_mining_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.hotword_mining_enabled = true;
    config.Save(temp);
    assert(AppConfig::Load(temp).hotword_mining_enabled);
    std::filesystem::remove(temp);
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
    config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_imu_wake_sensitivities.empty());
    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 250);
    // 设备交互设置按设备单播（IMU 唤醒灵敏度现属于设备级 InteractionSettings）。
    assert(ble_ptr->sent_imu_wake_sensitivities.back().device_id == "5A74");

    AppConfig updated = config;
    updated.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 500);
    assert(ble_ptr->sent_imu_wake_sensitivities.back().device_id == "5A74");
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
    config.default_interaction_settings.tap_sensitivity = 7;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_tap_sensitivities.empty());
    assert(ble_ptr->sent_tap_sensitivities.back().level == 7);
    // 设备交互设置按设备单播（敲击灵敏度现属于设备级 InteractionSettings）。
    assert(ble_ptr->sent_tap_sensitivities.back().device_id == "5A74");

    AppConfig updated = config;
    updated.default_interaction_settings.tap_sensitivity = 3;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_tap_sensitivities.back().level == 3);
    assert(ble_ptr->sent_tap_sensitivities.back().device_id == "5A74");
}

void TestBleEncoderPayloads() {
    auto led = BleProtocol::EncoderLedColorPayload("cyan");
    assert(std::string(led.begin(), led.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"cyan\"}");
    auto led_off = BleProtocol::EncoderLedColorPayload("off");
    assert(std::string(led_off.begin(), led_off.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"off\"}");

    auto gate_off = BleProtocol::EncoderRecordingGatePayload(false);
    assert(std::string(gate_off.begin(), gate_off.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":false}");
    auto gate_on = BleProtocol::EncoderRecordingGatePayload(true);
    assert(std::string(gate_on.begin(), gate_on.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":true}");
}

void TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.led_color = "purple";
    config.default_encoder_settings.press_action = "key";  // 派生门控关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 连接时按设备单播有效配置：无覆盖设备收到全局默认值。
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    assert(ble_ptr->sent_encoder_led_colors.size() == 1);
    assert(ble_ptr->sent_encoder_led_colors.back().first == "purple");
    assert(ble_ptr->sent_encoder_led_colors.back().second.has_value());
    assert(*ble_ptr->sent_encoder_led_colors.back().second == "5A74");
    assert(ble_ptr->sent_encoder_recording_gates.size() == 1);
    assert(ble_ptr->sent_encoder_recording_gates.back().first == false);
    assert(ble_ptr->sent_encoder_recording_gates.back().second.has_value());
    assert(*ble_ptr->sent_encoder_recording_gates.back().second == "5A74");

    // UpdateConfig 对已连接设备逐台单播；press_action=recording 派生门控打开。
    AppConfig updated = AppConfig::Defaults();
    updated.default_encoder_settings.led_color = "green";
    updated.default_encoder_settings.press_action = "recording";
    coordinator.UpdateConfig(updated);
    assert(ble_ptr->sent_encoder_led_colors.size() == 2);
    assert(ble_ptr->sent_encoder_led_colors.back().first == "green");
    assert(ble_ptr->sent_encoder_led_colors.back().second.has_value());
    assert(*ble_ptr->sent_encoder_led_colors.back().second == "5A74");
    assert(ble_ptr->sent_encoder_recording_gates.size() == 2);
    assert(ble_ptr->sent_encoder_recording_gates.back().first == true);
    assert(ble_ptr->sent_encoder_recording_gates.back().second.has_value());
    assert(*ble_ptr->sent_encoder_recording_gates.back().second == "5A74");
}

void TestCoordinatorSyncsEncoderSettingsPerDeviceOverride() {
    // 按设备覆盖：连接后每台设备收到各自的 led_color / recording_gate。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.led_color = "red";
    config.default_encoder_settings.press_action = "recording";
    config.paired_device_ids = {"5A74", "9BC1"};
    EncoderSettings override_settings;
    override_settings.led_color = "blue";
    override_settings.press_action = "key";
    config.device_encoder_settings["9BC1"] = override_settings;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->connected_device_ids.insert("9BC1");
    ble_ptr->on_connection_change(
        {ConnectedDevice{"5A74", "VS-5A74"}, ConnectedDevice{"9BC1", "VS-9BC1"}});
    assert(ble_ptr->sent_encoder_led_colors.size() == 2);
    assert(ble_ptr->sent_encoder_recording_gates.size() == 2);
    // 5A74 无覆盖 → 全局默认 red + 门控开；9BC1 覆盖 → blue + 门控关。
    for (const auto& [color, target] : ble_ptr->sent_encoder_led_colors) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(color == "red");
        } else {
            assert(*target == "9BC1");
            assert(color == "blue");
        }
    }
    for (const auto& [gate, target] : ble_ptr->sent_encoder_recording_gates) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(gate == true);
        } else {
            assert(*target == "9BC1");
            assert(gate == false);
        }
    }
}

void TestCoordinatorSyncsInteractionSettingsPerDeviceOverride() {
    // 设备交互设置按设备覆盖：连接后每台设备收到各自的有效配置（tap_to_arrow / 灵敏度 / IMU 唤醒）。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    config.default_interaction_settings.tap_sensitivity = 7;
    config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    config.paired_device_ids = {"5A74", "9BC1"};
    InteractionSettings override;
    override.tap_to_arrow = false;
    override.tap_sensitivity = 3;
    override.imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    config.device_interaction_settings["9BC1"] = override;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->connected_device_ids.insert("9BC1");
    ble_ptr->on_connection_change(
        {ConnectedDevice{"5A74", "VS-5A74"}, ConnectedDevice{"9BC1", "VS-9BC1"}});

    // tap_to_arrow：5A74 默认 true，9BC1 覆盖 false，均按设备单播。
    assert(ble_ptr->sent_tap_enabled.size() == 2);
    for (const auto& [enabled, target] : ble_ptr->sent_tap_enabled) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(enabled == true);
        } else {
            assert(*target == "9BC1");
            assert(enabled == false);
        }
    }

    // tap_sensitivity：5A74=7，9BC1=3。
    assert(ble_ptr->sent_tap_sensitivities.size() == 2);
    for (const auto& sent : ble_ptr->sent_tap_sensitivities) {
        assert(sent.device_id.has_value());
        if (*sent.device_id == "5A74") {
            assert(sent.level == 7);
        } else {
            assert(*sent.device_id == "9BC1");
            assert(sent.level == 3);
        }
    }

    // imu_wake_sensitivity：5A74=kHigh(250)，9BC1 覆盖 kLow(800)。
    assert(ble_ptr->sent_imu_wake_sensitivities.size() == 2);
    for (const auto& sent : ble_ptr->sent_imu_wake_sensitivities) {
        assert(sent.device_id.has_value());
        if (*sent.device_id == "5A74") {
            assert(sent.threshold_lsb == 250);
        } else {
            assert(*sent.device_id == "9BC1");
            assert(sent.threshold_lsb == 800);
        }
    }
}

void TestAppConfigTapSensitivityRoundTrip() {
    // 默认档 5。
    assert(AppConfig::Defaults().default_interaction_settings.tap_sensitivity == 5);

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
        config.default_interaction_settings.tap_sensitivity = 8;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.default_interaction_settings.tap_sensitivity == 8);

        std::filesystem::remove(temp);
    }

    // 越界值落盘后回读应被钳位到默认档 5。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_tap_sensitivity_clamp_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.default_interaction_settings.tap_sensitivity = 99;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.default_interaction_settings.tap_sensitivity == 5);

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

// ---- 小米蓝牙遥控器 2 Pro（ATVV）core 纯逻辑层 ----
// 协议规范见 Doc/Plan/xiaomi-remote-2-pro-support.md §3；按键语义镜像固件双击设计。

// 会话动作列表查找/收集辅助。
const XiaomiAtvvWriteTx* FindAtvvWriteTx(const std::vector<XiaomiAtvvAction>& actions) {
    for (const auto& action : actions) {
        if (const auto* tx = std::get_if<XiaomiAtvvWriteTx>(&action)) return tx;
    }
    return nullptr;
}

const StateEvent* FindAtvvEvent(const std::vector<XiaomiAtvvAction>& actions, std::string_view name) {
    for (const auto& action : actions) {
        if (const auto* event = std::get_if<XiaomiAtvvStateEvent>(&action)) {
            if (event->event.event == name) return &event->event;
        }
    }
    return nullptr;
}

std::vector<AudioFrame> CollectAtvvFrames(const std::vector<XiaomiAtvvAction>& actions) {
    std::vector<AudioFrame> frames;
    for (const auto& action : actions) {
        if (const auto* frame = std::get_if<XiaomiAtvvAudioFrame>(&action)) {
            frames.push_back(frame->frame);
        }
    }
    return frames;
}

bool HasAtvvError(const std::vector<XiaomiAtvvAction>& actions, std::string_view code) {
    for (const auto& action : actions) {
        if (const auto* error = std::get_if<XiaomiAtvvError>(&action)) {
            if (error->code == code) return true;
        }
    }
    return false;
}

// 快速完成握手进入 Ready（Start + v1.0 CAPS：16kHz、帧长 120）。
void AtvvHandshakeReady(XiaomiAtvvSession& session, std::int64_t now_ms) {
    session.Start(now_ms);
    session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, now_ms + 10);
}

void TestXiaomiAtvvCapsParsing() {
    // TX 命令构造。
    assert(XiaomiAtvvProtocol::GetCapsCommand() == (ByteVector{0x0A, 0x01, 0x00, 0x00, 0x03, 0x03}));
    assert(XiaomiAtvvProtocol::MicOpenAckCommand(false) == (ByteVector{0x0C, 0x00}));
    assert(XiaomiAtvvProtocol::MicOpenAckCommand(true) == (ByteVector{0x0C, 0x00, 0x02}));
    assert(XiaomiAtvvProtocol::MicCloseCommand(false, 0x07) == (ByteVector{0x0D, 0x07}));
    assert(XiaomiAtvvProtocol::MicCloseCommand(true, 0x07) == (ByteVector{0x0D}));

    // v1.0 标准布局：16kHz、interaction=3、协商帧长 120。
    auto caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78});
    assert(caps.has_value());
    assert(caps->IsV1OrLater());
    assert(caps->codecs == 0x02 && caps->Supports16kHz());
    assert(caps->interaction == 0x03);
    assert(caps->frame_bytes == 120);

    // 协商帧长 0 → 默认 120；自定义帧长 256；无帧长字段（len 5）→ 默认 120。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00});
    assert(caps && caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x01, 0x00});
    assert(caps && caps->frame_bytes == 256);
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03});
    assert(caps && caps->frame_bytes == 120);

    // 兼容分支：报 v1 但 codecs==0，旧版双字节 codec 布局（[4]=0x02 且 len≥9）；
    // 旧版布局未定义帧长字段，[5:7] 的垃圾值（0x0100=256）不得被采用，保持默认 120。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00});
    assert(caps && caps->codecs == 0x02 && caps->interaction == 0x03 && caps->Supports16kHz());
    assert(caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);

    // 旧版 v0 布局：len≥9，codecs=[4]，interaction=0，帧长默认。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00});
    assert(caps && !caps->IsV1OrLater());
    assert(caps->codecs == 0x02 && caps->interaction == 0x00);
    assert(caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);

    // 8kHz-only：解析成功但不支持 16kHz（由会话层判 Error）。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78});
    assert(caps && !caps->Supports16kHz());
    // v1 codecs==0 且不满足兼容分支（[4]&0x03==0）→ 无可用 codec。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x00, 0x00, 0x00, 0x78});
    assert(caps && caps->codecs == 0x00 && !caps->Supports16kHz());

    // 拒绝：空包/短包/错 opcode/v1 缺 interaction 字段/旧版短包。
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0C, 0x01, 0x00, 0x02, 0x03}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x00, 0x01, 0x00, 0x02}).has_value());
}

// 测试本地 IMA 编码器（公开标准算法的独立实现）：输出编码字节与编码器内部
// predictor 轨迹（即标准解码的期望输出），用于与解码器逐样本对拍。
struct ImaGoldenVector {
    ByteVector encoded;
    std::vector<std::int16_t> expected_decoded;
};

ImaGoldenVector ImaEncodeForTest(const std::vector<std::int16_t>& pcm) {
    static const int kStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767,
    };
    static const int kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    ImaGoldenVector out;
    int predictor = 0;
    int index = 0;
    std::optional<std::uint8_t> high_nibble;  // 高半字节优先
    for (const std::int16_t sample : pcm) {
        const int step = kStepTable[index];
        int nibble = 0;
        int diff = step >> 3;
        int delta = sample - predictor;
        if (delta < 0) {
            nibble = 8;
            delta = -delta;
        }
        if (delta >= step) {
            nibble |= 4;
            delta -= step;
            diff += step;
        }
        if (delta >= (step >> 1)) {
            nibble |= 2;
            delta -= step >> 1;
            diff += step >> 1;
        }
        if (delta >= (step >> 2)) {
            nibble |= 1;
            diff += step >> 2;
        }
        predictor = (nibble & 8) ? predictor - diff : predictor + diff;
        predictor = std::clamp(predictor, -32768, 32767);
        index = std::clamp(index + kIndexTable[nibble & 7], 0, 88);
        out.expected_decoded.push_back(static_cast<std::int16_t>(predictor));
        if (high_nibble.has_value()) {
            out.encoded.push_back(static_cast<std::uint8_t>((*high_nibble << 4) | nibble));
            high_nibble.reset();
        } else {
            high_nibble = static_cast<std::uint8_t>(nibble);
        }
    }
    // 奇数样本需补低半字节，会多解一个样本；测试只用偶数样本输入，此处不处理。
    assert(!high_nibble.has_value());
    return out;
}

void TestImaAdpcmDecoderGolden() {
    ImaAdpcmDecoder decoder;
    // 标准向量：reset(0,0) 后 0x11 → [1,2]。
    decoder.Reset(0, 0);
    auto out = decoder.Decode(ByteVector{0x11});
    assert((out == std::vector<std::int16_t>{1, 2}));
    // 预计算 golden（独立 Python 实现对拍，高半字节优先）。
    decoder.Reset(0, 0);
    out = decoder.Decode(ByteVector{0x11, 0x22, 0x77, 0x88, 0xF0, 0x0F, 0x45, 0x54});
    assert((out == std::vector<std::int16_t>{1, 2, 5, 8, 19, 49, 45, 42,
                                             -10, -3, 3, -90, 30, 208, 468, 781}));
    // 状态连续推进：分段解码与一次性解码逐样本相等。
    decoder.Reset(0, 0);
    auto joined = decoder.Decode(ByteVector{0x11, 0x22, 0x77});
    auto rest = decoder.Decode(ByteVector{0x88, 0xF0, 0x0F, 0x45, 0x54});
    joined.insert(joined.end(), rest.begin(), rest.end());
    assert(joined == out);

    // 编码往返：正弦+斜波（480 样本，偶数）经测试编码器后与解码器逐样本对拍。
    std::vector<std::int16_t> pcm(480);
    for (int i = 0; i < 480; ++i) {
        pcm[i] = static_cast<std::int16_t>(std::sin(i * 0.05) * 12000.0 + i * 10);
    }
    const auto golden = ImaEncodeForTest(pcm);
    decoder.Reset(0, 0);
    out = decoder.Decode(golden.encoded);
    assert(out == golden.expected_decoded);

    // Reset 钳位边界。
    decoder.Reset(0, 200);
    assert(decoder.step_index() == 88);
    decoder.Reset(0, -5);
    assert(decoder.step_index() == 0);
    decoder.Reset(-100, 40);
    assert(decoder.predictor() == -100 && decoder.step_index() == 40);
    // 高 step 起步解码正常推进。
    out = decoder.Decode(ByteVector{0x11, 0x11});
    assert(out.size() == 4);
}

// ===== ATVV golden fixtures 对拍 =====
// 数据源：atvv_capture.py 真机采集（或 atvv_bench.py --emit-demo-fixture 合成），
// 默认扫描 scripts/e2e_test/fixtures/xiaomi/**（VOICESTICK_REPO_ROOT 编译宏解析，
// 可用 VOICESTICK_ATVV_FIXTURES_DIR 环境变量覆盖）。每会话四件套：
// session_N.adpcm（原始流）、session_N.json（sidecar：帧长/增益/逐段 reset 区间）、
// session_N.raw.wav（纯解码）、session_N.wav（解码+三点平滑+增益）。
// 本测试按 sidecar 段落复现 C++ 解码路径，与两份 WAV 逐样本对拍。
// 无 fixtures 时打印 SKIP 直接返回（不算失败，不伪造结果）。

std::filesystem::path ResolveAtvvFixturesRoot() {
    if (const char* env = std::getenv("VOICESTICK_ATVV_FIXTURES_DIR");
        env != nullptr && *env != '\0') {
        return std::filesystem::path(env);
    }
#ifdef VOICESTICK_REPO_ROOT
    return std::filesystem::path(VOICESTICK_REPO_ROOT) /
           "scripts" / "e2e_test" / "fixtures" / "xiaomi";
#else
    return std::filesystem::path("scripts") / "e2e_test" / "fixtures" / "xiaomi";
#endif
}

std::string ReadTextFileForTest(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 读 PCM16 mono WAV（Python wave 模块产物）：walk RIFF chunk 取 fmt/data。
std::vector<std::int16_t> ReadWavPcm16ForTest(const std::filesystem::path& path) {
    const std::string bytes = ReadTextFileForTest(path);
    if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 ||
        bytes.compare(8, 4, "WAVE") != 0) {
        return {};
    }
    auto u16 = [&bytes](std::size_t off) -> std::uint16_t {
        return static_cast<std::uint16_t>(
            static_cast<unsigned char>(bytes[off]) |
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[off + 1])) << 8));
    };
    auto u32 = [&bytes](std::size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off + 3])) << 24);
    };
    std::size_t pos = 12;
    bool fmt_ok = false;
    while (pos + 8 <= bytes.size()) {
        const std::string id = bytes.substr(pos, 4);
        const std::uint32_t size = u32(pos + 4);
        const std::size_t payload = pos + 8;
        if (payload + size > bytes.size()) break;
        if (id == "fmt ") {
            fmt_ok = size >= 16 && u16(payload) == 1 &&      // PCM
                     u16(payload + 2) == 1 &&                // mono
                     u16(payload + 14) == 16;                // 16bit
        } else if (id == "data" && fmt_ok) {
            std::vector<std::int16_t> out(size / 2);
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = static_cast<std::int16_t>(u16(payload + i * 2));
            }
            return out;
        }
        pos = payload + size + (size & 1);  // chunk 按偶数字节对齐
    }
    return {};
}

struct AtvvGoldenSegment {
    std::size_t offset = 0;
    std::size_t bytes = 0;
    int predictor = 0;
    int step_index = 0;
};

bool ParseAtvvSidecarForTest(const std::string& json_text, double* gain_db,
                             std::vector<AtvvGoldenSegment>* segments) {
    cJSON* root = cJSON_Parse(json_text.c_str());
    if (root == nullptr) return false;
    const cJSON* gain = cJSON_GetObjectItemCaseSensitive(root, "gain_db");
    const cJSON* segs = cJSON_GetObjectItemCaseSensitive(root, "segments");
    bool ok = cJSON_IsNumber(gain) && cJSON_IsArray(segs) &&
              !cJSON_IsInvalid(segs) && cJSON_GetArraySize(segs) > 0;
    if (ok) {
        *gain_db = gain->valuedouble;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, segs) {
            const cJSON* offset = cJSON_GetObjectItemCaseSensitive(item, "offset");
            const cJSON* nbytes = cJSON_GetObjectItemCaseSensitive(item, "bytes");
            const cJSON* predictor = cJSON_GetObjectItemCaseSensitive(item, "predictor");
            const cJSON* step_index = cJSON_GetObjectItemCaseSensitive(item, "step_index");
            if (!cJSON_IsNumber(offset) || !cJSON_IsNumber(nbytes) ||
                !cJSON_IsNumber(predictor) || !cJSON_IsNumber(step_index)) {
                ok = false;
                break;
            }
            // double→size_t 窄化前提：sidecar 是本仓库自生成 fixtures 资产
            // （atvv_capture.py / atvv_bench.py --emit-demo-fixture 写出），
            // offset/bytes 为非负小整数；万一出现畸形值，由下方 golden 解码循环的
            // assert(seg.offset + seg.bytes <= adpcm_size) 兜底，属可接受前提。
            segments->push_back(AtvvGoldenSegment{
                static_cast<std::size_t>(offset->valuedouble),
                static_cast<std::size_t>(nbytes->valuedouble),
                predictor->valueint, step_index->valueint});
        }
    }
    cJSON_Delete(root);
    return ok;
}

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
    for (const auto& adpcm_path : adpcm_files) {
        const std::string stem = adpcm_path.stem().string();  // session_N
        const auto dir = adpcm_path.parent_path();
        const auto sidecar_path = dir / (stem + ".json");
        const auto raw_wav_path = dir / (stem + ".raw.wav");
        const auto wav_path = dir / (stem + ".wav");
        // 有 .adpcm 但缺 sidecar/WAV 属于残缺 fixtures，直接失败暴露问题。
        assert(std::filesystem::exists(sidecar_path, ec));
        assert(std::filesystem::exists(raw_wav_path, ec));
        assert(std::filesystem::exists(wav_path, ec));

        double gain_db = 0.0;
        std::vector<AtvvGoldenSegment> segments;
        assert(ParseAtvvSidecarForTest(ReadTextFileForTest(sidecar_path),
                                       &gain_db, &segments));

        const std::string adpcm_text = ReadTextFileForTest(adpcm_path);
        const auto* adpcm = reinterpret_cast<const std::uint8_t*>(adpcm_text.data());
        const std::size_t adpcm_size = adpcm_text.size();
        ImaAdpcmDecoder decoder;
        std::vector<std::int16_t> pcm;
        for (const auto& seg : segments) {
            assert(seg.offset + seg.bytes <= adpcm_size);
            decoder.Reset(static_cast<std::int16_t>(seg.predictor), seg.step_index);
            auto part = decoder.Decode(
                std::span<const std::uint8_t>(adpcm + seg.offset, seg.bytes));
            pcm.insert(pcm.end(), part.begin(), part.end());
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
        }
        assert(pcm == expected_raw);

        // 对拍 2：解码 + PcmPostprocessor(sidecar 增益) == session_N.wav。
        // Python 侧 smooth3+apply_gain 的舍入已对齐 std::lround。
        const PcmPostprocessor postprocessor(gain_db);
        const auto processed = postprocessor.Process(pcm);
        const auto expected_wav = ReadWavPcm16ForTest(wav_path);
        if (processed != expected_wav) {
            std::printf("FAIL %s wav 对拍失败（gain_db=%.2f）\n",
                        stem.c_str(), gain_db);
        }
        assert(processed == expected_wav);

        ++checked;
        std::printf("  golden %s: %zu samples, %zu segment(s) OK\n",
                    (dir.filename().string() + "/" + stem).c_str(),
                    pcm.size(), segments.size());
    }
    std::printf("ATVV golden fixtures: %d session(s) checked\n", checked);
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

// 完成 ATVV 握手（Start + v1.0 CAPS：16kHz、ADPCM 帧长 120 字节）。
void AtvvCoordinatorHandshake(XiaomiAtvvSession& session, std::int64_t& t) {
    session.Start(t);
    session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, t + 10);
    t += 10;
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
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(!loaded.xiaomi_suppress_f5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").gain_db == 20.5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").double_click_ms == 450);
    // 访问器内部归一化：带前缀与不带前缀等价。
    assert(loaded.XiaomiSettingsForDevice("RC-3A7F").gain_db == 20.5);
    assert(loaded.XiaomiSettingsForDevice("FFFF").gain_db == 12.0);
    std::filesystem::remove(temp);

    // 保存/加载往返。
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids = {"3A7F"};
    config.xiaomi_suppress_f5 = false;
    config.device_xiaomi_settings["3A7F"] = XiaomiSettings{.gain_db = 18.0, .double_click_ms = 400};
    config.Save(temp);
    loaded = AppConfig::Load(temp);
    assert(!loaded.xiaomi_suppress_f5);
    assert(loaded.XiaomiSettingsForDevice("3A7F").gain_db == 18.0);
    assert(loaded.XiaomiSettingsForDevice("3A7F").double_click_ms == 400);

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

// F5 抑制谓词：enabled 且 last>0 且 0<=age<=80ms 时吞，其余一律放行。
void TestXiaomiF5SuppressPredicate() {
    constexpr std::int64_t kLast = 100000;
    // 窗内（含 0/80ms 边界）吞。
    assert(ShouldSuppressF5(kLast, kLast, true));
    assert(ShouldSuppressF5(kLast + 79, kLast, true));
    assert(ShouldSuppressF5(kLast + kF5SuppressWindowMs, kLast, true));
    // 窗外（81ms）放行。
    assert(!ShouldSuppressF5(kLast + kF5SuppressWindowMs + 1, kLast, true));
    // 开关关闭放行。
    assert(!ShouldSuppressF5(kLast, kLast, false));
    // 从未开麦（last=0）放行。
    assert(!ShouldSuppressF5(kLast, 0, true));
    // 未来时间戳（时钟回拨/乱序，age<0）放行。
    assert(!ShouldSuppressF5(kLast - 1, kLast, true));
}

// 协调器 × 小米能力门控（规格 §4.4/§5.2）：RC 设备不下发交互/编码器设置、不走
// VoiceStick 固件更新；对照组 StickS3 设备行为不变。小米身份走 config 种子兜底路径
//（paired_devices 条目带 hardware，IsXiaomiRemoteDevice 直接命中），不注入
// device_info 事件，因此完全不触达 SavePairedDeviceInfo 落盘真实 config.toml。
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
    assert(defaults.tencent_engine_model_type == "16k_zh");
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

bool ArgvContains(const std::vector<std::wstring>& argv, const std::wstring& needle) {
    return std::find(argv.begin(), argv.end(), needle) != argv.end();
}

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

FlashOptions MakeFlashOptions(const FlashTestPaths& paths) {
    FlashOptions options;
    options.serial_port = L"COM5";
    options.firmware_path = paths.firmware.wstring();
    return options;
}

} // namespace

void TestFlashToolFlow() {
    const FlashTestPaths paths;

    // 成功：退出码 0 → Finished(success)。
    {
        FakeFlashRunner runner;
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(tool.Run());
        assert(runner.calls.size() == 1);
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && last.success && !last.cancelled);
    }

    // 失败：退出码非 0 → Finished(failure) 且有错误事件。
    {
        FakeFlashRunner runner;
        runner.exit_codes = {1};
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success && !last.cancelled);
        bool saw_error = false;
        for (const auto& e : events) if (e.kind == FlashEvent::kError) saw_error = true;
        assert(saw_error);
    }

    // EraseThenFull：先 erase_flash 后 write_flash 的顺序断言；erase 失败不进入写。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.mode = FlashMode::kEraseThenFull;
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(tool.Run());
        assert(runner.calls.size() == 2);
        assert(ArgvContains(runner.calls[0], L"erase_flash"));
        assert(ArgvContains(runner.calls[1], L"write_flash"));
    }
    {
        FakeFlashRunner runner;
        runner.exit_codes = {1, 0};  // erase 失败
        FlashOptions options = MakeFlashOptions(paths);
        options.mode = FlashMode::kEraseThenFull;
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(!tool.Run());
        assert(runner.calls.size() == 1);  // 不进入第二步
    }

    // 取消：runner 返回 kFlashCancelledExitCode → Finished(cancelled)。
    {
        FakeFlashRunner runner;
        runner.exit_codes = {kFlashCancelledExitCode};
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success && last.cancelled);
    }

    // 校验失败：未选串口 → 不拉起子进程，直接 Finished(failure)。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.serial_port.clear();
        std::vector<FlashEvent> events;
        FlashTool tool(options, paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        assert(runner.calls.empty());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success);
    }

    // 校验失败：非 .bin 后缀。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.firmware_path = paths.python.wstring();  // .exe 后缀
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(!tool.Run());
        assert(runner.calls.empty());
    }
}

// ---- 电池电压监测：power_log 解析与增量累积 ----

std::vector<std::uint8_t> BuildPowerLogEntry(std::uint32_t uptime_s, std::uint16_t vbat_mv,
                                             std::uint8_t mode, std::uint8_t flags,
                                             std::uint32_t reserved = 0) {
    return {
        static_cast<std::uint8_t>(uptime_s & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 8) & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 16) & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 24) & 0xFF),
        static_cast<std::uint8_t>(vbat_mv & 0xFF),
        static_cast<std::uint8_t>((vbat_mv >> 8) & 0xFF),
        mode,
        flags,
        static_cast<std::uint8_t>(reserved & 0xFF),
        static_cast<std::uint8_t>((reserved >> 8) & 0xFF),
        static_cast<std::uint8_t>((reserved >> 16) & 0xFF),
        static_cast<std::uint8_t>((reserved >> 24) & 0xFF),
    };
}

std::string Base64EncodeForTest(const std::vector<std::uint8_t>& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (i + 1 < data.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                (i + 2 < data.size() ? static_cast<std::uint32_t>(data[i + 2]) : 0);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < data.size() ? kTable[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < data.size() ? kTable[n & 0x3F] : '=');
    }
    return out;
}

std::vector<std::uint8_t> BuildStateJsonFrame(const std::string& json) {
    std::vector<std::uint8_t> frame{0x01, 0x10,
                                    static_cast<std::uint8_t>(json.size() & 0xFF),
                                    static_cast<std::uint8_t>((json.size() >> 8) & 0xFF)};
    frame.insert(frame.end(), json.begin(), json.end());
    return frame;
}

void TestPowerLogMonitor() {
    // 1) 条目解析。
    const auto anchor_entry = BuildPowerLogEntry(1000, 0, 0xFF, kPowerLogFlagTimeAnchor, 1755800000u);
    PowerLogEntryData parsed{};
    assert(ParsePowerLogEntry(anchor_entry.data(), anchor_entry.size(), &parsed));
    assert(parsed.uptime_s == 1000);
    assert(parsed.is_time_anchor);
    assert(parsed.anchor_epoch == 1755800000u);

    // 2) 分片帧解析：ParseStateEvent 对 power_log 帧应返回 nullopt（无 event 字段）。
    std::vector<std::uint8_t> blob;
    const auto periodic1 = BuildPowerLogEntry(1060, 4100, 0, kPowerLogFlagPeriodic);
    blob.insert(blob.end(), periodic1.begin(), periodic1.end());
    const auto json = std::string("{\"power_log\":{\"seq\":0,\"offset\":16,\"total\":40,") +
                      "\"eof\":0,\"data\":\"" + Base64EncodeForTest(blob) + "\"}}";
    const auto frame = BuildStateJsonFrame(json);
    assert(!BleProtocol::ParseStateEvent(frame).has_value());
    const auto fragment = BleProtocol::ParsePowerLogFragment(frame);
    assert(fragment.has_value());
    assert(fragment->seq == 0);
    assert(fragment->offset == 16);
    assert(fragment->total == 40);
    assert(!fragment->eof);
    assert(fragment->data == blob);

    // eof 空数据分片（探测响应）。
    const auto eof_frame = BuildStateJsonFrame(
        "{\"power_log\":{\"seq\":0,\"offset\":1000000000,\"total\":40,\"eof\":1,\"data\":\"\"}}");
    const auto eof_fragment = BleProtocol::ParsePowerLogFragment(eof_frame);
    assert(eof_fragment.has_value());
    assert(eof_fragment->eof);
    assert(eof_fragment->total == 40);
    assert(eof_fragment->data.empty());

    // 3) 命令 payload。
    const auto dump_payload = BleProtocol::PowerLogDumpPayload(1000000000u, 160);
    const std::string dump_json(dump_payload.begin(), dump_payload.end());
    assert(dump_json.find("\"power_log\"") != std::string::npos);
    assert(dump_json.find("\"cmd\":\"dump\"") != std::string::npos);
    assert(dump_json.find("\"offset\":1000000000") != std::string::npos);
    assert(dump_json.find("\"max\":160") != std::string::npos);
    const auto anchor_payload = BleProtocol::PowerLogTimeAnchorPayload(1755800000u);
    const std::string anchor_json(anchor_payload.begin(), anchor_payload.end());
    assert(anchor_json.find("\"cmd\":\"time_anchor\"") != std::string::npos);
    assert(anchor_json.find("\"epoch\":1755800000") != std::string::npos);

    // 3b) 供电态（USB）自动关机：power_mgmt 事件解析 + 命令 payload。
    // ParseStateEvent 对 power_mgmt 帧返回 nullopt（由 ParsePowerMgmtEvent 消费）。
    const auto pm_on_frame = BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":true}");
    assert(!BleProtocol::ParseStateEvent(pm_on_frame).has_value());
    const auto pm_on = BleProtocol::ParsePowerMgmtEvent(pm_on_frame);
    assert(pm_on.has_value() && *pm_on);
    const auto pm_off = BleProtocol::ParsePowerMgmtEvent(BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":false}"));
    assert(pm_off.has_value() && !*pm_off);
    // 缺字段或非 power_mgmt 帧 → nullopt。
    assert(!BleProtocol::ParsePowerMgmtEvent(
        BuildStateJsonFrame("{\"event\":\"power_mgmt\"}")).has_value());
    assert(!BleProtocol::ParsePowerMgmtEvent(frame).has_value());  // power_log 分片帧
    // 命令 payload：set 带布尔 enabled，get 为查询命令。
    const auto set_on_payload = BleProtocol::UsbAutoOffPayload(true);
    const std::string set_on_json(set_on_payload.begin(), set_on_payload.end());
    assert(set_on_json.find("\"event\":\"usb_auto_off\"") != std::string::npos);
    assert(set_on_json.find("\"enabled\":true") != std::string::npos);
    const auto set_off_payload = BleProtocol::UsbAutoOffPayload(false);
    const std::string set_off_json(set_off_payload.begin(), set_off_payload.end());
    assert(set_off_json.find("\"enabled\":false") != std::string::npos);
    const auto get_payload = BleProtocol::UsbAutoOffGetPayload();
    const std::string get_json(get_payload.begin(), get_payload.end());
    assert(get_json.find("\"event\":\"usb_auto_off_get\"") != std::string::npos);

    // 4) 累积器：锚点 + 周期采样 + 非周期事件过滤 + epoch 对齐。
    PowerLogAccumulator accumulator;
    std::vector<std::uint8_t> blob1;
    blob1.insert(blob1.end(), anchor_entry.begin(), anchor_entry.end());
    // 非周期模式切换事件（无 PERIODIC 位）：不应产生采样。
    const auto mode_entry = BuildPowerLogEntry(1010, 0, 1, 0);
    blob1.insert(blob1.end(), mode_entry.begin(), mode_entry.end());
    // 两个周期采样（一个充电、一个放电）。
    const auto sample1 = BuildPowerLogEntry(1060, 4100, 0,
                                            kPowerLogFlagPeriodic | kPowerLogFlagUsbPowered |
                                                kPowerLogFlagCharging);
    blob1.insert(blob1.end(), sample1.begin(), sample1.end());
    const auto sample2 = BuildPowerLogEntry(1120, 4090, 0, kPowerLogFlagPeriodic);
    blob1.insert(blob1.end(), sample2.begin(), sample2.end());

    std::vector<PowerLogSample> new_samples;
    assert(accumulator.ConsumeIncrementalBlob(blob1.data(), blob1.size(), &new_samples));
    assert(new_samples.size() == 2);
    assert(accumulator.samples().size() == 2);
    // epoch 对齐：anchor(epoch=1755800000, uptime=1000) → uptime 1060 → +60s。
    assert(accumulator.samples()[0].epoch_s == 1755800060);
    assert(accumulator.samples()[1].epoch_s == 1755800120);
    assert(accumulator.samples()[0].vbat_mv == 4100);
    assert(accumulator.samples()[0].charging);
    assert(accumulator.samples()[0].usb_powered);
    assert(!accumulator.samples()[1].charging);
    assert(accumulator.last_uptime_s() == 1120);

    // 5) 增量第二段：仅新周期采样。
    std::vector<std::uint8_t> blob2;
    const auto sample3 = BuildPowerLogEntry(1180, 4080, 0, kPowerLogFlagPeriodic);
    blob2.insert(blob2.end(), sample3.begin(), sample3.end());
    new_samples.clear();
    assert(accumulator.ConsumeIncrementalBlob(blob2.data(), blob2.size(), &new_samples));
    assert(new_samples.size() == 1);
    assert(accumulator.samples().size() == 3);
    assert(accumulator.samples()[2].epoch_s == 1755800180);

    // 6) 重启检测：uptime 回退返回 false 且状态不变。
    std::vector<std::uint8_t> blob_restart;
    const auto stale = BuildPowerLogEntry(50, 4000, 0, kPowerLogFlagPeriodic);
    blob_restart.insert(blob_restart.end(), stale.begin(), stale.end());
    new_samples.clear();
    assert(!accumulator.ConsumeIncrementalBlob(blob_restart.data(), blob_restart.size(),
                                               &new_samples));
    assert(new_samples.empty());
    assert(accumulator.samples().size() == 3);

    // 7) 长度非 12 倍数视为流损坏。
    const std::vector<std::uint8_t> bad_blob(13, 0);
    assert(!accumulator.ConsumeIncrementalBlob(bad_blob.data(), bad_blob.size(), nullptr));

    // 8) CSV：表头 + 每采样一行 + 无效读数标记。
    const std::string csv = accumulator.FormatCsv();
    assert(csv.find("seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v") == 0);
    assert(csv.find("1755800060") != std::string::npos);
    assert(csv.find("4.100") != std::string::npos);
    assert(csv.find("S0_ACTIVE") != std::string::npos);

    // 9) 无锚点场景：epoch 未对齐（-1）但仍收集采样。
    PowerLogAccumulator bare;
    std::vector<std::uint8_t> blob_no_anchor;
    const auto orphan = BuildPowerLogEntry(60, 4050, 0, kPowerLogFlagPeriodic);
    blob_no_anchor.insert(blob_no_anchor.end(), orphan.begin(), orphan.end());
    assert(bare.ConsumeIncrementalBlob(blob_no_anchor.data(), blob_no_anchor.size(), nullptr));
    assert(bare.samples().size() == 1);
    assert(bare.samples()[0].epoch_s == -1);

    // 10) 锚点之后的更新锚点生效（取 uptime 最大者）。
    PowerLogAccumulator re_anchor;
    std::vector<std::uint8_t> blob_re_anchor;
    blob_re_anchor.insert(blob_re_anchor.end(), anchor_entry.begin(), anchor_entry.end());
    const auto anchor2 = BuildPowerLogEntry(2000, 0, 0xFF, kPowerLogFlagTimeAnchor, 1755810000u);
    blob_re_anchor.insert(blob_re_anchor.end(), anchor2.begin(), anchor2.end());
    const auto sample_after = BuildPowerLogEntry(2060, 4070, 0, kPowerLogFlagPeriodic);
    blob_re_anchor.insert(blob_re_anchor.end(), sample_after.begin(), sample_after.end());
    assert(re_anchor.ConsumeIncrementalBlob(blob_re_anchor.data(), blob_re_anchor.size(), nullptr));
    assert(re_anchor.samples().size() == 1);
    assert(re_anchor.samples()[0].epoch_s == 1755810060);

    printf("TestPowerLogMonitor passed\n");
}

int main() {
#ifdef _DEBUG
    // CI/命令行友好：Debug 下 assert 失败写 stderr 后直接 abort，
    // 避免 CRT 默认弹「Microsoft Visual C++ Runtime Library」对话框挂起测试进程。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    TestDeviceIds();
    TestPairDeviceHelpers();
    TestPairingAdvertisementClassify();
    TestPowerLogMonitor();
    TestAudioFrameParsing();
    TestBleControlPayloads();
    TestStateParsing();
    TestEncoderRotateStateParsing();
    TestStateEventSourceParsing();
    TestEncoderStatusParsing();
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
    TestAsrHotwordCorpusBudget();
    TestHotwordSelector();
    TestTencentHotwordCharFilter();
    TestVolcengineTableIdConfigRoundTrip();
    TestAppConfig();
    TestAppConfigTapSensitivityRoundTrip();
    TestAppConfigAirMouseRoundTrip();
    TestAppConfigDebugAudioDirUtf8RoundTrip();
    TestConfigTemplateSeeding();
    TestNeedsAsrStep();
    TestActiveApiKeyBuiltinFallback();
    TestResolveActiveString();
    TestActiveResourceId();
    TestSavePreservingDiskCredentials();
    TestSaveSettingsDialog();
    TestLlmRefinePromptAndPayload();
    TestHotwordProcessConfig();
    TestHotwordExtractorPromptAndParse();
    TestHotwordCandidateMiner();
    TestHotwordExtractionPromptAndParse();
    TestFirmwareManifestParsingAndVersionCompare();
    TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate();
    TestBleEncoderPayloads();
    TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsInteractionSettingsPerDeviceOverride();
    TestCoordinatorUpdateFirmwareFromFile();
    TestParseOtaCliArgs();
    TestCoordinatorHotkeyWithoutConnectionShowsWakeHint();
    TestCoordinatorHotkeyWithConnectionSendsRemoteButton();
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
    TestTapEventInjectsArrowDown();
    TestTapDisabledWhenConfigOff();
    TestTapIgnoredDuringRecording();
    TestTapThrottledWithin500ms();
    TestTapThrottleRecoversAfter500ms();
    TestInputInjectorArrowUpFakeWiring();
    TestInputInjectorKeyComboFakeWiring();
    TestKeySpecParse();
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
    TestEncoderRotateIsolatedTwoStepNudgeStaysSlow();
    TestEncoderRotateSustainedTwoStepRotationGoesFast();
    TestEncoderRotateSlowFlushesAfterDecisionWindow();
    TestEncoderRotateSlowContinuousBatches();
    TestEncoderRotatePendingDirectionChangeFlushesOld();
    TestEncoderRotateFastInvalidKeyFallsBackToNormalKey();
    TestAppConfigEncoderFastSettingsRoundTrip();
    TestAppConfigEncoderFastSettingsInvalidFallback();
    TestPhysicalPrimaryUnaffectedByEncoderConfig();
    TestEncoderPressRecordingButtonUpStopsSession();
    TestEncoderSourceSecondaryFallsBackToPhysicalPath();
    TestEncoderConfigUpdateTakesEffectImmediately();
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
    TestDeepSeekThinkingDisabled();
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
    TestXiaomiAtvvCapsParsing();
    TestImaAdpcmDecoderGolden();
    TestImaAdpcmDecoderGoldenFixtures();
    TestFrameAccumulator();
    TestPcmPostprocessor();
    TestAudioOpusEncoderRoundTrip();
    TestXiaomiAtvvSessionFlow();
    TestXiaomiAtvvSessionKeyMapping();
    TestXiaomiAtvvSessionClickTapTimeoutSilent();
    TestXiaomiAtvvSessionReopenRejectWindow();
    TestXiaomiAtvvStreamStartOpensSession();
    TestXiaomiAtvvSessionEncoderResetPerSession();
    TestXiaomiAtvvServiceUuidAd();
    TestCoordinatorXiaomiHoldToTalkStreamsOggToAsr();
    TestCoordinatorXiaomiCancelSemantics();
    TestCoordinatorXiaomiWechatInputMethodDecodesToVirtualMic();
    TestCoordinatorXiaomiSubtitlePath();
    TestDeviceIdRcPrefix();
    TestAppConfigXiaomiTable();
    TestXiaomiF5SuppressPredicate();
    TestCoordinatorXiaomiCapabilityGating();
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
    TestComPortScoring();
    TestEsptoolCommandBuilder();
    TestEsptoolProgressParser();
    TestFlashToolFlow();
    return 0;
}
