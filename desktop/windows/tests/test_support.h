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

// FakeFlashRunner shared by flash tests in core_tests.cc and the codec_serial
// suite (needed by both translation units).
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

// N8 cut14 helper: IMA golden vector builder
// 测试本地 IMA 编码器（公开标准算法的独立实现）：输出编码字节与编码器内部
// predictor 轨迹（即标准解码的期望输出），用于与解码器逐样本对拍。
struct ImaGoldenVector {
    ByteVector encoded;
    std::vector<std::int16_t> expected_decoded;
};

inline ImaGoldenVector ImaEncodeForTest(const std::vector<std::int16_t>& pcm) {
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

// N8 cut14 helper: TestSha256Hex is a hash helper despite its Test prefix.
// 测试内独立 SHA-256：与被测实现各算各的，避免自证。
inline std::string TestSha256Hex(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        std::fprintf(stderr, "TestSha256Hex: BCryptOpenAlgorithmProvider failed\n");
        std::abort();
    }
    std::uint8_t digest[32] = {};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        std::fprintf(stderr, "TestSha256Hex: BCryptHash failed\n");
        std::abort();
    }
    char hex[65] = {};
    for (std::size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return hex;
}

// N8 cut14 helper block (orig 2758)
// ---- 电池电压监测：power_log 解析与增量累积 ----

