#include "voice_stick_coordinator.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace voicestick {

namespace {

void LogCoordinatorLine(const std::string& message) {
    LogCoordinator(message);
}

bool IsFirmwareManifestCompatible(const DeviceFirmwareInfo& info, const FirmwareManifest& manifest) {
    return IsFirmwareHardwareCompatible(info.hardware, info.current_version, manifest.hardware);
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
      paired_device_ids_(config_.paired_device_ids) {
    for (const auto& entry : config_.paired_devices) {
        if (entry.device_id.empty()) continue;
        auto& info = firmware_info_by_device_id_[entry.device_id];
        info.hardware = entry.hardware;
        info.current_version = entry.firmware_version;
    }
}

VoiceStickCoordinator::~VoiceStickCoordinator() {
    Shutdown();
    alive_->store(false);
    if (firmware_manifest_thread_.joinable()) {
        firmware_manifest_thread_.join();
    }
}

void VoiceStickCoordinator::Start() {
    ble_->on_connection_change = [this](std::vector<ConnectedDevice> devices) {
        ui_->SetConnectedDevices(devices);
        CancelActiveCycleIfDeviceDisconnected();
        RefreshFirmwareAvailability();
        ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
        ble_->SendInteractionMode(config_.interaction_mode, std::nullopt);
    };
    ble_->on_connection_error = [this](std::string device_id, std::string message) {
        ui_->SetPairingError(device_id, message);
    };
    ble_->on_scan_error = [this](std::string message) {
        LogCoordinatorLine("BLE scan error: " + message);
        ui_->SetStatus("Turn on Bluetooth");
    };
    ble_->on_state_event = [this](std::string device_id, StateEvent event) {
        HandleStateEvent(event, device_id);
    };
    ble_->on_audio_frame = [this](std::string device_id, AudioFrame frame) {
        HandleAudioFrame(frame, device_id);
    };
    ConfigureAsrCallbacks();
    ble_->Start();
    CheckFirmwareUpdatesIfNeeded(false, false);
    std::thread([this, alive = alive_] {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::hours(1));
            if (!alive->load()) return;
            CheckFirmwareUpdatesIfNeeded(false, false);
        }
    }).detach();
}

