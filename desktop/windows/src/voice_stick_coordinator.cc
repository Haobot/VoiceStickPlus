#include "voice_stick_coordinator.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace voicestick {

namespace {

void LogCoordinatorLine(const std::string& message) {
    LogCoordinator(message);
}

} // namespace

VoiceStickCoordinator::VoiceStickCoordinator(AppConfig config,
                                             std::unique_ptr<BleCentral> ble,
                                             std::unique_ptr<AsrClient> asr,
                                             VoiceStickUi* ui,
                                             InputInjector* input_injector)
    : config_(std::move(config)),
      ble_(std::move(ble)),
      asr_(std::move(asr)),
      ui_(ui),
      input_injector_(input_injector),
      debug_audio_recorder_(config_.debug_audio_cache, config_.debug_audio_directory),
      paired_device_ids_(config_.paired_device_ids) {}

void VoiceStickCoordinator::Start() {
    ble_->on_connection_change = [this](std::vector<ConnectedDevice> devices) {
        ui_->SetConnectedDevices(devices);
        CancelActiveCycleIfDeviceDisconnected();
        ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
    };
    ble_->on_connection_error = [this](std::string device_id, std::string message) {
        ui_->SetPairingError(device_id, message);
    };
    ble_->on_state_event = [this](std::string device_id, StateEvent event) {
        HandleStateEvent(event, device_id);
    };
    ble_->on_audio_frame = [this](std::string device_id, AudioFrame frame) {
        HandleAudioFrame(frame, device_id);
    };
    ConfigureAsrCallbacks();
    ble_->Start();
}

void VoiceStickCoordinator::UpdateConfig(AppConfig config) {
    const bool was_recognizing = asr_started_ || active_session_id_.has_value() ||
                                 !pending_paste_state_.IsIdle();
    if (was_recognizing) {
        asr_->Cancel();
        active_session_id_.reset();
        pending_paste_state_ = {};
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("config_update_cancel");
    }

    config_ = std::move(config);
    debug_audio_recorder_ = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    if (paired_device_ids_ != config_.paired_device_ids) {
        paired_device_ids_ = config_.paired_device_ids;
        ui_->SetPairedDeviceIds(paired_device_ids_);
        ble_->UpdatePairedDeviceIds(paired_device_ids_);
    }
}

void VoiceStickCoordinator::ReconnectPairedDevices() {
    ble_->UpdatePairedDeviceIds(paired_device_ids_);
}

void VoiceStickCoordinator::ConnectPairedDevice(const std::string& device_id,
                                                std::uint64_t bluetooth_address,
                                                BluetoothAddressKind address_kind,
                                                const std::string& name) {
    ble_->ConnectPairedDevice(device_id, bluetooth_address, address_kind, name);
}

void VoiceStickCoordinator::CancelPendingConnect(const std::string& device_id) {
    ble_->CancelPendingConnect(device_id);
}

void VoiceStickCoordinator::ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids) {
    paired_device_ids_ = device_ids;
    config_.paired_device_ids = device_ids;
    ui_->SetPairedDeviceIds(paired_device_ids_);
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
}

void VoiceStickCoordinator::RemovePairedDevice(const std::string& device_id) {
    auto it = std::find(paired_device_ids_.begin(), paired_device_ids_.end(), device_id);
    if (it == paired_device_ids_.end()) return;
    paired_device_ids_.erase(it);
    config_.paired_device_ids = paired_device_ids_;
    if (active_device_id_.has_value() && *active_device_id_ == device_id) {
        // The active recording cycle was tied to the device we just forgot;
        // reset transient session state so a stale frame can't run the rest
        // of the pipeline against a torn-down session.
        asr_->Cancel();
        debug_audio_recorder_.Discard();
        active_session_id_.reset();
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        EnterReady("forget_active_device");
    }
    LogCoordinatorLine("forget paired device VS-" + device_id);
    ui_->SetPairedDeviceIds(paired_device_ids_);
    ble_->UpdatePairedDeviceIds(paired_device_ids_);
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation() {
    return RestoreLastInputConfirmation(last_recoverable_device_id_);
}

void VoiceStickCoordinator::ConfigureAsrCallbacks() {
    asr_->on_partial = [this](std::string text) {
        ui_->ShowPartial(text);
        SendUiStateForActiveDevice("thinking", text);
    };
    asr_->on_final = [this](std::string text) {
        FinishWithFinalText(text);
    };
    asr_->on_error = [this](std::string message) {
        FinishWithAsrError(message);
    };
}

void VoiceStickCoordinator::HandleStateEvent(const StateEvent& event, const std::string& device_id) {
    if (event.event == "device_info") {
        ui_->SetDeviceInfo(DeviceInfo{device_id, event.hardware, event.firmware_version});
    } else if (event.event == "button_down") {
        HandleButtonDown(event, device_id);
    } else if (event.event == "button_up") {
        HandleButtonUp(event, device_id);
    }
}

void VoiceStickCoordinator::HandleButtonDown(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        HandlePrimaryButtonDown(event.session_id, device_id);
    }
}