inline std::vector<std::uint8_t> BuildPowerLogEntry(std::uint32_t uptime_s, std::uint16_t vbat_mv,
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


// N8 cut14 helper block (orig 2779)
inline std::string Base64EncodeForTest(const std::vector<std::uint8_t>& data) {
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


// N8 cut14 helper block (orig 2794)
inline std::vector<std::uint8_t> BuildStateJsonFrame(const std::string& json) {
    std::vector<std::uint8_t> frame{0x01, 0x10,
                                    static_cast<std::uint8_t>(json.size() & 0xFF),
                                    static_cast<std::uint8_t>((json.size() >> 8) & 0xFF)};
    frame.insert(frame.end(), json.begin(), json.end());
    return frame;
}


// N8 cut14 helper block (orig 2957)
// ---------- LocalAsrClient（本机麦克风模式迭代一：SenseVoice 离线识别） ----------

// 探测 SenseVoice 模型目录：环境变量 VOICESTICK_SENSEVOICE_DIR 优先，
// 否则按测试 exe 位置（build-x64）回推仓库根下的 m0/models。
inline static std::filesystem::path DetectSenseVoiceDir() {
    if (const char* env = std::getenv("VOICESTICK_SENSEVOICE_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* rel : {"../../../m0/models", "../../../../m0/models"}) {
        auto dir = fs::weakly_canonical(fs::path(rel) /
            "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17", ec);
        if (!ec && fs::exists(dir / "model.int8.onnx", ec) &&
            fs::exists(dir / "tokens.txt", ec)) {
            return dir;
        }
    }
    return {};
}


// N8 cut14 helper block (orig 2978)
// 读 16 kHz 单声道 PCM16 wav 的 data 段（RIFF 解析，非 PCM16/mono 直接失败）。
inline static bool ReadMonoPcm16Wav(const std::filesystem::path& path,
                             std::vector<std::int16_t>& pcm) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    ByteVector bytes((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0) return false;
    // 遍历 chunk 找 fmt 与 data。
    bool pcm16_mono_16k = false;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const auto chunk_size = static_cast<size_t>(bytes[pos + 4]) |
                                (static_cast<size_t>(bytes[pos + 5]) << 8) |
                                (static_cast<size_t>(bytes[pos + 6]) << 16) |
                                (static_cast<size_t>(bytes[pos + 7]) << 24);
        if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= bytes.size()) {
            const auto channels = static_cast<uint16_t>(bytes[pos + 10] |
                                                        (bytes[pos + 11] << 8));
            const auto sample_rate = static_cast<uint32_t>(bytes[pos + 12]) |
                                     (static_cast<uint32_t>(bytes[pos + 13]) << 8) |
                                     (static_cast<uint32_t>(bytes[pos + 14]) << 16) |
                                     (static_cast<uint32_t>(bytes[pos + 15]) << 24);
            const auto bits = static_cast<uint16_t>(bytes[pos + 22] |
                                                    (bytes[pos + 23] << 8));
            pcm16_mono_16k = channels == 1 && sample_rate == 16000 && bits == 16;
        } else if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
            if (!pcm16_mono_16k) return false;
            const auto sample_bytes = std::min(chunk_size, bytes.size() - pos - 8);
            pcm.resize(sample_bytes / 2);
            std::memcpy(pcm.data(), bytes.data() + pos + 8, pcm.size() * 2);
            return true;
        }
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    return false;
}


// N8 cut14 helper block (orig 4505)
// 追加 frame_count 个 40ms 静音帧并返回新增 Ogg 页字节（编码器/复用器状态跨调用
// 保留，复现协调器逐帧送流的形态）。
inline static ByteVector EncodeSilenceFrames(AudioOpusEncoder& encoder, OggOpusMuxer& muxer,
                                      int frame_count) {
    ByteVector out;
    std::vector<std::int16_t> silence(AudioOpusEncoder::kFrameSamples, 0);
    std::uint8_t packet[512];
    for (int i = 0; i < frame_count; ++i) {
        const auto result = encoder.Encode(silence.data(), silence.size(),
                                           packet, sizeof(packet));
        assert(result.encoded_bytes > 0);
        auto page = muxer.Append({packet, static_cast<size_t>(result.encoded_bytes)},
                                 false);
        out.insert(out.end(), page.begin(), page.end());
    }
    return out;
}


// N8 cut14 helper block (orig 4561)
// 从 "n=<样本数>" 假引擎文本取样本数。
inline static size_t SamplesFromFakeText(const std::string& text) {
    assert(text.size() > 2 && text.substr(0, 2) == "n=");
    return static_cast<size_t>(std::stoull(text.substr(2)));
}


// N8 cut14 helper block (orig 4973)
// OpenClipboard 对其他进程的瞬时占用（剪贴板监听器/IME 等）会短暂失败，
// Win32 官方建议重试。带重试的打开：最多 ~500ms，仍失败返回 false。
inline bool VaultOpenClipboardWithRetry() {
    for (int attempt = 0; attempt < 25; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(20);
    }
    return false;
}


// N8 cut14 helper block (orig 4983)
// 一次打开写入多个 HGLOBAL 格式（布置“用户剪贴板”内容；EmptyClipboard 清场）。
inline void VaultSetClipboard(const std::vector<std::pair<UINT, std::vector<BYTE>>>& items) {
    assert(VaultOpenClipboardWithRetry());
    EmptyClipboard();
    for (const auto& item : items) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, item.second.size());
        assert(memory != nullptr);
        void* ptr = GlobalLock(memory);
        assert(ptr != nullptr);
        memcpy(ptr, item.second.data(), item.second.size());
        GlobalUnlock(memory);
        assert(SetClipboardData(item.first, memory));
    }
    CloseClipboard();
}


// N8 cut14 helper block (orig 4999)
// 读回单个格式的字节（GetClipboardData 须在剪贴板打开态，返回前拷出）。
inline std::vector<BYTE> VaultGetBytes(UINT format) {
    std::vector<BYTE> out;
    if (OpenClipboard(nullptr)) {
        if (HANDLE handle = GetClipboardData(format)) {
            if (const SIZE_T size = GlobalSize(handle)) {
                if (void* ptr = GlobalLock(handle)) {
                    out.assign(static_cast<const BYTE*>(ptr),
                               static_cast<const BYTE*>(ptr) + size);
                    GlobalUnlock(handle);
                }
            }
        }
        CloseClipboard();
    }
    return out;
}


