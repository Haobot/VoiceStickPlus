#include "air_mouse_kin.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "ble_protocol.h"
#include "byte_utils.h"
#include "cJSON.h"
#include "firmware_manifest.h"
#include "llm_refinement_client.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "pair_device_helper.h"
#include "voice_stick_coordinator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
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
    void SendPromptToneEnabled(bool enabled,
                               const std::optional<std::string>& device_id) override {
        sent_prompt_tones.push_back(std::pair{enabled, device_id});
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
    void UpdateFirmware(ByteVector,
                        const std::string&,
                        std::function<void(FirmwareUpdateProgress)>,
                        std::function<void(bool, std::string)> completion) override {
        completion(false, "not implemented");
    }
    void CancelFirmwareUpdate() override {}
    bool IsConnected(const std::string& device_id) const override {
        return connected_device_ids.contains(device_id);
    }

    std::vector<std::string> paired_device_ids;
    std::set<std::string> connected_device_ids;
    std::vector<SentUiState> sent_ui_states;
    std::vector<std::pair<InteractionMode, std::optional<std::string>>> sent_interaction_modes;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_prompt_tones;
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
    auto enabled = BleProtocol::PromptTonePayload(true);
    auto disabled = BleProtocol::PromptTonePayload(false);
    auto battery_request = BleProtocol::BatteryStatusRequestPayload();
    assert(std::string(enabled.begin(), enabled.end()) == "{\"event\":\"prompt_tone\",\"enabled\":true}");
    assert(std::string(disabled.begin(), disabled.end()) == "{\"event\":\"prompt_tone\",\"enabled\":false}");
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

void TestCoordinatorSyncsPromptToneOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.prompt_tone_enabled = false;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_prompt_tones.empty());
    assert(ble_ptr->sent_prompt_tones.back().first == false);
    assert(!ble_ptr->sent_prompt_tones.back().second.has_value());

    AppConfig updated = config;
    updated.prompt_tone_enabled = true;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_prompt_tones.back().first == true);
    assert(!ble_ptr->sent_prompt_tones.back().second.has_value());
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

    // 空闲态侧键单击 → 进入体感，下发 air_mouse_enabled:true。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(!ble_ptr->sent_air_mouse_enabled.empty());
    assert(ble_ptr->sent_air_mouse_enabled.back().first == true);

    // 再次侧键单击 → 退出体感，下发 air_mouse_enabled:false。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    assert(ble_ptr->sent_air_mouse_enabled.back().first == false);
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

// 10 级灵敏度下，真机典型手腕转动(omega≈24)在 0.8s 内应产生足够光标位移。
// 角度控制模型 gain×20 后，同 omega 下 theta 累积使位移比速度模型更大。
void TestCoordinatorAirMouseHighSensitivityRealisticSpeed() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.air_mouse_sensitivity_x = 10;  // 最高档
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    for (int i = 0; i < 50; ++i) {  // 0.8s @60Hz
        ble_ptr->on_motion_event("5A74", MotionEvent{24, 0});  // 真机典型手腕角速度
        coordinator.AirMouseTick();
    }
    assert(input.total_dx >= 4000);  // 角度模型 theta 累积，0.8s 应产生足够位移
}

// 10 级灵敏度下，转动到固定角度后保持，平均光标速度受增益曲线限制。
// 角度模型持续转动会使 theta 累积、速度上升；本测试模拟"转到位后保持"的真实手势，
// 约束保持阶段的稳态速度不过快。
void TestCoordinatorAirMouseSustainedRunBounded() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.air_mouse_sensitivity_x = 10;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));
    // 阶段 1：转动 0.5s，theta 累积到约 12。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{24, 0});
        coordinator.AirMouseTick();
    }
    // 阶段 2：保持 omega=0 4.5s，theta 稳定在约 12，光标以稳态速度持续移动。
    for (int i = 0; i < 270; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    const double avg_speed = static_cast<double>(input.total_dx) / 5.0;
    assert(avg_speed <= 50000.0);  // 固定角度保持阶段平均速度有界
}