void VoiceStickCoordinator::HandleButtonUp(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        HandlePrimaryButtonUp(device_id);
    } else if (event.button == "secondary") {
        CancelPendingPaste(device_id);
    }
}

void VoiceStickCoordinator::HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                       const std::string& device_id) {
    if (HandleFrontButtonDuringPendingPaste(device_id)) return;
    if (IsWaitingForFinalText()) {
        RefreshDeviceUiState(device_id);
        return;
    }
    if (active_device_id_.has_value()) {
        RefreshDeviceUiState(device_id);
        return;
    }
    if (!session_id.has_value()) return;

    {
        std::lock_guard lock(audio_mutex_);
        active_session_id_ = session_id;
        active_device_id_ = device_id;
        active_session_started_at_ = std::chrono::steady_clock::now();
        received_audio_frames_ = 0;
        last_audio_seq_.reset();
        buffered_ogg_chunks_.clear();
        asr_started_ = false;
        sent_final_audio_chunk_ = false;
        pasted_final_text_ = false;
        pending_paste_state_ = {};
        is_showing_asr_error_ = false;
        ogg_muxer_.Reset();
        debug_audio_recorder_.Start(device_id, session_id);
        SetSessionState(SessionState::kRecording, "primary_down");
    }
    ui_->ShowListening();
    SendUiStateForActiveDevice("recording");
}

void VoiceStickCoordinator::HandlePrimaryButtonUp(const std::string& device_id) {
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || active_device_id_ != device_id) return;
    const double duration = CurrentRecordingDurationSeconds();
    active_session_id_.reset();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
    } else if (received_audio_frames_ == 0) {
        FinishWithAsrError("No audio frames from device");
    } else {
        SendFinalOggChunkIfNeeded(duration);
    }
}

void VoiceStickCoordinator::HandleAudioFrame(const AudioFrame& frame, const std::string& device_id) {
    const auto t0 = std::chrono::steady_clock::now();
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || frame.session_id != *active_session_id_ || active_device_id_ != device_id) {
        return;
    }
    if (frame.IsEnd() && frame.payload.empty()) {
        SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
        return;
    }
    if (frame.payload.empty()) return;
    if (last_audio_seq_.has_value() && frame.seq != *last_audio_seq_ + 1) {
        LogCoordinatorLine("audio seq gap dev=VS-" + device_id +
                           " session=" + std::to_string(*active_session_id_) +
                           " expected=" + std::to_string(*last_audio_seq_ + 1) +
                           " got=" + std::to_string(frame.seq));
    }
    last_audio_seq_ = frame.seq;
    ++received_audio_frames_;
    auto ogg_chunk = ogg_muxer_.Append(frame.payload, frame.IsEnd());
    debug_audio_recorder_.Append(ogg_chunk);
    SendOrBufferOggChunk(ogg_chunk, frame.IsEnd(), CurrentRecordingDurationSeconds() >= kMinimumRecordingDurationSeconds);
    if (frame.IsEnd()) {
        sent_final_audio_chunk_ = true;
        active_session_id_.reset();
        debug_audio_recorder_.Finish();
        EnterFinalizing("audio_end");
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (elapsed_us > 1000) {
        LogCoordinatorLine("audio frame slow dev=VS-" + device_id +
                           " seq=" + std::to_string(frame.seq) +
                           " elapsed_us=" + std::to_string(elapsed_us));
    }
}

