// N8: core_tests shared test fixtures (Sent* records, Fake* doubles, event// builders and assertions), extracted from core_tests.cc so that domain-split// suite files can share them. Include block mirrors core_tests (idempotent via// header guards); free functions are marked inline (multi-TU safety); class// members defined in-class are implicitly inline.#pragma once#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "audio_opus_encoder.h"
#include "ble_protocol.h"
#include "test_suites.h"  // N8: suite registry for split test files
#include "ima_adpcm_decoder.h"
#include "pcm_postprocessor.h"
#include "xiaomi_atvv_protocol.h"
#include "xiaomi_atvv_session.h"
#include "xiaomi_buttons.h"
#include "xiaomi_keymap_interceptor.h"
#include "xiaomi_usage_tap.h"
#include "xiaomi_usage_tap_decoder.h"
#include "xiaomi_usage_tap_host.h"
#include "xiaomi_usage_tap_manager.h"
#include "xiaomi_usage_tap.h"
#include "cmd_line.h"
#include "com_port_selector.h"

#include <opus.h>
#include "byte_utils.h"
#include "clipboard_vault.h"
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
#include "log.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "ogg_opus_demuxer.h"
#include "local_asr_client_win.h"
#include "local_refinement_client.h"
#include "model_manifest.h"
#include "model_downloader.h"
#include "model_download_session.h"
#include "voice_stick_cloud_api_win.h"
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#include "llama_cpp_engine.h"
#endif
#include "text_refiner.h"
#include "pinyin_guard.h"
#include "refine_history.h"
#include "mic_capture.h"
#include "license.h"
#include "serial_base32.h"
#include "wasapi_mic_capture.h"
#include "push_to_talk_key.h"
#include "provider_combo.h"
#include "selection_correction.h"
#include "shortcut_capture.h"
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
#include <winsock2.h>
#include <windows.h>  // SEH 探针（AddVectoredExceptionHandlerFirst）；置于 winsock2 之后避免 winsock 冲突
#include <bcrypt.h>
#include <thread>
#include <utility>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cctype>
#include <cmath>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <future>
#include <optional>
#include <atomic>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;
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
    void SendProtoNegotiate(const std::optional<std::string>& device_id) override {
        proto_negotiate_count++;
        (void)device_id;
    }
    int proto_negotiate_count = 0;
    void SendGatewayKeymapGet(const std::optional<std::string>& device_id) override {
        keymap_get_count++;
        (void)device_id;
    }
    int keymap_get_count = 0;
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
    void SendGatewayKeymapSet(const std::string& key, bool software,
                              const std::optional<std::string>& device_id) override {
        sent_gateway_keymap_sets.push_back(SentGatewayKeymapSet{key, software, device_id});
    }
    void SendGatewayTargetInfo(const std::string& name,
                               const std::optional<std::string>& device_id) override {
        sent_gateway_target_infos.push_back(std::pair{name, device_id});
    }
    void SendGatewaySelectTarget(bool self,
                                 const std::optional<std::string>& device_id) override {
        sent_gateway_select_targets.push_back(std::pair{self, device_id});
    }
    void SendRawControl(const std::string& json,
                        const std::optional<std::string>& device_id) override {
        sent_raw_controls.push_back(std::pair{json, device_id});
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
    struct SentGatewayKeymapSet {
        std::string key;
        bool software;
        std::optional<std::string> device_id;
    };
    std::vector<SentGatewayKeymapSet> sent_gateway_keymap_sets;
    // P1 目标表：桌面端上报的主机名（name, device_id）。
    std::vector<std::pair<std::string, std::optional<std::string>>> sent_gateway_target_infos;
    // P1 切换器：桌面端下发的目标选择（self, device_id）。
    std::vector<std::pair<bool, std::optional<std::string>>> sent_gateway_select_targets;
    // 调试/自动化：原始控制帧下发（json, device_id）。
    std::vector<std::pair<std::string, std::optional<std::string>>> sent_raw_controls;
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
    void SetDeviceGatewayMode(const std::string& device_id, bool gateway) override {
        gateway_mode_by_device_id[device_id] = gateway;
        last_gateway_mode = gateway;
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
    void ShowFirmwareUpdateBalloon(const std::string& device_id,
                                   const std::string& current_version,
                                   const std::string& latest_version,
                                   bool is_below_minimum) override {
        firmware_update_balloons.push_back(device_id + ":" + current_version + ":" + latest_version +
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
    // 网关模式上报（gateway_status）：仅记录，供需要时断言。
    std::map<std::string, bool> gateway_mode_by_device_id;
    bool last_gateway_mode = false;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id;
    std::vector<std::string> pairing_errors;
    std::vector<std::string> firmware_update_prompts;
    std::vector<std::string> firmware_update_balloons;
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
inline bool ContainsWide(std::wstring_view haystack, std::wstring_view needle) {
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

// 测试用本机麦克风采集器：解耦真实 WASAPI，on_pcm 由测试线程手动触发
// （等价真实实现的采集线程回调语义）。
class FakeMicCapture : public IMicCapture {
public:
    bool Start() override {
        ++start_count;
        return start_result;
    }
    void Stop() override {
        ++stop_count;
    }
    std::string LastStartError() const override {
        return start_error;
    }

    bool start_result = true;
    std::string start_error;
    int start_count = 0;
    int stop_count = 0;
};

inline StateEvent ButtonEvent(const std::string& event,
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
inline StateEvent DoubleClickEvent(const std::string& button) {
    StateEvent state_event;
    state_event.event = "button_double_click";
    state_event.button = button;
    return state_event;
}

// 构造敲击事件（固件上报的 {"event":"tap","kind":"double"}）。
inline StateEvent TapEvent(const std::string& kind = "double") {
    StateEvent state_event;
    state_event.event = "tap";
    state_event.button = kind;  // 复用 button 字段承载 kind，与协议解析一致
    return state_event;
}

// 构造编码器旋转事件（固件上报的 {"event":"encoder_rotate","direction":"cw","steps":2}）。
inline StateEvent EncoderRotateEvent(const std::string& direction, std::uint32_t steps) {
    StateEvent state_event;
    state_event.event = "encoder_rotate";
    state_event.direction = direction;
    state_event.steps = steps;
    return state_event;
}

// 构造编码器按键事件（固件上报带 "source":"encoder"）。
inline StateEvent EncoderButtonEvent(const std::string& event,
                              std::optional<std::uint32_t> session_id = std::nullopt) {
    StateEvent state_event;
    state_event.event = event;
    state_event.button = "primary";
    state_event.session_id = session_id;
    state_event.source = "encoder";
    return state_event;
}

inline AudioFrame AudioDataFrame(std::uint32_t session_id, std::uint32_t seq, bool is_end = false) {
    AudioFrame frame;
    frame.session_id = session_id;
    frame.seq = seq;
    frame.flags = is_end ? 0x02 : 0;
    frame.payload = {1, 2, 3, 4};
    return frame;
}

inline AudioFrame EmptyEndFrame(std::uint32_t session_id, std::uint32_t seq) {
    AudioFrame frame;
    frame.session_id = session_id;
    frame.seq = seq;
    frame.flags = 0x02;
    return frame;
}

inline bool HasUiState(const FakeBleCentral& ble, const std::string& state, const std::string& device_id) {
    return std::any_of(ble.sent_ui_states.begin(), ble.sent_ui_states.end(),
                       [&](const SentUiState& sent) {
                           return sent.state == state &&
                                  sent.device_id.has_value() &&
                                  *sent.device_id == device_id;
                       });
}

inline bool HasUiStateText(const FakeBleCentral& ble,
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

// 构造一个 16 kHz、40 ms（640 样本）的单声道正弦波 PCM 帧。
inline std::vector<int16_t> MakeSinePcm(int frequency_hz, int sample_rate = 16000) {
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
inline std::vector<uint8_t> EncodeOpusPacket(const std::vector<int16_t>& pcm,
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

// ---- 小米蓝牙遥控器 2 Pro（ATVV）core 纯逻辑层 ----
// 协议规范见 Doc/Plan/xiaomi-remote-2-pro-support.md §3；按键语义镜像固件双击设计。

// 会话动作列表查找/收集辅助。
inline const XiaomiAtvvWriteTx* FindAtvvWriteTx(const std::vector<XiaomiAtvvAction>& actions) {
    for (const auto& action : actions) {
        if (const auto* tx = std::get_if<XiaomiAtvvWriteTx>(&action)) return tx;
    }
    return nullptr;
}

inline const StateEvent* FindAtvvEvent(const std::vector<XiaomiAtvvAction>& actions, std::string_view name) {
    for (const auto& action : actions) {
        if (const auto* event = std::get_if<XiaomiAtvvStateEvent>(&action)) {
            if (event->event.event == name) return &event->event;
        }
    }
    return nullptr;
}

inline std::vector<AudioFrame> CollectAtvvFrames(const std::vector<XiaomiAtvvAction>& actions) {
    std::vector<AudioFrame> frames;
    for (const auto& action : actions) {
        if (const auto* frame = std::get_if<XiaomiAtvvAudioFrame>(&action)) {
            frames.push_back(frame->frame);
        }
    }
    return frames;
}

inline bool HasAtvvError(const std::vector<XiaomiAtvvAction>& actions, std::string_view code) {
    for (const auto& action : actions) {
        if (const auto* error = std::get_if<XiaomiAtvvError>(&action)) {
            if (error->code == code) return true;
        }
    }
    return false;
}

// 快速完成握手进入 Ready（Start + v1.0 CAPS：16kHz、帧长 120）。
inline void AtvvHandshakeReady(XiaomiAtvvSession& session, std::int64_t now_ms) {
    session.Start(now_ms);
    session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, now_ms + 10);
}

// 完成 ATVV 握手（Start + v1.0 CAPS：16kHz、ADPCM 帧长 120 字节）。
inline void AtvvCoordinatorHandshake(XiaomiAtvvSession& session, std::int64_t& t) {
    session.Start(t);
    session.HandleControlCommand(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78}, t + 10);
    t += 10;
}

// ---- 协调器 × 小米事件流（规格 §7.1）----
// 事件由 XiaomiAtvvSession 真实产出后直接注入 FakeBleCentral 回调（不需真 BLE）；
// 协调器对设备类别无感知，RC-XXXX 与 VS-XXXX 走同一状态机。

// 把 session 产出的动作注入协调器（WriteTx 是回遥控器字节、Error 无协调器语义，均忽略）。
inline void InjectAtvvActions(FakeBleCentral& ble, const std::string& device_id,
                       const std::vector<XiaomiAtvvAction>& actions) {
    for (const auto& action : actions) {
        if (const auto* event = std::get_if<XiaomiAtvvStateEvent>(&action)) {
            ble.on_state_event(device_id, event->event);
        } else if (const auto* frame = std::get_if<XiaomiAtvvAudioFrame>(&action)) {
            ble.on_audio_frame(device_id, frame->frame);
        }
    }
}


// hold_to_talk 按下段：MIC_OPEN → STREAM_START → 音频暂存 → 跨 300ms 阈值确认长按
// （button_down + 暂存帧注入协调器）。返回后协调器应处于 recording。
inline void AtvvBeginHoldRecording(FakeBleCentral& ble, const std::string& device_id,
                            XiaomiAtvvSession& session, std::int64_t& t) {
    InjectAtvvActions(ble, device_id, session.HandleControlCommand(ByteVector{0x08}, t));
    InjectAtvvActions(ble, device_id,
                      session.HandleControlCommand(ByteVector{0x04, 0x03, 0x02, 0x01}, t + 10));
    InjectAtvvActions(ble, device_id, session.HandleAudioData(ByteVector(480, 0x11), t + 20));
    InjectAtvvActions(ble, device_id, session.Tick(t + XiaomiAtvvSession::kHoldThresholdMs));
    t += XiaomiAtvvSession::kHoldThresholdMs;
}

// 松开段：STOP（button_up 注入）→ 150ms 尾包宽限到期 FinalizeStream（end 帧注入）。
inline void AtvvEndRecording(FakeBleCentral& ble, const std::string& device_id,
                      XiaomiAtvvSession& session, std::int64_t& t) {
    t += 600;
    InjectAtvvActions(ble, device_id, session.HandleControlCommand(ByteVector{0x00}, t));
    InjectAtvvActions(ble, device_id, session.Tick(t + XiaomiAtvvSession::kAudioTailGraceMs));
    t += XiaomiAtvvSession::kAudioTailGraceMs;
}

// usage tap 纯逻辑层（Doc/Plan/xiaomi-remote-usage-tap.md §3.2.1 + §6）：9 字节
// 报文解析、usage→按钮表、会话集合 diff 沿、三键直触发状态机、tap 佐证表。
inline void MakeTapReport(uint8_t (&out)[9], uint16_t a, uint16_t b, uint16_t c) {
    out[0] = 0x01; out[1] = 0x00; out[2] = 0x00;
    out[3] = static_cast<uint8_t>(a & 0xFF); out[4] = static_cast<uint8_t>(a >> 8);
    out[5] = static_cast<uint8_t>(b & 0xFF); out[6] = static_cast<uint8_t>(b >> 8);
    out[7] = static_cast<uint8_t>(c & 0xFF); out[8] = static_cast<uint8_t>(c >> 8);
}

// N8 cut13 helper block (orig lines 2667-2687)
// ===== ATVV golden fixtures 对拍 =====
// 数据源：atvv_capture.py 真机采集（或 atvv_bench.py --emit-demo-fixture 合成），
// 默认扫描 scripts/e2e_test/fixtures/xiaomi/**（VOICESTICK_REPO_ROOT 编译宏解析，
// 可用 VOICESTICK_ATVV_FIXTURES_DIR 环境变量覆盖）。每会话四件套：
// session_N.adpcm（原始流）、session_N.json（sidecar：帧长/增益/逐段 reset 区间）、
// session_N.raw.wav（纯解码）、session_N.wav（解码+三点平滑+增益）。
// 本测试按 sidecar 段落复现 C++ 解码路径，与两份 WAV 逐样本对拍。
// 无 fixtures 时打印 SKIP 直接返回（不算失败，不伪造结果）。

inline std::filesystem::path ResolveAtvvFixturesRoot() {
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


// N8 cut13 helper block (orig lines 2689-2694)
inline std::string ReadTextFileForTest(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}


// N8 cut13 helper block (orig lines 2696-2735)
// 读 PCM16 mono WAV（Python wave 模块产物）：walk RIFF chunk 取 fmt/data。
inline std::vector<std::int16_t> ReadWavPcm16ForTest(const std::filesystem::path& path) {
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


// N8 cut13 helper block (orig lines 2737-2742)
struct AtvvGoldenSegment {
    std::size_t offset = 0;
    std::size_t bytes = 0;
    int predictor = 0;
    int step_index = 0;
};


// N8 cut13 helper block (orig lines 2744-2777)
inline bool ParseAtvvSidecarForTest(const std::string& json_text, double* gain_db,
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


// N8 cut13 helper block (orig lines 5526-5528)
inline bool ArgvContains(const std::vector<std::wstring>& argv, const std::wstring& needle) {
    return std::find(argv.begin(), argv.end(), needle) != argv.end();
}


// N8 cut13 helper block (orig lines 5660-5678)
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


// N8 cut13 helper block (orig lines 5680-5685)
inline FlashOptions MakeFlashOptions(const FlashTestPaths& paths) {
    FlashOptions options;
    options.serial_port = L"COM5";
    options.firmware_path = paths.firmware.wstring();
    return options;
}