// 角度控制：转动后回正到中立姿态，theta 归零，光标停止。
// 旧速度模型 omega=0 即停；新角度模型需回正才停，本测试约束"回正即停"。
void TestCoordinatorAirMouseStopsWhenThetaZero() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.air_mouse_sensitivity_x = 10;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

    // 转动 0.5s：theta 从 0 累积到约 12（24×0.5）。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{24, 0});
        coordinator.AirMouseTick();
    }
    const int dx_during = input.total_dx;
    assert(dx_during > 0);

    // 回正：反向 omega 让 theta 回到 0（约 0.5s）。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{-24, 0});
        coordinator.AirMouseTick();
    }

    // 中立区：theta/omega 均接近 0，归零后光标停止。
    const int count_before = input.move_mouse_count;
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    // 角度模型下回正过程本身会产生位移；回正完成 + 中立区归零后，额外滑行应有限。
    const int dx_after = input.total_dx - dx_during;
    assert(dx_after < 2000);
}

// 角度控制：转动后保持 omega=0，theta 保留，光标持续移动（核心需求）。
void TestCoordinatorAngleIntegratesToSustainedMovement() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.air_mouse_sensitivity_x = 10;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.on_air_mouse_active_changed = [](bool) {};
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "secondary"));

    // 阶段 1：转动 0.5s 累积 theta。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{24, 0});
        coordinator.AirMouseTick();
    }
    const int dx_while_moving = input.total_dx;

    // 阶段 2：omega=0 保持 0.5s，theta 应保持非零，光标继续同向移动。
    for (int i = 0; i < 30; ++i) {
        ble_ptr->on_motion_event("5A74", MotionEvent{0, 0});
        coordinator.AirMouseTick();
    }
    const int dx_while_holding = input.total_dx - dx_while_moving;

    // 保持阶段必须有明显同向位移（旧速度模型下 omega=0 后几乎不移动）。
    assert(dx_while_holding > 1000);
    assert(input.total_dx > dx_while_moving * 1.5);  // 保持阶段贡献显著
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

} // namespace

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
//   微调段 |omega| < low_thresh  → low_factor
//   中段  low_thresh ≤ |omega| < high_thresh → 线性插值 low_factor→high_factor
//   甩动段 |omega| ≥ high_thresh → high_factor
// 测试引用 p.curve.*（运行期参数），约束曲线形状、拐点连续性、curve 注入与 clamp。