// N8 cut14 helper block (orig 5017)
inline std::vector<BYTE> VaultBytesOf(const std::wstring& text) {
    // 含 NUL 终止符：剪贴板 CF_UNICODETEXT 数据系统按终止符结尾规范化，
    // 布置与读回的字节口径必须一致（都含终止符）。
    return std::vector<BYTE>(
        reinterpret_cast<const BYTE*>(text.c_str()),
        reinterpret_cast<const BYTE*>(text.c_str()) + (text.size() + 1) * sizeof(wchar_t));
}


// N8 cut14 helper block (orig 5189)
inline ModelFileSpec MakeSpecFromBody(const std::string& body, std::vector<std::string> urls) {
    ModelFileSpec spec;
    spec.rel_path = "test/file.bin";
    spec.bytes = body.size();
    spec.sha256 = TestSha256Hex(body);
    spec.urls = std::move(urls);
    return spec;
}


// N8 cut14 helper block (orig 5371)
inline std::filesystem::path MakeTempDir(const char* name) {
    static std::atomic<int> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("voicestick_model_dl_" + std::to_string(counter.fetch_add(1)) + "_" + name);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}


// N8 cut14 helper block (orig 5380)
inline std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}


// N8 cut14 helper block (orig 5386)
inline void WriteFileBytes(const std::filesystem::path& path, const std::string& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}


// N8 cut14 helper block (orig 5391)
// 失败即终止：保持「测试失败 = 进程异常终止」语义，exit code 层面可见
//（NDEBUG 下 assert 结构性失效的教训，见 Doc/Expe 五坑文档坑 1）。
inline void AbortIfFailed(int failed, const char* test_name) {
    if (failed > 0) {
        std::fprintf(stderr, "FAIL %s: %d assertion(s) failed\n", test_name, failed);
        std::abort();
    }
    std::printf(">> %s OK\n", test_name);
}


// N8 cut14 helper block (orig 6187)
// ===== 跨端契约 fixtures（tests/contract，规格 Doc/Ref/protocol.md）=====
// 黄金字节由 tests/contract/generate_fixtures.py 独立构造（不从实现反推）；本测试
// 用 Windows 解析器/构建器对拍期望。键序不构成契约，control 组比对对象语义；
// expect 只取两端公共字段（单端缺口清单见 tests/contract/README.md）。

inline static std::vector<std::uint8_t> ContractUnhex(const std::string& hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size();) {
        if (std::isspace(static_cast<unsigned char>(hex[i]))) { ++i; continue; }
        if (i + 1 >= hex.size()) return {};
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        i += 2;
    }
    return out;
}


// N8 cut14 helper block (orig 6213)
inline static std::string ContractHexStr(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}


// N8 cut14 helper block (orig 6224)
inline static std::string ContractJsonStr(const cJSON* item) {
    return (item && item->valuestring) ? item->valuestring : std::string();
}


// N8 cut14 helper block (orig 6228)
inline static std::uint32_t ContractJsonU32(const cJSON* item) {
    return item ? static_cast<std::uint32_t>(item->valuedouble) : 0;
}