void VoiceStickCoordinator::SendFinalOggChunkIfNeeded(double recording_duration_seconds) {
    if (sent_final_audio_chunk_) return;
    sent_final_audio_chunk_ = true;
    if (!asr_started_ && recording_duration_seconds < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
        return;
    }
    auto final_chunk = ogg_muxer_.Finish();
    debug_audio_recorder_.Append(final_chunk);
    debug_audio_recorder_.Finish();
    active_session_id_.reset();
    SendOrBufferOggChunk(final_chunk, true, true);
    EnterFinalizing("final_audio_sent");
}

void VoiceStickCoordinator::SendOrBufferOggChunk(const ByteVector& chunk, bool is_last, bool can_start_asr) {
    if (asr_started_) {
        asr_->SendOggOpusChunk(chunk, is_last);
        return;
    }
    buffered_ogg_chunks_.push_back(chunk);
    if (can_start_asr && !StartAsrAndFlushBufferedChunks(is_last)) {
        FinishWithAsrError("Failed to start ASR");
    }
}

bool VoiceStickCoordinator::StartAsrAndFlushBufferedChunks(bool last_chunk_is_final) {
    if (asr_started_) return true;
    if (!asr_->Start()) {
        buffered_ogg_chunks_.clear();
        return false;
    }
    asr_started_ = true;
    for (std::size_t i = 0; i < buffered_ogg_chunks_.size(); ++i) {
        const bool is_last = (i + 1 == buffered_ogg_chunks_.size()) && last_chunk_is_final;
        asr_->SendOggOpusChunk(buffered_ogg_chunks_[i], is_last);
    }
    buffered_ogg_chunks_.clear();
    return true;
}

void VoiceStickCoordinator::CancelShortRecording() {
    buffered_ogg_chunks_.clear();
    asr_->Cancel();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterReady("short_recording");
}

void VoiceStickCoordinator::FinishWithFinalText(const std::string& text) {
    if (pasted_final_text_) return;
    pasted_final_text_ = true;
    if (text.empty()) {
        pending_paste_state_ = {};
        ui_->ShowFinalCountdown("No speech", [this] {
            FinishRecognitionCycle();
            EnterReady("empty_final_done", false);
        });
        return;
    }
    last_recoverable_text_ = text;
    last_recoverable_device_id_ = active_device_id_;
    ui_->SetHasRecoverableInput(true);
    EnterPendingConfirmation(text, "asr_final");
}

void VoiceStickCoordinator::FinishWithAsrError(const std::string& message) {
    asr_->Cancel();
    pending_paste_state_ = {};
    active_session_id_.reset();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterError(message, "asr_error");
}

void VoiceStickCoordinator::RecoverFromAsrError(bool hide_overlay) {
    if (!is_showing_asr_error_) return;
    is_showing_asr_error_ = false;
    EnterReady("error_recovered", hide_overlay);
}

void VoiceStickCoordinator::CommitPendingPaste(const std::string& text) {
    if (pending_paste_state_.text == text) CompletePendingPaste(text);
}