void VoiceStickCoordinator::Shutdown() {
    asr_->Cancel();
    active_session_id_.reset();
    active_device_id_.reset();
    active_session_started_at_ = {};
    pending_paste_state_ = {};
    debug_audio_recorder_.Discard();
    ble_->Shutdown();
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
    ble_->SendInteractionMode(config_.interaction_mode, std::nullopt);
    debug_audio_recorder_ = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    if (paired_device_ids_ != config_.paired_device_ids) {
        paired_device_ids_ = config_.paired_device_ids;
        ui_->SetPairedDeviceIds(paired_device_ids_);
        ble_->UpdatePairedDeviceIds(paired_device_ids_);
        CheckFirmwareUpdatesIfNeeded(false, false);
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
    for (const auto& entry : config_.paired_devices) {
        if (std::find(paired_device_ids_.begin(), paired_device_ids_.end(), entry.device_id) == paired_device_ids_.end()) {
            continue;
        }
        auto& info = firmware_info_by_device_id_[entry.device_id];
        if (info.hardware.empty()) info.hardware = entry.hardware;
        if (info.current_version.empty()) info.current_version = entry.firmware_version;
    }
    CheckFirmwareUpdatesIfNeeded(false, false);
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

void VoiceStickCoordinator::CheckFirmwareUpdatesNow() {
    CheckFirmwareUpdatesIfNeeded(true, true);
}

void VoiceStickCoordinator::CheckFirmwareAfterPairing(const std::string& device_id) {
    {
        std::lock_guard lock(firmware_mutex_);
        pending_firmware_update_prompt_device_ids_.insert(device_id);
    }
    CheckFirmwareUpdatesIfNeeded(false, false);
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::UpdateFirmwareFromLatest(
    const std::string& device_id,
    std::function<void(FirmwareUpdateProgress)> progress,
    std::function<void(bool, std::string)> completion) {
    std::optional<FirmwareManifest> manifest;
    {
        std::lock_guard lock(firmware_mutex_);
        manifest = latest_firmware_manifest_;
    }
    if (!manifest.has_value()) {
        completion(false, "Firmware update manifest is not loaded yet.");
        return;
    }

    auto client = firmware_manifest_client_;
    std::thread([this, alive = alive_, client = std::move(client), device_id, manifest = std::move(*manifest),
                 progress = std::move(progress), completion = std::move(completion)]() mutable {
        std::string error;
        auto image = client.DownloadOtaSync(manifest, error);
        if (!alive->load()) return;
        if (!image.has_value()) {
            completion(false, error.empty() ? "Failed to download firmware." : error);
            return;
        }
        ble_->UpdateFirmware(std::move(*image), device_id, std::move(progress), std::move(completion));
    }).detach();
}

void VoiceStickCoordinator::CancelFirmwareUpdate() {
    ble_->CancelFirmwareUpdate();
}

void VoiceStickCoordinator::ConfigureAsrCallbacks() {
    asr_->on_partial = [this](std::string text) {
        ui_->ShowPartial(text, active_device_id_);
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
        UpdateDeviceFirmwareInfo(event, device_id);
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
    ui_->ShowListening(active_device_id_);
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
        ui_->ShowFinalCountdown("No speech", active_device_id_, [this] {
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

void VoiceStickCoordinator::UpdateDeviceFirmwareInfo(const StateEvent& event, const std::string& device_id) {
    std::string hardware_to_save;
    std::string version_to_save;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& info = firmware_info_by_device_id_[device_id];
        if (!event.hardware.empty()) {
            info.hardware = event.hardware;
            hardware_to_save = event.hardware;
        }
        if (!event.firmware_version.empty()) {
            info.current_version = event.firmware_version;
            version_to_save = event.firmware_version;
        }
        info.error_message.clear();
    }
    if (!hardware_to_save.empty() || !version_to_save.empty()) {
        config_.SavePairedDeviceInfo(device_id, hardware_to_save, version_to_save);
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::CheckFirmwareUpdatesIfNeeded(bool force, bool show_errors) {
    if (paired_device_ids_.empty() && !force) return;
    {
        std::lock_guard lock(firmware_mutex_);
        if (firmware_manifest_check_in_flight_) return;
        if (!force && has_last_firmware_manifest_check_at_ &&
            std::chrono::steady_clock::now() - last_firmware_manifest_check_at_ < kFirmwareManifestCacheDuration) {
            // Use the cached manifest to refresh any newly connected device info.
        } else {
            firmware_manifest_check_in_flight_ = true;
            SetFirmwareChecking(true);
            if (firmware_manifest_thread_.joinable()) {
                firmware_manifest_thread_.join();
            }
            auto alive = alive_;
            firmware_manifest_thread_ = std::thread([this, alive, show_errors] {
                std::string error;
                auto manifest = firmware_manifest_client_.FetchManifestSync(error);
                if (!alive->load()) return;
                {
                    std::lock_guard callback_lock(firmware_mutex_);
                    firmware_manifest_check_in_flight_ = false;
                    for (auto& [_, info] : firmware_info_by_device_id_) {
                        info.is_checking = false;
                    }
                    if (manifest.has_value()) {
                        LogCoordinatorLine("firmware manifest version=" + manifest->version +
                                           " hardware=" + manifest->hardware);
                        latest_firmware_manifest_ = std::move(manifest);
                        last_firmware_manifest_check_at_ = std::chrono::steady_clock::now();
                        has_last_firmware_manifest_check_at_ = true;
                        for (auto& [_, info] : firmware_info_by_device_id_) {
                            info.error_message.clear();
                        }
                    } else {
                        LogCoordinatorLine("firmware manifest check failed: " + error);
                        for (const auto& device_id : paired_device_ids_) {
                            if (show_errors || firmware_info_by_device_id_.contains(device_id)) {
                                firmware_info_by_device_id_[device_id].error_message = error;
                            }
                        }
                    }
                }
                RefreshFirmwareAvailability();
            });
            return;
        }
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::RefreshFirmwareAvailability() {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    std::vector<std::tuple<std::string, std::string, std::string, bool>> update_prompts;
    {
        std::lock_guard lock(firmware_mutex_);
        for (auto& [device_id, info] : firmware_info_by_device_id_) {
            info.latest_version.clear();
            info.update_available = false;
            if (!latest_firmware_manifest_.has_value() ||
                info.hardware.empty() ||
                info.current_version.empty()) {
                snapshot[device_id] = info;
                continue;
            }
            if (!IsFirmwareManifestCompatible(info, *latest_firmware_manifest_)) {
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + latest_firmware_manifest_->version +
                                   " update=false reason=hardware_mismatch manifest_hardware=" +
                                   latest_firmware_manifest_->hardware);
            } else {
                info.latest_version = latest_firmware_manifest_->version;
                info.update_available = FirmwareVersion::IsOlderThan(
                    info.current_version, latest_firmware_manifest_->version);
                if (ShouldShowFirmwareUpdatePromptAfterPairing(device_id, info)) {
                    update_prompts.emplace_back(
                        device_id,
                        info.current_version,
                        info.latest_version,
                        FirmwareVersion::IsOlderThan(
                            info.current_version,
                            AppConfig::minimum_compatible_firmware_version));
                }
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + info.latest_version +
                                   " update=" + (info.update_available ? "true" : "false"));
            }
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
    for (const auto& [device_id, current_version, latest_version, is_below_minimum] : update_prompts) {
        ui_->ShowFirmwareUpdatePrompt(device_id, current_version, latest_version, is_below_minimum);
    }
}

bool VoiceStickCoordinator::ShouldShowFirmwareUpdatePromptAfterPairing(const std::string& device_id,
                                                                       const DeviceFirmwareInfo& info) {
    if (!pending_firmware_update_prompt_device_ids_.contains(device_id) ||
        info.current_version.empty() || info.latest_version.empty()) {
        return false;
    }
    if (!info.update_available) {
        pending_firmware_update_prompt_device_ids_.erase(device_id);
        return false;
    }
    pending_firmware_update_prompt_device_ids_.erase(device_id);
    return true;
}

void VoiceStickCoordinator::SetFirmwareChecking(bool is_checking) {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    {
        for (const auto& device_id : paired_device_ids_) {
            auto& info = firmware_info_by_device_id_[device_id];
            info.is_checking = is_checking;
            if (is_checking) info.error_message.clear();
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
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
    ui_->ShowFinalCountdown(text, active_device_id_, [this, text] { CommitPendingPaste(text); });
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterPausedConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kPaused, text};
    SetSessionState(SessionState::kPausedConfirmation, reason);
    ui_->ShowPausedFinal(text, active_device_id_);
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterError(const std::string& message, std::string_view reason) {
    is_showing_asr_error_ = true;
    SetSessionState(SessionState::kError, reason);
    SendUiStateForActiveDevice("error", message);
    ui_->ShowError(message, active_device_id_, [this] { RecoverFromAsrError(false); });
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