// N8 cut14 helper block (orig 6232)
// control 构建器分发（args → 本端 payload）。两端缺一边的构建器不入公共样本。
inline static std::optional<ByteVector> ContractBuildControl(const std::string& kind,
                                                      const cJSON* args) {
    auto s = [&](const char* k) {
        return ContractJsonStr(cJSON_GetObjectItemCaseSensitive(args, k));
    };
    if (kind == "ui_state") {
        return BleProtocol::UiStatePayload(s("state"), s("text"));
    }
    if (kind == "interaction_mode") {
        return BleProtocol::InteractionModePayload(s("mode"));
    }
    if (kind == "show_imu_debug") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::ShowImuDebugPayload(cJSON_IsTrue(v));
    }
    if (kind == "imu_wake_sensitivity") {
        return BleProtocol::ImuWakeSensitivityPayload(
            cJSON_GetObjectItemCaseSensitive(args, "threshold")->valueint);
    }
    if (kind == "tap_enabled") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::TapEnabledPayload(cJSON_IsTrue(v));
    }
    if (kind == "tap_sensitivity") {
        return BleProtocol::TapSensitivityPayload(
            cJSON_GetObjectItemCaseSensitive(args, "level")->valueint);
    }
    if (kind == "encoder_led_color") {
        return BleProtocol::EncoderLedColorPayload(s("color"));
    }
    if (kind == "encoder_recording_gate") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::EncoderRecordingGatePayload(cJSON_IsTrue(v));
    }
    if (kind == "gateway_keymap_set") {
        return BleProtocol::GatewayKeymapSetPayload(s("key"), s("route") == "software");
    }
    if (kind == "gateway_target_info") {
        return BleProtocol::GatewayTargetInfoPayload(s("name"));
    }
    if (kind == "air_mouse_enabled") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::AirMouseEnabledPayload(cJSON_IsTrue(v));
    }
    if (kind == "usb_auto_off") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::UsbAutoOffPayload(cJSON_IsTrue(v));
    }
    if (kind == "battery_status_request") {
        return BleProtocol::BatteryStatusRequestPayload();
    }
    if (kind == "remote_button") {
        return BleProtocol::RemoteButtonPayload(
            s("action"), s("button"), s("source"),
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "request_id")));
    }
    if (kind == "power_log_dump") {
        return BleProtocol::PowerLogDumpPayload(
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "offset")),
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "max")));
    }
    if (kind == "power_log_clear") {
        return BleProtocol::PowerLogClearPayload();
    }
    return std::nullopt;
}


// N8 cut14 helper block (orig 6300)
inline static std::optional<ByteVector> ContractBuildOtaControl(const std::string& kind,
                                                         const cJSON* args) {
    auto u32 = [&](const char* k) {
        return ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, k));
    };
    if (kind == "ota_begin") {
        return BleProtocol::OtaBeginPayload(u32("image_size"), u32("transfer_id"));
    }
    if (kind == "ota_data") {
        const auto chunk = ContractUnhex(ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(args, "chunk_hex")));
        if (chunk.empty()) return std::nullopt;
        return BleProtocol::OtaDataPayload(u32("transfer_id"), u32("offset"), chunk);
    }
    if (kind == "ota_end") {
        return BleProtocol::OtaEndPayload(u32("transfer_id"), u32("image_size"));
    }
    if (kind == "ota_abort") {
        return BleProtocol::OtaAbortPayload(u32("transfer_id"));
    }
    return std::nullopt;
}

// N8 cut14: license serial fixtures (static data, per-TU copies by design).
static const std::string kTestSerial1 =
    "048V0-GQ6JD-S7VXB-D040G-0000T-QR3RC-WZT5Q-TCNTZ-M5RZA-PH0Q8-VZPZN-Y4GQV-"
    "4H8EP-3H7MX-P8EKY-0MYKQ-Z1J6R-SBHS1-4BJHA-D7GTV-E7FXB-NSD3G-R0NJE-FYNGM-MJATM-0G";
static const std::string kTestSerial2 =
    "09XM4-63ZJ4-HJKKF-ZZW10-0000H-P1J9M-YA462-E9WSF-D3HE0-6KCZR-XXT6V-2XQR1-"
    "9254S-SVXB5-Y39VQ-GNR34-T6ZP8-918AT-RJM1J-8XBBT-XPVR9-1XQ88-75RGX-D9B6Z-M56XM-3R";
static const std::string kTestSerial3 =
    "070K0-W9PTN-ERNPB-C041G-00008-A5DQY-XFK9E-B1S5Z-025HR-C2B9N-K9BK9-QMR5G-"
    "E7650-GT39D-V9B0M-FH46M-Q05PZ-CBFPS-80QFK-J5F00-N3XW5-AVD6Q-AY1HD-AE2T7-1KY18-08";
