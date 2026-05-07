#pragma once

#include "app_config.h"
#include "ble_protocol.h"
#include "debug_audio_recorder.h"
#include "ogg_opus_muxer.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

struct ConnectedDevice {
    std::string id;
    std::string name;
};

struct DeviceInfo {
    std::string device_id;
    std::string hardware;
    std::string firmware_version;
};

class BleCentral {
public:
    virtual ~BleCentral() = default;
    virtual void Start() = 0;
    virtual void UpdatePairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void ConnectPairedDevice(const std::string& device_id,
                                     std::uint64_t bluetooth_address,
                                     BluetoothAddressKind address_kind,
                                     const std::string& name) = 0;
    virtual void SendUiState(const std::string& state,
                               const std::string& text,
                               const std::optional<std::string>& device_id) = 0;
    virtual bool IsConnected(const std::string& device_id) const = 0;
    virtual void CancelPendingConnect(const std::string& device_id) {}

    std::function<void(std::vector<ConnectedDevice>)> on_connection_change;
    std::function<void(std::string, std::string)> on_connection_error;
    std::function<void(std::string, StateEvent)> on_state_event;
    std::function<void(std::string, AudioFrame)> on_audio_frame;
};

class AsrClient {
public:
    virtual ~AsrClient() = default;
    virtual bool Start() = 0;
    virtual void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) = 0;
    virtual void Cancel() = 0;

    std::function<void(std::string)> on_partial;
    std::function<void(std::string)> on_final;
    std::function<void(std::string)> on_error;
    std::function<void(std::string)> on_upgrade_url;
};

class VoiceStickUi {
public:
    virtual ~VoiceStickUi() = default;
    virtual void SetStatus(const std::string& status) = 0;
    virtual void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) = 0;
    virtual void SetDeviceInfo(const DeviceInfo& info) = 0;
    virtual void SetPairingError(const std::string& device_id, const std::string& message) = 0;
    virtual void SetPairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void SetHasRecoverableInput(bool has_recoverable_input) = 0;
    virtual void ShowListening() = 0;
    virtual void ShowPartial(const std::string& text) = 0;
    virtual void ShowFinalCountdown(const std::string& text, std::function<void()> on_complete) = 0;
    virtual void ShowPausedFinal(const std::string& text) = 0;
    virtual void ShowError(const std::string& text, std::function<void()> on_complete) = 0;
    virtual void HideOverlay(std::function<void()> on_hidden = {}) = 0;
};

class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual void Paste(const std::string& text, bool press_enter) = 0;
};

class VoiceStickCoordinator {
public:
    VoiceStickCoordinator(AppConfig config,
                          std::unique_ptr<BleCentral> ble,
                          std::unique_ptr<AsrClient> asr,
                          VoiceStickUi* ui,
                          InputInjector* input_injector);

    void Start();
    void UpdateConfig(AppConfig config);
    void ReconnectPairedDevices();
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name);
    void ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids);
    void RemovePairedDevice(const std::string& device_id);
    void CancelPendingConnect(const std::string& device_id);
    bool RestoreLastInputConfirmation();

private:
    enum class PendingPasteKind {
        kIdle,
        kWaitingToPaste,
        kPaused,
    };

    enum class SessionState {
        kReady,
        kRecording,
        kFinalizing,
        kPendingConfirmation,
        kPausedConfirmation,
        kError,
    };

    struct PendingPasteState {
        PendingPasteKind kind = PendingPasteKind::kIdle;
        std::string text;

        bool IsIdle() const { return kind == PendingPasteKind::kIdle; }
    };

    void ConfigureAsrCallbacks();
    void HandleStateEvent(const StateEvent& event, const std::string& device_id);
    void HandleButtonDown(const StateEvent& event, const std::string& device_id);
    void HandleButtonUp(const StateEvent& event, const std::string& device_id);
    void HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandlePrimaryButtonUp(const std::string& device_id);
    void HandleAudioFrame(const AudioFrame& frame, const std::string& device_id);
    void SendFinalOggChunkIfNeeded(double recording_duration_seconds);
    void SendOrBufferOggChunk(const ByteVector& chunk, bool is_last, bool can_start_asr);
    bool StartAsrAndFlushBufferedChunks(bool last_chunk_is_final);
    void CancelShortRecording();
    void FinishWithFinalText(const std::string& text);
    void FinishWithAsrError(const std::string& message);
    void RecoverFromAsrError(bool hide_overlay = true);
    void CommitPendingPaste(const std::string& text);
    void CompletePendingPaste(const std::string& text);
    bool RestoreLastInputConfirmation(std::optional<std::string> device_id);
    bool HandleFrontButtonDuringPendingPaste(const std::string& device_id);
    void CancelPendingPaste(const std::string& device_id);
    void CancelRecognitionInProgress();
    void CancelActiveCycleIfDeviceDisconnected();
    void FinishRecognitionCycle();
    bool IsWaitingForFinalText() const;
    void SetSessionState(SessionState state, std::string_view reason);
    void EnterReady(std::string_view reason, bool hide_overlay = true);
    void EnterFinalizing(std::string_view reason);
    void EnterPendingConfirmation(const std::string& text, std::string_view reason);
    void EnterPausedConfirmation(const std::string& text, std::string_view reason);
    void EnterError(const std::string& message, std::string_view reason);
    void RefreshDeviceUiState(const std::string& device_id);
    void SendUiStateForActiveDevice(const std::string& state, const std::string& text = "");
    double CurrentRecordingDurationSeconds() const;

    AppConfig config_;
    std::unique_ptr<BleCentral> ble_;
    std::unique_ptr<AsrClient> asr_;
    VoiceStickUi* ui_;
    InputInjector* input_injector_;
    std::mutex audio_mutex_;
    OggOpusMuxer ogg_muxer_{16000, 1};
    DebugAudioRecorder debug_audio_recorder_;
    SessionState session_state_ = SessionState::kReady;
    std::optional<std::uint32_t> active_session_id_;
    std::optional<std::string> active_device_id_;
    std::chrono::steady_clock::time_point active_session_started_at_;
    int received_audio_frames_ = 0;
    std::optional<std::uint32_t> last_audio_seq_;
    std::vector<ByteVector> buffered_ogg_chunks_;
    bool asr_started_ = false;
    bool sent_final_audio_chunk_ = false;
    bool pasted_final_text_ = false;
    PendingPasteState pending_paste_state_;
    std::optional<std::string> last_recoverable_text_;
    std::optional<std::string> last_recoverable_device_id_;
    std::vector<std::string> paired_device_ids_;
    bool is_showing_asr_error_ = false;
    static constexpr double kMinimumRecordingDurationSeconds = 0.5;
};

} // namespace voicestick