void VoiceStickCoordinator::CompletePendingPaste(const std::string& text) {
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    input_injector_->Paste(text, config_.auto_enter);
    EnterReady("paste_complete", false);
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation(std::optional<std::string> device_id) {
    if (!pending_paste_state_.IsIdle() || active_session_id_.has_value() || !last_recoverable_text_.has_value()) {
        return false;
    }
    active_device_id_ = std::move(device_id);
    EnterPausedConfirmation(*last_recoverable_text_, "restore_last_input");
    return true;
}

bool VoiceStickCoordinator::HandleFrontButtonDuringPendingPaste(const std::string& device_id) {
    if (pending_paste_state_.IsIdle()) return false;
    if (active_device_id_ != device_id) return true;
    if (pending_paste_state_.kind == PendingPasteKind::kWaitingToPaste) {
        EnterPausedConfirmation(pending_paste_state_.text, "pause_pending_paste");
        return true;
    }
    ui_->HideOverlay([this, text = pending_paste_state_.text] { CommitPendingPaste(text); });
    return true;
}

void VoiceStickCoordinator::CancelPendingPaste(const std::string& device_id) {
    if (active_session_id_.has_value()) return;
    if (IsWaitingForFinalText()) {
        if (active_device_id_ == device_id) CancelRecognitionInProgress();
        else RefreshDeviceUiState(device_id);
        return;
    }
    if (pending_paste_state_.IsIdle()) {
        RestoreLastInputConfirmation(device_id);
        return;
    }
    if (active_device_id_ != device_id) return;
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_pending_paste");
}

void VoiceStickCoordinator::CancelRecognitionInProgress() {
    active_session_id_.reset();
    asr_->Cancel();
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_recognition");
}

void VoiceStickCoordinator::CancelActiveCycleIfDeviceDisconnected() {
    if (active_device_id_.has_value() && !ble_->IsConnected(*active_device_id_)) {
        asr_->Cancel();
        pending_paste_state_ = {};
        active_session_id_.reset();
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("device_disconnected");
    }
}

void VoiceStickCoordinator::FinishRecognitionCycle() {
    asr_started_ = false;
    sent_final_audio_chunk_ = false;
    pasted_final_text_ = false;
    buffered_ogg_chunks_.clear();
}

bool VoiceStickCoordinator::IsWaitingForFinalText() const {
    return session_state_ == SessionState::kFinalizing;
}

void VoiceStickCoordinator::SetSessionState(SessionState state, std::string_view reason) {
    if (session_state_ == state) return;
    auto state_name = [](SessionState value) {
        switch (value) {
        case SessionState::kReady: return "ready";
        case SessionState::kRecording: return "recording";
        case SessionState::kFinalizing: return "finalizing";
        case SessionState::kPendingConfirmation: return "pending_confirmation";
        case SessionState::kPausedConfirmation: return "paused_confirmation";
        case SessionState::kError: return "error";
        }
        return "unknown";
    };
    LogCoordinatorLine("state " + std::string(state_name(session_state_)) +
                       " -> " + state_name(state) +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    session_state_ = state;
}

void VoiceStickCoordinator::EnterReady(std::string_view reason, bool hide_overlay) {
    SetSessionState(SessionState::kReady, reason);
    ui_->SetStatus("Ready");
    SendUiStateForActiveDevice("ready");
    if (hide_overlay) ui_->HideOverlay();
    active_device_id_.reset();
}

void VoiceStickCoordinator::EnterFinalizing(std::string_view reason) {
    SetSessionState(SessionState::kFinalizing, reason);
    ui_->SetStatus("Processing");
    SendUiStateForActiveDevice("thinking");
}

void VoiceStickCoordinator::EnterPendingConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kWaitingToPaste, text};
    SetSessionState(SessionState::kPendingConfirmation, reason);
    ui_->ShowFinalCountdown(text, [this, text] { CommitPendingPaste(text); });
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterPausedConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kPaused, text};
    SetSessionState(SessionState::kPausedConfirmation, reason);
    ui_->ShowPausedFinal(text);
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterError(const std::string& message, std::string_view reason) {
    is_showing_asr_error_ = true;
    SetSessionState(SessionState::kError, reason);
    SendUiStateForActiveDevice("error", message);
    ui_->ShowError(message, [this] { RecoverFromAsrError(false); });
}

void VoiceStickCoordinator::RefreshDeviceUiState(const std::string& device_id) {
    switch (session_state_) {
    case SessionState::kRecording:
        ble_->SendUiState(active_device_id_ == device_id ? "recording" : "ready", "", device_id);
        break;
    case SessionState::kFinalizing:
        ble_->SendUiState(active_device_id_ == device_id ? "thinking" : "ready", "", device_id);
        break;
    case SessionState::kPendingConfirmation:
    case SessionState::kPausedConfirmation:
        if (active_device_id_ == device_id) {
            ble_->SendUiState("pending_confirmation", pending_paste_state_.text, device_id);
        } else {
            ble_->SendUiState("ready", "", device_id);
        }
        break;
    case SessionState::kError:
        ble_->SendUiState(active_device_id_ == device_id ? "error" : "ready", "", device_id);
        break;
    case SessionState::kReady:
        ble_->SendUiState("ready", "", device_id);
        break;
    }
}

void VoiceStickCoordinator::SendUiStateForActiveDevice(const std::string& state, const std::string& text) {
    ble_->SendUiState(state, text, active_device_id_);
}

double VoiceStickCoordinator::CurrentRecordingDurationSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - active_session_started_at_).count();
}

} // namespace voicestick