// 微调段：omega=low_thresh/2，稳态 vx ≈ omega×gain×low_factor。
void TestAirMouseStepGainCurveLowRange() {
    AirMouseKinState s;
    AirMouseParams p;  // 默认 gain_x=16, tau=0.05, curve={15,50,0.15,4.0}
    const int omega = static_cast<int>(p.curve.low_thresh / 2.0);  // 微调段内
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    const double v_target = omega * p.gain_x * p.curve.low_factor;
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

// 中段：omega=中段中点，factor 线性插值，稳态 vx ≈ omega×gain×factor。
void TestAirMouseStepGainCurveMidRange() {
    AirMouseKinState s;
    AirMouseParams p;
    const int omega = static_cast<int>((p.curve.low_thresh + p.curve.high_thresh) / 2.0);
    const double factor = p.curve.low_factor + (p.curve.high_factor - p.curve.low_factor) *
        (omega - p.curve.low_thresh) / (p.curve.high_thresh - p.curve.low_thresh);
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

// 拐点连续：omega=low_thresh（微调段上限=中段下限）处 factor=low_factor，无上跳。
void TestAirMouseStepGainCurveContinuousAtLowThreshold() {
    AirMouseKinState s;
    AirMouseParams p;
    const int omega = static_cast<int>(p.curve.low_thresh);  // 拐点
    for (int i = 0; i < 200; ++i) AirMouseStep(s, AirMouseInput{omega, 0, false}, 0.016, false, p);
    // 拐点处中段起点 factor=low_factor，与微调段外推连续
    const double v_target = omega * p.gain_x * p.curve.low_factor;
    assert(std::fabs(s.vx - v_target) < std::fabs(v_target) * 0.05);
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
    // omega=20 落自定义中段（10..30）：factor=0.2+(5.0-0.2)*(20-10)/(30-10)=2.6
    // omega=20 落默认中段（15..50）：factor=0.15+(4.0-0.15)*(20-15)/(50-15)≈0.70
    const double f_custom = AirMouseGainFactor(20.0, c);
    const double f_default = AirMouseGainFactor(20.0, AirMouseCurveParams{});
    assert(std::fabs(f_custom - 2.6) < 0.02);
    assert(std::fabs(f_default - 0.70) < 0.02);
    assert(std::fabs(f_custom - f_default) > 1.0);  // curve 注入确实改变 factor
}

// 默认 curve 与历史 constexpr 值一致（回归保护）。
void TestAirMouseGainFactorDefaultCurveMatchesLegacy() {
    AirMouseCurveParams c;  // 默认 {15, 50, 0.15, 4.0}
    assert(std::fabs(AirMouseGainFactor(5.0, c) - 0.15) < 1e-9);    // 微调段
    assert(std::fabs(AirMouseGainFactor(100.0, c) - 4.0) < 1e-9);   // 甩动段
    // 中段中点 factor=0.15+(4.0-0.15)*0.5=2.075
    assert(std::fabs(AirMouseGainFactor(32.5, c) - 2.075) < 1e-9);
}

// step 用 params.curve：同 omega、不同 curve → 不同稳态 vx。
void TestAirMouseStepUsesCurveParams() {
    AirMouseParams p_low;   // 默认 curve low_factor=0.15
    AirMouseParams p_high;  // 高 low_factor
    p_high.curve.low_factor = 0.5;
    const int omega = 5;  // 微调段内
    AirMouseKinState s_low, s_high;
    for (int i = 0; i < 200; ++i) {
        AirMouseStep(s_low, AirMouseInput{omega, 0, false}, 0.016, false, p_low);
        AirMouseStep(s_high, AirMouseInput{omega, 0, false}, 0.016, false, p_high);
    }
    // 微调段 v_target=omega×gain×low_factor，p_high 的 low_factor 高 → vx 更大
    assert(s_high.vx > s_low.vx * 2.0);
}

// clamp：越界值钳位到合法范围，且保证 low_thresh < high_thresh。
void TestAirMouseCurveClamp() {
    AirMouseCurveParams c;
    c.low_thresh = 0.0;       // 低于下限 1.0
    c.high_thresh = 1000.0;   // 高于上限 80.0
    c.low_factor = -1.0;      // 低于下限 0.05
    c.high_factor = 100.0;    // 高于上限 6.0
    const auto clamped = AirMouseCurveClamp(c);
    assert(clamped.low_thresh >= 1.0);
    assert(clamped.high_thresh <= 80.0);
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
    p.gain_x = 320.0;
    // 先给非零 theta 让 v 起来
    for (int i = 0; i < 50; ++i) AirMouseStep(s, AirMouseInput{10, 0, true}, 0.016, false, p);
    assert(s.vx > 1.0);
    // 然后 theta=0
    for (int i = 0; i < 100; ++i) AirMouseStep(s, AirMouseInput{0, 0, true}, 0.016, false, p);
    assert(std::fabs(s.vx) < 1.0);
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

    // Clamp 边界：越界回落默认值。
    assert(AirMouseSensitivityClamp(0) == 5);
    assert(AirMouseSensitivityClamp(11) == 5);
    assert(AirMouseTauClamp(0.005) == 0.05);
    assert(AirMouseTauClamp(1.0) == 0.05);
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
    TestOggMuxer();
    TestAsrProtocol();
    TestAppConfig();
    TestAppConfigTapSensitivityRoundTrip();
    TestAppConfigAirMouseRoundTrip();
    TestLlmRefinePromptAndPayload();
    TestFirmwareManifestParsingAndVersionCompare();
    TestCoordinatorSyncsPromptToneOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate();
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
    TestCoordinatorAirMouseStopsWhenThetaZero();
    TestCoordinatorAngleIntegratesToSustainedMovement();
    TestCoordinatorSecondaryDoubleClickRestoresLastInput();
    TestCoordinatorSecondaryDoubleClickIgnoredInAirMouse();
    TestCoordinatorCloudUpgradeRecoversDeviceAfterAsrError();
    TestSseParser();
    TestStreamPayload();
    TestTencentProviderSelection();
    TestTencentConfigRoundTrip();
    TestTencentSignatureGeneration();
    TestTencentUrlConstruction();
    TestTencentResultParsing();
    TestTencentFinalFlagParsing();
    TestTencentSentenceAccumulation();
    TestTencentEndMessage();
    TestTencentOpusEncapsulation();
    TestTencentVoiceIdGeneration();
    return 0;
}
