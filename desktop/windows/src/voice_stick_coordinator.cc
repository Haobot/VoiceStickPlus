#include "voice_stick_coordinator.h"

#include "localization.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iterator>
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

std::string AsrStartFailureMessage(const AsrClient& asr) {
    const auto message = asr.LastStartError();
    return message.empty() ? "Failed to start ASR" : message;
}

// steady_clock 毫秒时间戳（watchdog 活动时间用，进程内自洽即可，不需要绝对 epoch）。
std::int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VoiceStickCoordinator::VoiceStickCoordinator(AppConfig config,
                                             std::unique_ptr<BleCentral> ble,
                                             std::unique_ptr<AsrClient> asr,
                                             VoiceStickUi* ui,
                                             InputInjector* input_injector,
                                             std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory,
                                             std::function<std::unique_ptr<IVirtualMicRenderer>(const IVirtualMicRenderer::Options&)> wechat_renderer_factory,
                                             std::function<std::unique_ptr<IWechatInputMethodHotkey>(const std::string&)> wechat_hotkey_factory,
                                             std::function<std::unique_ptr<IDefaultAudioDeviceController>()> wechat_device_switcher_factory,
                                             std::filesystem::path device_switch_state_path,
                                             std::chrono::milliseconds recording_hard_timeout,
                                             std::chrono::milliseconds finalizing_timeout,
                                             std::chrono::milliseconds audio_stall_timeout)
    : config_(std::move(config)),
      ble_(std::move(ble)),
      asr_(std::move(asr)),
      asr_factory_(std::move(asr_factory)),
      translator_(config_),
      refiner_(config_),
      ui_(ui),
      input_injector_(input_injector),
      debug_audio_recorder_(config_.debug_audio_cache, config_.debug_audio_directory),
      paired_device_ids_(config_.paired_device_ids),
      wechat_renderer_factory_(std::move(wechat_renderer_factory)),
      wechat_hotkey_factory_(std::move(wechat_hotkey_factory)),
      wechat_device_switcher_factory_(std::move(wechat_device_switcher_factory)),
      device_switch_state_path_(std::move(device_switch_state_path)) {
    recording_hard_timeout_ = recording_hard_timeout;
    finalizing_timeout_ = finalizing_timeout;
    audio_stall_timeout_ = audio_stall_timeout;
    for (const auto& entry : config_.paired_devices) {
        if (entry.device_id.empty()) continue;
        auto& info = firmware_info_by_device_id_[entry.device_id];
        info.hardware = entry.hardware;
        info.current_version = entry.firmware_version;
    }
    // 运行期 air_mouse 参数初值从配置加载（AirMouseTick 用，热调参面板经 UpdateAirMouseParams 改）。
    live_air_mouse_params_ = AirMouseParamsFromConfig();
}

VoiceStickCoordinator::~VoiceStickCoordinator() {
    alive_->store(false);
    Shutdown();
    if (firmware_manifest_thread_.joinable()) {
        firmware_manifest_thread_.join();
    }
}

void VoiceStickCoordinator::Start() {
    RecoverDeviceSwitchStateIfNeeded();
    ble_->on_connection_change = [this](std::vector<ConnectedDevice> devices) {
        if (is_shutdown_) return;
        connected_device_ids_.clear();
        for (const auto& dev : devices) {
            connected_device_ids_.push_back(dev.id);
        }
        ui_->SetConnectedDevices(devices);
        CancelActiveCycleIfDeviceDisconnected();
        RefreshFirmwareAvailability();
        ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
        ble_->SendInteractionMode(InteractionModeToSend(), std::nullopt);
        ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt);
        ble_->SendTapEnabled(config_.tap_to_arrow, std::nullopt);
        ble_->SendTapSensitivity(config_.tap_sensitivity, std::nullopt);
        ble_->SendImuWakeSensitivity(
            ImuWakeSensitivityThresholdLsb(config_.imu_wake_sensitivity), std::nullopt);
    };
    ble_->on_connection_error = [this](std::string device_id, std::string message) {
        if (is_shutdown_) return;
        ui_->SetPairingError(device_id, message);
    };
    ble_->on_scan_error = [this](std::string message) {
        if (is_shutdown_) return;
        LogCoordinatorLine("BLE scan error: " + message);
        ui_->SetStatus("Turn on Bluetooth");
    };
    ble_->on_state_event = [this](std::string device_id, StateEvent event) {
        if (is_shutdown_) return;
        HandleStateEvent(event, device_id);
    };
    ble_->on_audio_frame = [this](std::string device_id, AudioFrame frame) {
        if (is_shutdown_) return;
        HandleAudioFrame(frame, device_id);
    };
    ble_->on_motion_event = [this](std::string device_id, MotionEvent event) {
        if (is_shutdown_) return;
        HandleMotionEvent(event, device_id);
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
    if (is_shutdown_) return;
    is_shutdown_ = true;
    ble_->on_connection_change = nullptr;
    ble_->on_connection_error = nullptr;
    ble_->on_scan_error = nullptr;
    ble_->on_state_event = nullptr;
    ble_->on_audio_frame = nullptr;
    ble_->on_motion_event = nullptr;
    asr_->Cancel();
    for (auto& [_, cycle] : subtitle_cycles_) {
        if (cycle->asr) cycle->asr->Cancel();
        cycle->debug_audio_recorder.Discard();
    }
    subtitle_cycles_.clear();
    active_subtitle_sessions_.clear();
    ui_->HideSubtitles();
    CancelAudioEndTimeout();
    CancelStreamingRefinement();
    active_session_id_.reset();
    active_device_id_.reset();
    active_session_started_at_ = {};
    pending_paste_state_ = {};
    debug_audio_recorder_.Discard();
    ble_->Shutdown();
}

void VoiceStickCoordinator::UpdateConfig(AppConfig config) {
    const bool was_recognizing = asr_started_ || active_session_id_.has_value() ||
                                 !pending_paste_state_.IsIdle() || !subtitle_cycles_.empty();
    if (was_recognizing) {
        asr_->Cancel();
        for (auto& [_, cycle] : subtitle_cycles_) {
            if (cycle->asr) cycle->asr->Cancel();
            cycle->debug_audio_recorder.Discard();
        }
        subtitle_cycles_.clear();
        active_subtitle_sessions_.clear();
        ui_->HideSubtitles();
        active_session_id_.reset();
        pending_paste_state_ = {};
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("config_update_cancel");
    }

    config_ = std::move(config);
    live_air_mouse_params_ = AirMouseParamsFromConfig();  // config_ 变化，运行期 live 参数跟随
    translator_ = LLMTranslationClient(config_);
    refiner_ = LLMRefinementClient(config_);
    ble_->SendInteractionMode(InteractionModeToSend(), std::nullopt);
    ble_->SendShowImuDebug(config_.show_imu_debug, std::nullopt);
    ble_->SendTapEnabled(config_.tap_to_arrow, std::nullopt);
    ble_->SendTapSensitivity(config_.tap_sensitivity, std::nullopt);
    ble_->SendImuWakeSensitivity(
        ImuWakeSensitivityThresholdLsb(config_.imu_wake_sensitivity), std::nullopt);
    debug_audio_recorder_ = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    if (asr_factory_) {
        asr_ = asr_factory_(config_);
        ConfigureAsrCallbacks();
    }
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
    // forget 后设备会断开重连：清理残留体感态，避免拦截重连后的主键录音。
    {
        const bool was_active = air_mouse_active_devices_.erase(device_id) > 0;
        air_mouse_states_.erase(device_id);
        if (was_active) {
            ble_->SendAirMouseEnabled(false, device_id);
            LogCoordinatorLine("air mouse disabled on VS-" + device_id + " (forget)");
            if (on_air_mouse_active_changed) on_air_mouse_active_changed(!air_mouse_states_.empty());
        }
    }
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
    CheckFirmwareUpdatesIfNeeded(true, false);
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

void VoiceStickCoordinator::UpdateFirmwareFromFile(
    const std::string& file_path, const std::string& device_id,
    std::function<void(FirmwareUpdateProgress)> progress,
    std::function<void(bool, std::string)> completion) {
    std::ifstream f(file_path, std::ios::binary);
    if (!f) {
        completion(false, "Cannot open firmware file.");
        return;
    }
    ByteVector image((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (image.empty()) {
        completion(false, "Firmware file is empty.");
        return;
    }
    // 与底层 UpdateFirmware 的 OTA 分区上限一致（3MB），提前给友好错误。
    if (image.size() > 3 * 1024 * 1024) {
        completion(false, "Firmware file is larger than the OTA partition.");
        return;
    }
    ble_->UpdateFirmware(std::move(image), device_id,
                         std::move(progress), std::move(completion));
}

void VoiceStickCoordinator::CancelFirmwareUpdate() {
    ble_->CancelFirmwareUpdate();
}

void VoiceStickCoordinator::ConfigureAsrCallbacks() {
    asr_->on_partial = [this](std::string text) {
        TouchFinalizingWatchdog();
        ui_->ShowPartial(text, active_device_id_);
        if (ShouldSendPartialToDevice()) {
            SendUiStateForActiveDevice("thinking", text);
        }
    };
    asr_->on_segment = [this](AsrSegment segment) {
        TouchFinalizingWatchdog();
        HandleDefiniteSegment(segment);
    };
    asr_->on_final = [this](std::string text) {
        FinishWithFinalText(text);
    };
    asr_->on_error = [this](std::string message) {
        FinishWithAsrError(message);
    };
    asr_->on_upgrade_url = [this](std::string url, std::string message) {
        const auto device_id = active_device_id_;
        RecoverFromAsrError(false);
        ui_->ShowCloudUpgrade(message, url, device_id);
    };
}

void VoiceStickCoordinator::ConfigureSubtitleAsrCallbacks(SubtitleCycle* cycle) {
    if (!cycle || !cycle->asr) return;
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    cycle->asr->on_partial = [this, device_id, session_id](std::string text) {
        auto* cycle = FindSubtitleCycle(device_id, session_id);
        if (!cycle || !CanUpdateOverlayForSubtitleCycle(device_id, session_id)) return;
        ui_->ShowPartial(text, device_id);
        if (ShouldSendSubtitlePartialToDevice(cycle)) {
            ble_->SendUiState("thinking", text, device_id);
        }
    };
    cycle->asr->on_segment = [this, device_id, session_id](AsrSegment segment) {
        if (!IsActiveSubtitleCycle(device_id, session_id)) return;
        HandleSubtitleDefiniteSegment(segment, device_id);
    };
    cycle->asr->on_final = [this, device_id, session_id](std::string text) {
        FinishSubtitleCycleWithFinalText(device_id, session_id, text);
    };
    cycle->asr->on_error = [this, device_id, session_id](std::string message) {
        FinishSubtitleCycleWithError(device_id, session_id, message);
    };
    cycle->asr->on_upgrade_url = [this, device_id](std::string url, std::string message) {
        ble_->SendUiState("ready", "", device_id);
        ui_->ShowCloudUpgrade(message, url, device_id);
    };
}

void VoiceStickCoordinator::HandleStateEvent(const StateEvent& event, const std::string& device_id) {
    if (event.event == "device_info") {
        ui_->SetDeviceInfo(DeviceInfo{device_id, event.hardware, event.firmware_version});
        UpdateDeviceFirmwareInfo(event, device_id);
    } else if (event.event == "battery_status") {
        if (event.battery_level.has_value()) {
            ui_->SetDeviceBattery(device_id, event.battery_level.value(),
                                   event.battery_charging.value_or(false),
                                   event.battery_usb_powered.value_or(false));
        }
    } else if (event.event == "button_down") {
        HandleButtonDown(event, device_id);
    } else if (event.event == "button_up") {
        HandleButtonUp(event, device_id);
    } else if (event.event == "button_click") {
        HandleButtonClick(event, device_id);
    } else if (event.event == "button_double_click") {
        HandleButtonDoubleClick(event, device_id);
    } else if (event.event == "tap") {
        HandleTapEvent(event, device_id);
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
        HandleSecondaryButtonClick(device_id);
    }
}

void VoiceStickCoordinator::HandleWechatInputMethodPrimaryButtonDown(
    std::optional<std::uint32_t> session_id, const std::string& device_id) {
    // wechat_active 残留（上次 button_up/audio_end 都丢）时先停旧会话再 Start 新的，
    // 否则用户长按完全无反应。安全前提：固件 hold_to_talk 录音中再按主键走 hold_threshold
    // 分支 return（不发新 button_down），click_to_talk 录音中再按发的是 button_click，
    // 故收到 button_down 时若 wechat_active=true 必为残留，可安全 Stop + 重启。
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
    }
    if (!session_id.has_value() || *session_id == 0) {
        ble_->SendUiState("ready", "", device_id);
        return;
    }
    if (HandleFrontButtonDuringPendingPaste(device_id)) return;

    // 前台为高权限程序时 SendInput 注入必被 UIPI 丢弃，启动会话无意义且留空转残留。
    // 检测到则气泡提醒用户提权运行并跳过启动；按进程名去重，同一高权限程序本次运行只提醒一次。
    if (MaybeWarnForegroundElevated(device_id)) {
        ble_->SendUiState("ready", "", device_id);
        return;
    }

    // 首字延迟诊断锚点：button_down 到达桌面端时刻。各环节打印相对此点的累计毫秒，
    // 用于量化设备切换/WASAPI Start/固件首帧 BLE 延迟的真实耗时，定位优化目标。
    wechat_latency_anchor_ = std::chrono::steady_clock::now();
    LogCoordinatorLine("wechat latency: button_down arrived dev=VS-" + device_id);

    if (!StartWechatInputMethodSession(session_id, device_id)) {
        // 启动失败（虚拟麦克风未找到/热键发送失败）：ShowError 已在内部提示，
        // 不弹录音悬浮窗、不发 recording 状态，避免松开时浮窗残留。
        ble_->SendUiState("ready", "", device_id);
        return;
    }
    // wechat 模式同样需要 recording 硬超时兜底：button_up/audio_end 都丢（固件 drain 超时
    // 丢 audio_end + BLE 抖动 button_up 丢）时回 ready，避免永久卡 listening。
    ScheduleRecordingHardTimeout();
    // wechat 模式不弹 VoiceStick 录音悬浮窗：第三方输入法自带语音面板，
    // 弹出 VoiceStick 浮窗会遮挡且造成"松开不消失"的混乱，仅通过设备屏幕 recording 提供反馈。
    SendUiStateForActiveDevice("recording");
}

void VoiceStickCoordinator::HandleWechatInputMethodPrimaryButtonUp(
    const std::string& device_id) {
    if (!IsWechatInputMethodActive() || active_device_id_ != device_id) {
        return;
    }
    StopWechatInputMethodSession();
    EnterReady("wechat_button_up");
}

void VoiceStickCoordinator::HandleWechatInputMethodAudioFrame(
    const AudioFrame& frame, const std::string& device_id) {
    // audio_end 到达即结束整个会话（停 renderer、松热键、清 wechat_active），覆盖 button_up
    // 走 BLE notify 无 ACK 在闪断时丢失、状态残留致下次长按被吞。锁内只做最小收尾并标记，
    // 释放锁后调 StopWechatInputMethodSession（它内部也获取 audio_mutex_，故必须先释放）。
    bool end_of_stream = false;
    bool hotkey_send_failed = false;
    {
        std::lock_guard lock(audio_mutex_);
        if (!active_session_id_.has_value() || frame.session_id != *active_session_id_ ||
            active_device_id_ != device_id) {
            return;
        }
        if (frame.IsEnd() && frame.payload.empty()) {
            // 空 payload + IsEnd：固件用此表示音频流结束（与主路径一致），
            // 需收尾调试音频并结束会话，不能由下面的空 payload 早退跳过。
            wechat_audio_end_received_ = true;
            if (ShouldDiscardWechatRecording()) {
                debug_audio_recorder_.Discard();
            } else {
                debug_audio_recorder_.Finish();
            }
            active_session_id_.reset();
            end_of_stream = true;
        } else if (!frame.payload.empty()) {
            ++received_audio_frames_;
            // 先封装 Ogg Opus 并写入调试音频缓存（不依赖解码器，确保解码失败也落盘）。
            auto ogg_chunk = ogg_muxer_.Append(frame.payload, frame.IsEnd());
            debug_audio_recorder_.Append(ogg_chunk);

            constexpr std::size_t kPcmCapacity = 1920;  // 120 ms @ 16 kHz mono。
            int16_t pcm[kPcmCapacity];
            const auto result = wechat_decoder_->Decode(frame.payload.data(), frame.payload.size(), pcm,
                                                        kPcmCapacity);
            if (result.opus_error != 0 || result.decoded_samples <= 0) {
                LogCoordinatorLine("wechat decode failed dev=VS-" + device_id +
                                   " error=" + std::to_string(result.opus_error));
                if (frame.IsEnd()) {
                    wechat_audio_end_received_ = true;
                    if (ShouldDiscardWechatRecording()) {
                        debug_audio_recorder_.Discard();
                    } else {
                        debug_audio_recorder_.Finish();
                    }
                    active_session_id_.reset();
                    end_of_stream = true;
                }
            } else {
                wechat_ring_buffer_->Write(pcm, result.decoded_samples);

                // 首帧解码成功入 ring_buffer：持锁 SendDown 弹框，弹框前 CABLE Output 已有 PCM，
                // 微信弹框即取到有效音频（避免弹框早于音频就绪的首字卡顿）。持锁避免与 Stop race。
                if (!wechat_hotkey_sent_down_) {
                    LogWechatLatency("first frame decoded, SendDown begin");
                    // 点按式发完整点击（down+up），hold 模式发按下：Typeless 等点按式输入法
                    // 靠完整 click 触发，仅按下不释放不弹框。
                    const bool click_mode =
                        (config_.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk);
                    const bool ok = wechat_hotkey_->IsValid() &&
                        (click_mode ? wechat_hotkey_->SendClick()
                                    : wechat_hotkey_->SendDown());
                    if (ok) {
                        wechat_hotkey_sent_down_ = true;
                        LogWechatLatency(click_mode ? "SendClick end (popup triggered)"
                                                    : "SendDown end (popup triggered)");
                    } else {
                        hotkey_send_failed = true;
                    }
                }

                if (frame.IsEnd()) {
                    wechat_audio_end_received_ = true;
                    if (ShouldDiscardWechatRecording()) {
                        debug_audio_recorder_.Discard();
                    } else {
                        debug_audio_recorder_.Finish();
                    }
                    active_session_id_.reset();
                    end_of_stream = true;
                }
            }
        }
        // 锁内 end_of_stream 已设：记 last_stopped 供点按式忽略迟到停止 click。
        // frame.session_id 此时等于原 active_session_id_（上方早退守卫保证），audio_end
        // 分支已 reset active_session_id_，故在此用 frame.session_id 而非 active_session_id_。
        if (end_of_stream) {
            last_stopped_wechat_session_id_ = frame.session_id;
            last_stopped_wechat_at_ = std::chrono::steady_clock::now();
        }
    }

    if (hotkey_send_failed) {
        // SendDown 失败（SendInput 异常）：停会话报错回 ready。未 SendDown 故 Stop 不 SendUp。
        StopWechatInputMethodSession();
        ui_->ShowError("Failed to send WeChat input method hotkey", device_id, {});
        EnterReady("wechat_hotkey_send_failed");
        return;
    }
    if (end_of_stream) {
        // button_up 先到则 wechat_active 已 false，StopWechatInputMethodSession 的收尾幂等无害；
        // audio_end 先到则此处 Stop 后 button_up 再到时 IsWechatInputMethodActive()=false 早退，
        // 不重复 Stop / 不重复 SendUp。
        StopWechatInputMethodSession();
        EnterReady("wechat_audio_end");
    }
}

bool VoiceStickCoordinator::StartWechatInputMethodSession(
    std::optional<std::uint32_t> session_id, const std::string& device_id) {
    if (!wechat_decoder_) {
        wechat_decoder_ = std::make_unique<AudioOpusDecoder>(16000, 1);
    }
    if (!wechat_ring_buffer_) {
        wechat_ring_buffer_ = std::make_unique<PcmRingBuffer>(8192);
    }
    if (!wechat_renderer_) {
        IVirtualMicRenderer::Options options;
        options.sample_rate = 16000;
        options.channels = 1;
        options.bits_per_sample = 16;
        // 事件驱动渲染下 WASAPI buffer 即端到端音频缓冲。20ms 最小化按下到虚拟麦
        // 出声的管道延迟（阶段2 帕累托实测：buffer_ms == 管道延迟ms，20ms 无 device
        // underrun）。真机 BLE 稳态（日志首帧后 1:1）故余量充足；若抖动致丢字可回退 30/50。
        options.buffer_duration_ms = 20;
        options.device_name_substring =
            std::wstring(config_.wechat_input_method.virtual_mic_playback_name.begin(),
                         config_.wechat_input_method.virtual_mic_playback_name.end());
        wechat_renderer_ = wechat_renderer_factory_
                               ? wechat_renderer_factory_(options)
                               : std::make_unique<WasapiVirtualMicRenderer>(options);
    }

    wechat_ring_buffer_->Clear();
    wechat_decoder_->Reset();
    wechat_hotkey_ = wechat_hotkey_factory_
                         ? wechat_hotkey_factory_(config_.wechat_input_method.ActiveHotkey(config_.wechat_input_method.trigger_mode))
                         : std::make_unique<WechatInputMethodHotkey>(config_.wechat_input_method.ActiveHotkey(config_.wechat_input_method.trigger_mode));

    // auto_switch：录音期把默认录音设备(eConsole)切到虚拟麦克风(CABLE Output)，松开切回。
    // 角色分离只切 eConsole，eCommunications 保持真实麦不动，Teams/Skype 通信类会议零干扰。
    // 必须在 SendDown 之前完成：微信弹框即从默认设备取音，未切好会取到真实麦。
    if (config_.wechat_input_method.auto_switch_default_recording_device) {
        LogWechatLatency("auto_switch begin");
        if (!wechat_device_switcher_) {
            wechat_device_switcher_ = wechat_device_switcher_factory_
                ? wechat_device_switcher_factory_()
                : std::make_unique<DefaultAudioDeviceController>();
        }
        if (wechat_device_switcher_) {
            auto saved = wechat_device_switcher_->GetDefaultCapture(DeviceRole::kConsole);
            std::wstring cable_name_w(
                config_.wechat_input_method.virtual_mic_capture_name.begin(),
                config_.wechat_input_method.virtual_mic_capture_name.end());
            auto cable = wechat_device_switcher_->FindCaptureByName(cable_name_w);
            if (saved && cable &&
                wechat_device_switcher_->SetDefaultCapture(cable->id, {DeviceRole::kConsole})) {
                saved_default_capture_id_ = saved->id;
                DeviceSwitchState state{true, WStringToUtf8(saved->id),
                                        WStringToUtf8(saved->friendly_name)};
                SaveDeviceSwitchState(DeviceSwitchStatePath(), state);
            } else {
                LogCoordinatorLine("auto_switch: failed to switch default capture to " +
                                   config_.wechat_input_method.virtual_mic_capture_name);
                // 不阻断会话：renderer.Start 会自行报错或正常，保持现有错误路径。
            }
        } else {
            LogCoordinatorLine("auto_switch: enabled but no device switcher available");
        }
        LogWechatLatency("auto_switch end");
    }

    // 先 renderer.Start（WASAPI 通路就绪），SendDown 推迟到首帧 Opus 解码成功后（见
    // HandleWechatInputMethodAudioFrame）：避免微信弹框即取音却读到静音致首字卡顿。
    // Start 失败直接返回：未 SendDown 故无需补 SendUp 回滚热键。
    LogWechatLatency("renderer.Start begin");
    if (!wechat_renderer_->Start(wechat_ring_buffer_.get())) {
        LogWechatLatency("renderer.Start failed");
        ui_->ShowError("Virtual microphone not found: " +
                           config_.wechat_input_method.virtual_mic_playback_name,
                       device_id, {});
        return false;
    }
    LogWechatLatency("renderer.Start end");

    {
        std::lock_guard lock(audio_mutex_);
        ogg_muxer_.Reset();
        debug_audio_recorder_.Start(device_id, session_id);
        wechat_audio_end_received_ = false;
        wechat_hotkey_sent_down_ = false;
        received_audio_frames_ = 0;
        active_session_id_ = session_id;
        active_device_id_ = device_id;
        active_session_started_at_ = std::chrono::steady_clock::now();
        wechat_input_method_active_ = true;
        SetSessionState(SessionState::kRecording, "wechat_primary_down");
    }
    return true;
}

void VoiceStickCoordinator::StopWechatInputMethodSession() {
    CancelRecordingHardTimeout();
    // 仅当已 SendDown/SendClick 才配对停止热键；未弹框（首帧前 button_up/断连/空 end）不发。
    // 点按式发完整点击停止（与启动对称），hold 模式发释放。
    if (wechat_hotkey_ && wechat_hotkey_->IsValid() && wechat_hotkey_sent_down_) {
        if (config_.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk) {
            wechat_hotkey_->SendClick();
        } else {
            wechat_hotkey_->SendUp();
        }
    }
    if (wechat_renderer_) {
        wechat_renderer_->Stop();
    }
    // 切回原默认录音设备(eConsole)。须在 renderer->Stop(drain 完成)之后：drain 期间
    // renderer 仍往 CABLE Input 写，提前切回会让微信取音源错乱、丢尾音。
    if (saved_default_capture_id_.has_value() && wechat_device_switcher_) {
        wechat_device_switcher_->SetDefaultCapture(*saved_default_capture_id_,
                                                   {DeviceRole::kConsole});
        saved_default_capture_id_.reset();
        ClearDeviceSwitchState(DeviceSwitchStatePath());
    }
    if (wechat_ring_buffer_) {
        wechat_ring_buffer_->Clear();
    }
    if (wechat_decoder_) {
        wechat_decoder_->Reset();
    }

    std::lock_guard lock(audio_mutex_);
    FinishWechatInputMethodRecording();
    // 记最近停止的会话：点按式 audio_end 抢跑 button_click 时，据此忽略迟到的停止 click。
    if (active_session_id_.has_value()) {
        last_stopped_wechat_session_id_ = active_session_id_;
        last_stopped_wechat_at_ = std::chrono::steady_clock::now();
    }
    active_session_id_.reset();
    active_device_id_.reset();
    wechat_input_method_active_ = false;
    wechat_hotkey_sent_down_ = false;
    wechat_latency_anchor_.reset();
}

void VoiceStickCoordinator::FinishWechatInputMethodRecording() {
    // Start 失败路径（未真正进入录音）：不落盘。
    if (!wechat_input_method_active_) return;
    // 零帧或短于最小录音时长的会话丢弃，不落盘调试音频：hold_to_talk_instant 按下即开
    // 录音，无意点按 / button_up 抢跑早于音频帧时会产生仅含 ogg 头的极小文件。
    if (ShouldDiscardWechatRecording()) {
        debug_audio_recorder_.Discard();
        return;
    }
    // 未收到 audio_end 帧（如提前松开按键）：补一个 EOS 页再 Finish，保证 ogg 文件完整。
    if (!wechat_audio_end_received_) {
        auto final_chunk = ogg_muxer_.Finish();
        debug_audio_recorder_.Append(final_chunk);
        wechat_audio_end_received_ = true;
    }
    debug_audio_recorder_.Finish();
}

bool VoiceStickCoordinator::IsWechatInputMethodActive() const {
    return wechat_input_method_active_;
}

bool VoiceStickCoordinator::IsStaleWechatStopClick(
    std::optional<std::uint32_t> session_id) const {
    if (!session_id.has_value() || !last_stopped_wechat_session_id_.has_value() ||
        !last_stopped_wechat_at_.has_value()) {
        return false;
    }
    if (*session_id != *last_stopped_wechat_session_id_) return false;
    // 窗口内视为迟到的停止 click：固件 session_id 递增不复用，2 秒覆盖 audio_end 与
    // button_click 的 BLE 传输抖动，又避免跨会话误判。
    constexpr auto kStaleWindow = std::chrono::seconds(2);
    return (std::chrono::steady_clock::now() - *last_stopped_wechat_at_) <= kStaleWindow;
}

void VoiceStickCoordinator::SetForegroundProbe(std::unique_ptr<IForegroundProcessProbe> probe) {
    foreground_probe_ = std::move(probe);
}

bool VoiceStickCoordinator::MaybeWarnForegroundElevated(const std::string& device_id) {
    if (!foreground_probe_) return false;
    std::wstring process_name;
    if (!foreground_probe_->IsForegroundHigherIntegrity(process_name)) return false;
    // 前台为高权限程序：SendInput 注入必被 UIPI 丢弃，跳过会话启动。
    // 按进程名去重，同一高权限程序本次运行只弹一次气泡。
    if (!elevation_warned_process_.has_value() || *elevation_warned_process_ != process_name) {
        ui_->ShowNotification("需提权运行 VoiceStick",
                              "检测到 " + WStringToUtf8(process_name) +
                                  " 以高权限运行，语音输入被系统拦截。"
                                  "右键托盘 -> 以管理员身份重启后重试。");
        elevation_warned_process_ = process_name;
    }
    LogCoordinatorLine("foreground elevated dev=VS-" + device_id +
                       " proc=" + WStringToUtf8(process_name) +
                       ", skipping wechat session start (UIPI)");
    return true;
}

std::filesystem::path VoiceStickCoordinator::DeviceSwitchStatePath() const {
    if (!device_switch_state_path_.empty()) return device_switch_state_path_;
    return config_.ConfigPath().parent_path() / "default_device_switch_state.json";
}

void VoiceStickCoordinator::RecoverDeviceSwitchStateIfNeeded() {
    DeviceSwitchState state;
    if (!LoadDeviceSwitchState(DeviceSwitchStatePath(), state) || !state.switched) return;
    if (state.saved_default_capture_id.empty()) {
        ClearDeviceSwitchState(DeviceSwitchStatePath());
        return;
    }
    if (!wechat_device_switcher_) {
        wechat_device_switcher_ = wechat_device_switcher_factory_
            ? wechat_device_switcher_factory_()
            : std::make_unique<DefaultAudioDeviceController>();
    }
    if (wechat_device_switcher_) {
        wechat_device_switcher_->SetDefaultCapture(
            Utf8ToWString(state.saved_default_capture_id), {DeviceRole::kConsole});
        LogCoordinatorLine("auto_switch: recovered stale default capture device");
    } else {
        LogCoordinatorLine("auto_switch: stale state found but no device switcher to restore");
    }
    ClearDeviceSwitchState(DeviceSwitchStatePath());
}

void VoiceStickCoordinator::HandleButtonClick(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        // 体感态：主键单击映射为鼠标左键，不走录音/字幕逻辑。
        if (IsAirMouseActive(device_id)) {
            LogCoordinatorLine("air mouse primary click on VS-" + device_id + ", left button");
            input_injector_->ClickLeftButton();
            return;
        }
        if (config_.default_output_profile.target == OutputTarget::kSubtitle) {
            if (config_.interaction_mode != InteractionMode::kClickToTalk) {
                ble_->SendUiState("ready", "", device_id);
                return;
            }
            if (HasActiveSubtitleSession(device_id)) {
                HandleSubtitlePrimaryButtonUp(device_id);
            } else {
                HandleSubtitlePrimaryButtonDown(event.session_id, device_id);
            }
            return;
        }
        if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) {
            if (config_.wechat_input_method.trigger_mode != InteractionMode::kClickToTalk) {
                ble_->SendUiState("ready", "", device_id);
                return;
            }
            // 停止判定须校验 session_id：残留 active（停止 click + audio_end 都丢）时，
            // 新启动 click（新 session_id）不得被误当停止，否则新会话音频被丢弃、与第三方
            // 输入法状态错位且不自愈。button_double_click 不带 session_id（nullopt）视作匹配。
            if (IsWechatInputMethodActive() && active_device_id_ == device_id &&
                (!event.session_id.has_value() || !active_session_id_.has_value() ||
                 *event.session_id == *active_session_id_)) {
                HandleWechatInputMethodPrimaryButtonUp(device_id);
            } else if (IsStaleWechatStopClick(event.session_id)) {
                // 点按式停止时 audio_end 帧可能抢跑 button_click：audio_end 先停会话，
                // 迟到的停止 button_click（同 session_id）不得误判为启动新会话。
                LogCoordinatorLine("wechat click_to_talk stale stop click ignored dev=VS-" +
                                   device_id);
            } else {
                HandleWechatInputMethodPrimaryButtonDown(event.session_id, device_id);
            }
            return;
        }
        if (HandleFrontButtonDuringPendingPaste(device_id)) return;
        if (config_.interaction_mode != InteractionMode::kClickToTalk) {
            ble_->SendUiState("ready", "", device_id);
            return;
        }
        if (session_state_ == SessionState::kRecording && active_device_id_ == device_id) {
            HandlePrimaryButtonUp(device_id);
        } else if (session_state_ == SessionState::kFinalizing && active_device_id_ == device_id) {
            ble_->SendUiState("thinking", "", device_id);
        } else {
            HandlePrimaryButtonDown(event.session_id, device_id);
        }
    } else if (event.button == "secondary") {
        HandleSecondaryButtonClick(device_id);
    }
}

void VoiceStickCoordinator::HandleSecondaryButtonClick(const std::string& device_id) {
    // 体感态已开：侧键单击退出体感（优先于其它语义）。
    if (IsAirMouseActive(device_id)) {
        ToggleAirMouse(device_id);
        return;
    }
    if (HasActiveSubtitleSession(device_id)) {
        CancelSubtitleCycle(device_id, "secondary_cancel");
        return;
    }
    if (std::any_of(subtitle_cycles_.begin(), subtitle_cycles_.end(),
                    [&](const auto& entry) { return entry.first.first == device_id; })) {
        CancelSubtitleCyclesForDevice(device_id, "secondary_cancel");
        return;
    }
    // 有活跃录音/识别/待粘贴：侧键单击保留原取消语义。
    if (active_session_id_.has_value() || IsWaitingForFinalText() ||
        !pending_paste_state_.IsIdle()) {
        CancelPendingPaste(device_id);
        return;
    }
    // 真正空闲：侧键单击进入体感鼠标模式。
    // 「恢复上次输入确认」已改由侧键双击触发（见 HandleButtonDoubleClick 的 secondary 分支），
    // 与进入体感的单击手势分离，避免抢占。
    ToggleAirMouse(device_id);
}

void VoiceStickCoordinator::HandleButtonDoubleClick(const StateEvent& event, const std::string& device_id) {
    // 侧键双击：恢复上次输入确认（与侧键单击=进/退体感分离）。
    if (event.button == "secondary") {
        // 体感态下忽略侧键双击的恢复语义，避免与体感操作冲突。
        if (IsAirMouseActive(device_id)) return;
        // wechat_input_method 模式下侧键双击同样执行恢复，但如果当前有录音则先取消。
        if (IsWechatInputMethodActive()) {
            StopWechatInputMethodSession();
        }
        LogCoordinatorLine("secondary double-click on VS-" + device_id + ", restoring last input");
        RestoreLastInputConfirmation(device_id);
        return;
    }
    if (event.button != "primary") return;

    LogCoordinatorLine("double-click detected on VS-" + device_id + ", sending Enter");
    // 如果当前有活跃录音，取消它。wechat 模式走专用停止路径（发 hotkey.SendUp，让第三方输入法
    // 把已识别文字送入输入框），主路径走 ASR 取消。随后统一注入 Enter 发送输入框文字。
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
    } else {
        std::lock_guard lock(audio_mutex_);
        if (active_session_id_.has_value() && active_device_id_ == device_id) {
            CancelAudioEndTimeout();
            asr_->Cancel();
            pending_paste_state_ = {};
            active_session_id_.reset();
            debug_audio_recorder_.Discard();
            FinishRecognitionCycle();
        }
    }
    // Subtitle 模式下的活跃字幕会话也一并取消。
    CancelSubtitleCyclesForDevice(device_id, "double_click");
    // 注入 Enter 按键。
    input_injector_->SendEnter();
    // 回到就绪状态。
    ble_->SendUiState("ready", "", device_id);
    EnterReady("double_click_enter");
}

void VoiceStickCoordinator::HandleTapEvent(const StateEvent& event, const std::string& device_id) {
    (void)event;
    // 总开关关闭则忽略。
    if (!config_.tap_to_arrow) return;
    // 体感态忽略敲击，避免与体感移动/点击冲突。
    if (IsAirMouseActive(device_id)) return;
    // 录音中或识别中忽略敲击，避免震动干扰当前语音周期。
    // 与双击主键不同：tap 不取消录音/识别，仅在不冲突时注入方向键。
    if (session_state_ == SessionState::kRecording ||
        session_state_ == SessionState::kFinalizing) {
        return;
    }
    // 节流：两次方向键注入最短间隔 500ms，防止快速连击导致光标连续下移。
    const auto now = std::chrono::steady_clock::now();
    if (now - last_tap_inject_at_ < std::chrono::milliseconds(500)) {
        LogCoordinatorLine("tap detected on VS-" + device_id + ", throttled (<500ms since last)");
        return;
    }
    last_tap_inject_at_ = now;
    LogCoordinatorLine("tap detected on VS-" + device_id + ", sending ArrowDown");
    input_injector_->SendArrowDown();
    ble_->SendUiState("ready", "", device_id);
}

bool VoiceStickCoordinator::IsAirMouseActive(const std::string& device_id) const {
    return air_mouse_active_devices_.contains(device_id);
}

bool VoiceStickCoordinator::ToggleAirMouse(const std::string& device_id) {
    if (IsAirMouseActive(device_id)) {
        // 退出体感：清位、清运动学状态、通知固件停表、恢复设备就绪显示。
        air_mouse_active_devices_.erase(device_id);
        air_mouse_states_.erase(device_id);
        ble_->SendAirMouseEnabled(false, device_id);
        ble_->SendUiState("ready", "", device_id);
        LogCoordinatorLine("air mouse disabled on VS-" + device_id);
        if (on_air_mouse_active_changed) on_air_mouse_active_changed(!air_mouse_states_.empty());
        return false;
    }
    // 进入体感：置位、初始化运动学状态、通知固件校准并上报 motion。
    air_mouse_active_devices_.insert(device_id);
    AirMouseDeviceState state{};
    state.last_omega_t = std::chrono::steady_clock::now();  // 防止首次积分 dt 爆炸
    air_mouse_states_[device_id] = state;
    ble_->SendAirMouseEnabled(true, device_id);
    // 下发 ui_state=air_mouse 让设备显示体感态提示，避免用户不知情下主键变鼠标左键
    // 误判"按下没反应"（关闭路径已下发 ready，此处对称补齐）。
    ble_->SendUiState("air_mouse", "", device_id);
    LogCoordinatorLine("air mouse enabled on VS-" + device_id);
    if (on_air_mouse_active_changed) on_air_mouse_active_changed(!air_mouse_states_.empty());
    return true;
}

AirMouseParams VoiceStickCoordinator::AirMouseParamsFromConfig() const {
    AirMouseParams p;
    // 角度控制模型（kAngle，默认）：速度命令用瞬时角速率 omega（见 AirMouseTick），
    // v_target = omega × gain × factor(|omega|)。omega 即固件上报的缩放角速率（dps × REPORT_GAIN=4）。
    // 增益守恒重标定（P1 去双重缩放）：gain = sensitivity × 48，甩动段（high_factor）速度与前代一致。
    // P2 曲线改为平滑 sigmoid、low_factor 0.15→0.25：低端精细段响应更跟手（10dps 处约 2.8× 更灵敏），
    // 40dps 处约 +12%，甩动段基本不变；gain 维持 48 无需重标。真机标定范围约 24~96。
    p.gain_x = static_cast<double>(config_.air_mouse_sensitivity_x) * 48.0;
    p.gain_y = static_cast<double>(config_.air_mouse_sensitivity_y) * 48.0;
    p.tau = config_.air_mouse_tau;
    p.invert_y = config_.air_mouse_invert_y;
    // 曲线参数从配置组装，经 AirMouseCurveClamp 钳位（防配置越界致曲线退化或除零）。
    p.curve.low_thresh = config_.air_mouse_curve_low_thresh;
    p.curve.high_thresh = config_.air_mouse_curve_high_thresh;
    p.curve.low_factor = config_.air_mouse_curve_low_factor;
    p.curve.high_factor = config_.air_mouse_curve_high_factor;
    p.curve = AirMouseCurveClamp(p.curve);
    // 控制模式与飞行摇杆参数。
    p.control_mode = AirMouseControlModeFromName(config_.air_mouse_control_mode);
    p.neutral_deadzone = AirMouseNeutralDeadzoneClamp(config_.air_mouse_neutral_deadzone);
    p.rate_gain = AirMouseRateGainClamp(config_.air_mouse_rate_gain);
    p.rate_friction = AirMouseRateFrictionClamp(config_.air_mouse_rate_friction);
    p.rate_max_speed = AirMouseRateMaxSpeedClamp(config_.air_mouse_rate_max_speed);
    return p;
}

void VoiceStickCoordinator::UpdateAirMouseParams(const AirMouseParams& params) {
    // 热调参轻量路径：仅更新运行期参数，不存盘、不重建 LLM 客户端。
    // AirMouseTick 用 live_air_mouse_params_，下个 tick 即时生效。保存由调用方写 config_ + Save。
    live_air_mouse_params_ = params;
}

void VoiceStickCoordinator::AirMouseTick() {
    if (air_mouse_states_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto params = live_air_mouse_params_;  // 运行期 live 参数（热调参即时生效，不走 UpdateConfig）
    const double stale_age_sec = std::chrono::duration<double>(kAirMouseOmegaStaleAge).count();
    // 固定 dt = tick 周期（WM_TIMER 60Hz 稳定）；stale 判断用 last_omega_t。
    const double dt = std::chrono::duration<double>(kAirMouseTickInterval).count();
    for (auto& [device_id, state] : air_mouse_states_) {
        const double omega_age = std::chrono::duration<double>(now - state.last_omega_t).count();
        const bool stale = omega_age > stale_age_sec;

        // 断帧/超龄（stale）：固件约 50Hz 持续上报，停手时下发 omega=0 帧使 last_omega 自然归零；
        // 但若连接抖动/丢帧导致长期无新帧，kAngle 模式若保留旧 last_omega 会继续驱动光标，
        // 故此处把 last_omega 归零，使速度环经 tau 滑行停止（修复 P0 引入的"断帧仍持续移动"）。
        if (stale) {
            state.last_omega_x = 0.0;
            state.last_omega_y = 0.0;
        }

        // 回到中立区（theta/omega 都很小）或 stale 时归零 theta，实现"回正即停"。
        // 对 kRate 模式 theta 仍驱动加速度，归零可重置飞行摇杆状态。
        if (stale ||
            (std::fabs(state.theta_x) < kAirMouseAngleDeadzone &&
             std::fabs(state.theta_y) < kAirMouseAngleDeadzone &&
             std::fabs(state.last_omega_x) < kAirMouseOmegaDeadzone &&
             std::fabs(state.last_omega_y) < kAirMouseOmegaDeadzone)) {
            state.theta_x = 0.0;
            state.theta_y = 0.0;
        }

        AirMouseInput input;
        if (params.control_mode == AirMouseControlMode::kAngle) {
            // P0 修复：角度控制的速度命令用瞬时角速率 omega（last_omega），而非积分转角 theta。
            // theta 随持续旋转无限增长，套用增益曲线会正反馈失控（匀速转 3s 光标速度从万级飙到
            // 数十万 px/s）。改用 omega 后：匀速转=匀速移、停转即停，增益曲线阈值重新对应物理角速率。
            // kRate 飞行摇杆模式仍用 theta（积分量）驱动加速度，保持原逻辑（见下）。
            input.value_x = state.last_omega_x;
            input.value_y = state.last_omega_y;
            input.is_angle = false;
        } else {
            // kRate：theta 控制光标速度变化率，回中后速度保持并摩擦衰减。
            input.value_x = static_cast<int>(state.theta_x);
            input.value_y = static_cast<int>(state.theta_y);
            input.is_angle = true;
        }

        const auto result = AirMouseStep(state.kin, input, dt, stale, params);
        static int tick_log_counter = 0;
        if (++tick_log_counter % 30 == 0) {
            LogCoordinatorLine("tick VS-" + device_id + " vx=" + std::to_string(state.kin.vx) +
                               " dx=" + std::to_string(result.dx) +
                               " stale=" + (stale ? "1" : "0") +
                               " mode=" + (params.control_mode == AirMouseControlMode::kAngle ? "angle" : "rate") +
                               " omx=" + std::to_string(state.last_omega_x) +
                               " theta=" + std::to_string(state.theta_x));
        }
        if (result.dx != 0 || result.dy != 0) {
            input_injector_->MoveMouse(result.dx, result.dy);
        }
    }
}

void VoiceStickCoordinator::HandleMotionEvent(const MotionEvent& event, const std::string& device_id) {
    // 仅在该设备处于体感态时更新 omega 并积分 theta；光标位移由 AirMouseTick 统一驱动。
    if (!IsAirMouseActive(device_id)) return;
    auto& state = air_mouse_states_[device_id];

    // theta 集成：对 omega 积分得到相对中立姿态的偏转角，供 kRate 飞行摇杆模式（theta→加速度）使用。
    // kAngle 模式已改为直接用瞬时 omega 驱动速度，不再依赖 theta。
    // 用 tick 周期作为 dt 估计，避免 BLE 帧率抖动和测试连续调用导致 dt≈0；固件 50Hz 与桌面 60Hz 接近。
    const double dt = std::chrono::duration<double>(kAirMouseTickInterval).count();

    // 保持某角度时 omega≈0，theta 不变，光标持续移动；回正到中立区后 AirMouseTick 归零。
    state.theta_x += event.dx * dt;
    state.theta_y += event.dy * dt;
    state.theta_x = std::clamp(state.theta_x, -kAirMouseMaxTheta, kAirMouseMaxTheta);
    state.theta_y = std::clamp(state.theta_y, -kAirMouseMaxTheta, kAirMouseMaxTheta);

    state.last_omega_x = event.dx;
    state.last_omega_y = event.dy;
    state.last_omega_t = std::chrono::steady_clock::now();
    LogCoordinatorLine("motion VS-" + device_id + " dx=" + std::to_string(event.dx) +
                       " dy=" + std::to_string(event.dy) +
                       " theta=" + std::to_string(state.theta_x));
}

void VoiceStickCoordinator::HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                       const std::string& device_id) {
    // 体感态：主键长按/按下不启动录音（点击由 button_click 映射为左键）。
    if (IsAirMouseActive(device_id)) {
        return;
    }
    if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) {
        HandleWechatInputMethodPrimaryButtonDown(session_id, device_id);
        return;
    }
    if (config_.default_output_profile.target == OutputTarget::kSubtitle) {
        HandleSubtitlePrimaryButtonDown(session_id, device_id);
        return;
    }
    if (HandleFrontButtonDuringPendingPaste(device_id)) return;
    // 残留自愈：同一设备卡在 recording（button_up/audio_end 都丢）时再来 button_down，先停旧
    // 会话再 Start 新的，否则被吞掉致用户怎么按都没反应。安全前提：固件 hold_to_talk 录音中
    // 再按主键不发新 button_down，故收到 button_down 时若仍在 recording 必为残留。finalizing /
    // pending 等状态保留原 RefreshDeviceUiState（不打断正常等 audio_end 或确认流程）。
    if (session_state_ == SessionState::kRecording && active_device_id_.has_value() &&
        *active_device_id_ == device_id) {
        std::lock_guard lock(audio_mutex_);
        CancelShortRecording();
    } else if (session_state_ != SessionState::kReady || active_device_id_.has_value()) {
        RefreshDeviceUiState(device_id);
        return;
    }
    if (!session_id.has_value() || *session_id == 0) {
        ble_->SendUiState("ready", "", device_id);
        return;
    }

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
        ScheduleRecordingHardTimeout();
        ScheduleRecordingStallWatchdog();
    }
    ui_->ShowListening(active_device_id_);
    SendUiStateForActiveDevice("recording");
}

void VoiceStickCoordinator::HandlePrimaryButtonUp(const std::string& device_id) {
    if (IsWechatInputMethodActive()) {
        HandleWechatInputMethodPrimaryButtonUp(device_id);
        return;
    }
    if (HasActiveSubtitleSession(device_id)) {
        HandleSubtitlePrimaryButtonUp(device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || active_device_id_ != device_id) return;
    const double duration = CurrentRecordingDurationSeconds();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
    } else {
        BeginWaitingForAudioEnd("button_up");
    }
}

void VoiceStickCoordinator::HandleAudioFrame(const AudioFrame& frame, const std::string& device_id) {
    const auto t0 = std::chrono::steady_clock::now();
    if (IsWechatInputMethodActive()) {
        HandleWechatInputMethodAudioFrame(frame, device_id);
        return;
    }
    if (FindSubtitleCycle(device_id, frame.session_id)) {
        HandleSubtitleAudioFrame(frame, device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || frame.session_id != *active_session_id_ || active_device_id_ != device_id) {
        return;
    }
    last_audio_frame_ms_.store(SteadyNowMs());
    if (frame.IsEnd() && frame.payload.empty()) {
        CancelAudioEndTimeout();
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
    SendOrBufferOggChunk(
        ogg_chunk,
        frame.IsEnd(),
        CurrentRecordingDurationSeconds() >= kMinimumRecordingDurationSeconds &&
            (!waiting_for_audio_end_.load() || frame.IsEnd()));
    if (frame.IsEnd()) {
        CancelAudioEndTimeout();
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

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                            const std::string& device_id) {
    if (!session_id.has_value() || *session_id == 0) {
        ble_->SendUiState("ready", "", device_id);
        return;
    }
    if (auto active = active_subtitle_sessions_.find(device_id);
        (active != active_subtitle_sessions_.end() && active->second == *session_id) ||
        subtitle_cycles_.contains({device_id, *session_id})) {
        return;
    }
    if (auto previous = active_subtitle_sessions_.find(device_id);
        previous != active_subtitle_sessions_.end()) {
        ClearActiveSubtitleSession(device_id, previous->second);
    }
    if (!asr_factory_) {
        ui_->ShowError("Subtitle ASR is not available", device_id, [this, device_id] {
            ble_->SendUiState("ready", "", device_id);
        });
        return;
    }
    auto cycle = std::make_unique<SubtitleCycle>();
    cycle->device_id = device_id;
    cycle->session_id = *session_id;
    cycle->started_at = std::chrono::steady_clock::now();
    cycle->asr = asr_factory_(config_);
    cycle->debug_audio_recorder = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    ConfigureSubtitleAsrCallbacks(cycle.get());
    cycle->debug_audio_recorder.Start(device_id, session_id);
    subtitle_cycles_[{device_id, *session_id}] = std::move(cycle);
    active_subtitle_sessions_[device_id] = *session_id;
    ui_->ShowListening(device_id);
    ble_->SendUiState("recording", "", device_id);
}

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonUp(const std::string& device_id) {
    auto* cycle = FindActiveSubtitleCycle(device_id);
    if (!cycle) return;
    const auto session_id = cycle->session_id;
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelSubtitleCycle(device_id, "short_recording");
    } else {
        BeginWaitingForSubtitleAudioEnd(cycle, "button_up");
        FinishSubtitleAudioInput(cycle);
    }
}

void VoiceStickCoordinator::HandleSubtitleAudioFrame(const AudioFrame& frame, const std::string& device_id) {
    auto* cycle = FindSubtitleCycle(device_id, frame.session_id);
    if (!cycle) return;
    if (frame.IsEnd() && frame.payload.empty()) {
        CancelSubtitleAudioEndTimeout(cycle);
        SendSubtitleFinalOggChunkIfNeeded(device_id, frame.session_id);
        return;
    }
    if (frame.payload.empty()) return;
    if (cycle->last_audio_seq.has_value() && frame.seq != *cycle->last_audio_seq + 1) {
        LogCoordinatorLine("subtitle audio seq gap dev=VS-" + device_id +
                           " session=" + std::to_string(cycle->session_id) +
                           " expected=" + std::to_string(*cycle->last_audio_seq + 1) +
                           " got=" + std::to_string(frame.seq));
    }
    cycle->last_audio_seq = frame.seq;
    ++cycle->received_audio_frames;
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    auto ogg_chunk = cycle->ogg_muxer.Append(frame.payload, frame.IsEnd());
    cycle->debug_audio_recorder.Append(ogg_chunk);
    SendOrBufferSubtitleOggChunk(cycle, ogg_chunk, frame.IsEnd(),
                                 duration >= kMinimumRecordingDurationSeconds &&
                                     (!cycle->waiting_for_audio_end || frame.IsEnd()));
    if (frame.IsEnd()) {
        CancelSubtitleAudioEndTimeout(cycle);
        cycle->sent_final_audio_chunk = true;
        cycle->debug_audio_recorder.Finish();
        FinishSubtitleAudioInput(cycle);
    }
}

void VoiceStickCoordinator::BeginWaitingForSubtitleAudioEnd(SubtitleCycle* cycle, std::string_view reason) {
    if (!cycle || cycle->waiting_for_audio_end) return;
    cycle->waiting_for_audio_end = true;
    LogCoordinatorLine("waiting for subtitle audio END VS-" + cycle->device_id +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    if (config_.interaction_mode != InteractionMode::kHoldToTalk) {
        ui_->SetStatus("Processing");
        ble_->SendUiState("thinking", "", cycle->device_id);
    }
    ScheduleSubtitleAudioEndTimeout(cycle->device_id, cycle->session_id);
}

void VoiceStickCoordinator::ScheduleSubtitleAudioEndTimeout(const std::string& device_id,
                                                            std::uint32_t session_id) {
    auto* cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle) return;
    const auto generation = ++cycle->audio_end_wait_generation;
    std::thread([this, alive = alive_, device_id, session_id, generation] {
        std::this_thread::sleep_for(kAudioEndTimeout);
        if (!alive->load()) return;
        auto* cycle = FindSubtitleCycle(device_id, session_id);
        if (!cycle || !cycle->waiting_for_audio_end ||
            cycle->audio_end_wait_generation != generation) {
            return;
        }
        LogCoordinatorLine("subtitle audio END timeout VS-" + device_id +
                           "; finalizing buffered audio");
        SendSubtitleFinalOggChunkIfNeeded(device_id, session_id);
    }).detach();
}

void VoiceStickCoordinator::CancelSubtitleAudioEndTimeout(SubtitleCycle* cycle) {
    if (!cycle) return;
    cycle->waiting_for_audio_end = false;
    ++cycle->audio_end_wait_generation;
}

void VoiceStickCoordinator::FinishSubtitleAudioInput(SubtitleCycle* cycle) {
    if (!cycle) return;
    if (config_.interaction_mode == InteractionMode::kHoldToTalk) {
        ClearActiveSubtitleSession(cycle->device_id, cycle->session_id);
        ble_->SendUiState("ready", "", cycle->device_id);
    } else {
        ui_->SetStatus("Processing");
        ble_->SendUiState("thinking", "", cycle->device_id);
    }
}

void VoiceStickCoordinator::SendSubtitleFinalOggChunkIfNeeded(const std::string& device_id,
                                                              std::uint32_t session_id) {
    auto* cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle) return;
    if (cycle->sent_final_audio_chunk) return;
    cycle->sent_final_audio_chunk = true;
    CancelSubtitleAudioEndTimeout(cycle);
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    if (!cycle->asr_started && duration < kMinimumRecordingDurationSeconds) {
        const bool was_active = IsActiveSubtitleCycle(device_id, session_id);
        ClearActiveSubtitleSession(device_id, session_id);
        auto it = subtitle_cycles_.find({device_id, session_id});
        if (it != subtitle_cycles_.end()) {
            if (it->second->asr) it->second->asr->Cancel();
            it->second->debug_audio_recorder.Discard();
            subtitle_cycles_.erase(it);
        }
        if (was_active) {
            ui_->HideOverlay();
            ui_->SetStatus("Ready");
            ble_->SendUiState("ready", "", device_id);
        }
        return;
    }
    if (cycle->received_audio_frames == 0) {
        FinishSubtitleCycleWithError(device_id, session_id, "No audio frames from device");
        return;
    }
    auto final_chunk = cycle->ogg_muxer.Finish();
    cycle->debug_audio_recorder.Append(final_chunk);
    cycle->debug_audio_recorder.Finish();
    SendOrBufferSubtitleOggChunk(cycle, final_chunk, true, true);
}

void VoiceStickCoordinator::SendOrBufferSubtitleOggChunk(SubtitleCycle* cycle,
                                                         const ByteVector& chunk,
                                                         bool is_last,
                                                         bool can_start_asr) {
    if (!cycle || !cycle->asr) return;
    if (cycle->asr_started) {
        cycle->asr->SendOggOpusChunk(chunk, is_last);
        return;
    }
    cycle->buffered_ogg_chunks.push_back(chunk);
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    if (can_start_asr && !StartSubtitleAsrAndFlushBufferedChunks(cycle, is_last)) {
        std::string message = "Failed to start ASR";
        if (auto* current_cycle = FindSubtitleCycle(device_id, session_id); current_cycle && current_cycle->asr) {
            message = AsrStartFailureMessage(*current_cycle->asr);
        }
        FinishSubtitleCycleWithError(device_id, session_id, message);
    }
}

bool VoiceStickCoordinator::StartSubtitleAsrAndFlushBufferedChunks(SubtitleCycle* cycle,
                                                                   bool last_chunk_is_final) {
    if (!cycle || !cycle->asr) return false;
    if (cycle->asr_started) return true;
    AsrSessionOptions options;
    options.hotwords = config_.asr_hotwords;
    const bool use_definite_segments = ShouldUseDefiniteSegments(OutputProfileForDevice(cycle->device_id));
    options.show_utterances = use_definite_segments;
    options.result_type = use_definite_segments ? AsrResultType::kSingle : AsrResultType::kFull;
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    auto* asr = cycle->asr.get();
    if (!asr->Start(options)) {
        if (auto* current_cycle = FindSubtitleCycle(device_id, session_id)) {
            current_cycle->buffered_ogg_chunks.clear();
        }
        return false;
    }
    cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle || cycle->asr.get() != asr) return false;
    cycle->asr_started = true;
    for (std::size_t i = 0; i < cycle->buffered_ogg_chunks.size(); ++i) {
        const bool is_last = (i + 1 == cycle->buffered_ogg_chunks.size()) && last_chunk_is_final;
        cycle->asr->SendOggOpusChunk(cycle->buffered_ogg_chunks[i], is_last);
    }
    cycle->buffered_ogg_chunks.clear();
    return true;
}

void VoiceStickCoordinator::SendFinalOggChunkIfNeeded(double recording_duration_seconds) {
    if (sent_final_audio_chunk_) return;
    sent_final_audio_chunk_ = true;
    CancelAudioEndTimeout();
    if (!asr_started_ && recording_duration_seconds < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
        return;
    }
    if (received_audio_frames_ == 0) {
        active_session_id_.reset();
        FinishWithAsrError("No audio frames from device");
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
        if (!is_showing_asr_error_) FinishWithAsrError(AsrStartFailureMessage(*asr_));
    }
}

bool VoiceStickCoordinator::StartAsrAndFlushBufferedChunks(bool last_chunk_is_final) {
    if (asr_started_) return true;
    AsrSessionOptions options;
    options.hotwords = config_.asr_hotwords;
    const auto profile = OutputProfileForDevice(active_device_id_);
    const bool use_definite_segments = ShouldUseDefiniteSegments(profile);
    options.show_utterances = use_definite_segments;
    options.result_type = use_definite_segments ? AsrResultType::kSingle : AsrResultType::kFull;
    if (!asr_->Start(options)) {
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
    active_session_id_.reset();
    active_session_started_at_ = {};
    CancelAudioEndTimeout();
    buffered_ogg_chunks_.clear();
    asr_->Cancel();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterReady("short_recording");
}

void VoiceStickCoordinator::FinishWithFinalText(const std::string& text) {
    if (pasted_final_text_) return;
    pasted_final_text_ = true;
    // 保存 ASR 原文作为 finalizing watchdog 的回退文本：后续 translate/refine 任一环节
    // 无响应时可直接粘贴原文，保证不丢本次输入。
    finalizing_fallback_text_ = text;
    const auto profile = OutputProfileForDevice(active_device_id_);
    if (profile.target == OutputTarget::kSubtitle) {
        if (!text.empty() && active_device_id_.has_value()) {
            ShowSubtitleText(text, profile, *active_device_id_);
        }
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        EnterReady("subtitle_final_done");
        return;
    }
    if (text.empty()) {
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        ui_->HideOverlay();
        EnterReady("empty_final_done", false);
        return;
    }
    if (profile.transform == TextTransform::kTranslate) {
        ui_->SetStatus("Translating");
        TransformText(text, profile, [this](bool ok, std::string result) {
            if (ok) {
                last_recoverable_text_ = result;
                last_recoverable_device_id_ = active_device_id_;
                ui_->SetHasRecoverableInput(true);
                EnterPendingConfirmation(result, "translation_final");
            } else {
                FinishWithAsrError(result);
            }
        });
        return;
    }
    if (config_.refine_enabled) {
        ui_->SetStatus("Refining");
        // 立即把 ASR 原文刷上悬浮窗并进入精修态（kRefining 指示器 + 末尾闪烁光标），
        // 让用户在 LLM 首 token 到达前（建连 + TTFT 约 1~2s）就能看到识别结果，
        // 而非冻结在旧 partial 上造成"卡住"感。精修流式 token 随后经 AppendPartial 覆盖。
        if (active_device_id_.has_value()) {
            ui_->ShowRefining(text, *active_device_id_);
        }
    }
    TransformText(text, profile, [this](bool ok, std::string result) {
        (void)ok;
        last_recoverable_text_ = result;
        last_recoverable_device_id_ = active_device_id_;
        ui_->SetHasRecoverableInput(true);
        EnterPendingConfirmation(result, "asr_final");
    });
}

void VoiceStickCoordinator::HandleDefiniteSegment(const AsrSegment& segment) {
    if (!segment.definite || !active_device_id_.has_value()) return;
    const auto profile = OutputProfileForDevice(active_device_id_);
    if (!ShouldUseDefiniteSegments(profile)) return;
    ui_->HideOverlay();
    ShowSubtitleText(segment.text, profile, *active_device_id_);
}

void VoiceStickCoordinator::HandleSubtitleDefiniteSegment(const AsrSegment& segment,
                                                          const std::string& device_id) {
    if (!segment.definite) return;
    const auto profile = OutputProfileForDevice(device_id);
    if (!ShouldUseDefiniteSegments(profile)) return;
    ui_->HideOverlay();
    ShowSubtitleText(segment.text, profile, device_id);
}

void VoiceStickCoordinator::FinishSubtitleCycleWithFinalText(const std::string& device_id,
                                                             std::uint32_t session_id,
                                                             const std::string& text) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end() || it->second->finished_final_text) return;
    it->second->finished_final_text = true;
    const auto profile = OutputProfileForDevice(device_id);
    if (!text.empty()) {
        ShowSubtitleText(text, profile, device_id, [this, device_id, session_id](bool did_show_subtitle) {
            FinishSubtitleCycle(
                device_id,
                session_id,
                did_show_subtitle && ShouldHideOverlayForFinishedSubtitleCycle(device_id, session_id));
        });
        return;
    }
    FinishSubtitleCycle(device_id, session_id,
                        ShouldHideOverlayForFinishedSubtitleCycle(device_id, session_id));
}

void VoiceStickCoordinator::FinishSubtitleCycleWithError(const std::string& device_id,
                                                         std::uint32_t session_id,
                                                         const std::string& message) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("subtitle ASR error VS-" + device_id + ": " + message);
    if (it->second->asr) it->second->asr->Cancel();
    it->second->debug_audio_recorder.Discard();
    CancelSubtitleAudioEndTimeout(it->second.get());
    ClearActiveSubtitleSession(device_id, session_id);
    ui_->ShowError(message, device_id, [this, device_id] {
        ble_->SendUiState("ready", "", device_id);
    });
    subtitle_cycles_.erase(it);
}

void VoiceStickCoordinator::CancelSubtitleCycle(const std::string& device_id, std::string_view reason) {
    auto* cycle = FindActiveSubtitleCycle(device_id);
    if (!cycle) return;
    const auto session_id = cycle->session_id;
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("cancel subtitle cycle VS-" + device_id + " reason=" + std::string(reason));
    if (it->second->asr) it->second->asr->Cancel();
    it->second->debug_audio_recorder.Discard();
    CancelSubtitleAudioEndTimeout(it->second.get());
    ui_->HideOverlay();
    ble_->SendUiState("ready", "", device_id);
    ClearActiveSubtitleSession(device_id, session_id);
    subtitle_cycles_.erase(it);
}

void VoiceStickCoordinator::FinishSubtitleCycle(const std::string& device_id,
                                                std::uint32_t session_id,
                                                bool hide_overlay) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("finish subtitle cycle VS-" + device_id +
                       " session=" + std::to_string(session_id) +
                       " hide_overlay=" + (hide_overlay ? "true" : "false"));
    if (hide_overlay) ui_->HideOverlay();
    ClearActiveSubtitleSession(device_id, session_id);
    if (!HasActiveSubtitleSession(device_id)) {
        ui_->SetStatus("Ready");
        ble_->SendUiState("ready", "", device_id);
    }
    subtitle_cycles_.erase(it);
}

bool VoiceStickCoordinator::ShouldHideOverlayForFinishedSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) const {
    return IsActiveSubtitleCycle(device_id, session_id) || !HasActiveSubtitleSession(device_id);
}

bool VoiceStickCoordinator::CanUpdateOverlayForSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) const {
    auto it = active_subtitle_sessions_.find(device_id);
    return it == active_subtitle_sessions_.end() || it->second == session_id;
}

bool VoiceStickCoordinator::ShouldSendPartialToDevice() const {
    return sent_final_audio_chunk_;
}

bool VoiceStickCoordinator::ShouldSendSubtitlePartialToDevice(const SubtitleCycle* cycle) const {
    return cycle && IsActiveSubtitleCycle(cycle->device_id, cycle->session_id) &&
           cycle->sent_final_audio_chunk;
}

VoiceStickCoordinator::SubtitleCycle* VoiceStickCoordinator::FindSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    return it == subtitle_cycles_.end() ? nullptr : it->second.get();
}

VoiceStickCoordinator::SubtitleCycle* VoiceStickCoordinator::FindActiveSubtitleCycle(
    const std::string& device_id) {
    auto it = active_subtitle_sessions_.find(device_id);
    if (it == active_subtitle_sessions_.end()) return nullptr;
    return FindSubtitleCycle(device_id, it->second);
}

bool VoiceStickCoordinator::IsActiveSubtitleCycle(const std::string& device_id,
                                                  std::uint32_t session_id) const {
    auto it = active_subtitle_sessions_.find(device_id);
    return it != active_subtitle_sessions_.end() && it->second == session_id;
}

bool VoiceStickCoordinator::HasActiveSubtitleSession(const std::string& device_id) const {
    return active_subtitle_sessions_.contains(device_id);
}

void VoiceStickCoordinator::ClearActiveSubtitleSession(const std::string& device_id,
                                                       std::uint32_t session_id) {
    auto it = active_subtitle_sessions_.find(device_id);
    if (it != active_subtitle_sessions_.end() && it->second == session_id) {
        active_subtitle_sessions_.erase(it);
    }
}

void VoiceStickCoordinator::CancelSubtitleCyclesForDevice(const std::string& device_id,
                                                          std::string_view reason) {
    LogCoordinatorLine("cancel subtitle cycles VS-" + device_id + " reason=" + std::string(reason));
    active_subtitle_sessions_.erase(device_id);
    for (auto it = subtitle_cycles_.begin(); it != subtitle_cycles_.end();) {
        if (it->first.first != device_id) {
            ++it;
            continue;
        }
        if (it->second->asr) it->second->asr->Cancel();
        it->second->debug_audio_recorder.Discard();
        CancelSubtitleAudioEndTimeout(it->second.get());
        it = subtitle_cycles_.erase(it);
    }
    ui_->HideOverlay();
    ble_->SendUiState("ready", "", device_id);
}

void VoiceStickCoordinator::ShowSubtitleText(const std::string& text,
                                             const OutputProfile& profile,
                                             const std::string& device_id,
                                             std::function<void(bool)> completion) {
    TransformText(text, profile, [this, device_id, completion = std::move(completion)](bool ok, std::string result) mutable {
        if (ok) {
            if (!HasActiveSubtitleSession(device_id)) ui_->HideOverlay();
            ui_->ShowSubtitle(result, device_id, ThemeColorForDevice(device_id));
            if (completion) completion(true);
        } else {
            ui_->ShowError(result, device_id, [] {});
            if (completion) completion(false);
        }
    });
}

void VoiceStickCoordinator::TransformText(const std::string& text,
                                          const OutputProfile& profile,
                                          std::function<void(bool, std::string)> completion) {
    if (profile.transform == TextTransform::kTranslate) {
        translator_.Translate(text, profile.translation_target, config_.asr_hotwords, std::move(completion));
        return;
    }
    // 原文路径：若启用精修，过一道 LLM 去停顿空格 / 修标点 / 去口头语；best-effort，失败回退原文。
    if (config_.refine_enabled && !text.empty()) {
        // 流式精修：on_token 在后台线程节流式追加显示（用 AppendPartial 跳过文字滚动
        // 动画，避免高频 token 反复重置 140ms 动画导致闪动；overlay OnTimer 已优化为
        // 文本未变时不重建 D2D 文本布局，避免卡死）。
        CancelStreamingRefinement();
        refinement_cancel_token_ = std::make_shared<std::atomic_bool>(false);

        auto alive = alive_;
        auto cancel = refinement_cancel_token_;
        auto device_id = active_device_id_;

        // 节流状态：跨 on_token 回调共享，每 ~60ms 最多更新一次 UI
        struct ThrottleState {
            std::mutex mutex;
            std::string accumulated;
            std::chrono::steady_clock::time_point last_update{};
        };
        auto throttle = std::make_shared<ThrottleState>();

        refiner_.RefineStream(
            text,
            config_.refine_prompt,
            // on_token（后台线程）：节流式追加更新悬浮窗
            [this, alive, cancel, device_id, throttle](std::string token) {
                if (!alive->load() || (cancel && cancel->load())) return;
                TouchFinalizingWatchdog();
                std::string current;
                bool should_update = false;
                {
                    std::lock_guard lock(throttle->mutex);
                    throttle->accumulated += token;
                    auto now = std::chrono::steady_clock::now();
                    if (now - throttle->last_update >= std::chrono::milliseconds(60)) {
                        throttle->last_update = now;
                        current = throttle->accumulated;
                        should_update = true;
                    }
                }
                if (should_update) {
                    ui_->AppendPartial(current, device_id);
                }
            },
            // on_complete（后台线程）：最终文本已就绪
            [this, alive, cancel, text, device_id, throttle,
             completion = std::move(completion)](bool ok, std::string result) mutable {
                if (!alive->load() || (cancel && cancel->load())) return;
                std::string final_text = text;
                if (ok && !result.empty()) {
                    // 热词守卫：精修把 ASR 原文中已正确的热词改坏时回退原文
                    // （小模型精修不稳定的本地兜底）。
                    if (LLMRefinementClient::RefineResultKeepsHotwords(text, result,
                                                                       config_.asr_hotwords)) {
                        final_text = result;
                        // 用最终的累积文本做最后一次 UI 刷新
                        ui_->ShowPartial(result, device_id);
                        MineHotwordCandidatesFromRefinement(text, result);
                    } else {
                        LogCoordinatorLine("refine corrupted a hotword present in ASR text; "
                                           "falling back to original");
                    }
                }
                CancelStreamingRefinement();
                completion(true, final_text);
                MaybeExtractHotwordCandidates(final_text);
            },
            cancel);
        return;
    }
    completion(true, text);
    MaybeExtractHotwordCandidates(text);
}

void VoiceStickCoordinator::MineHotwordCandidatesFromRefinement(const std::string& original,
                                                                const std::string& refined) {
    const auto mined = MineRefinementCandidates(original, refined, config_.asr_hotwords);
    if (!mined.empty()) RecordAndNotifyHotwordCandidates(mined);
}

void VoiceStickCoordinator::MaybeExtractHotwordCandidates(const std::string& final_text) {
    if (!config_.hotword_mining_enabled || config_.llm_api_key.empty() || final_text.empty()) {
        LogCoordinatorLine(std::string("hotword extraction skipped: ") +
                           (!config_.hotword_mining_enabled
                                ? "mining_disabled"
                                : (config_.llm_api_key.empty() ? "no_llm_key" : "empty_text")));
        return;
    }
    LogCoordinatorLine("hotword extraction started");
    auto alive = alive_;
    refiner_.ExtractHotwordCandidates(
        final_text, config_.asr_hotwords,
        [this, alive](bool ok, std::vector<std::string> words) {
            if (!alive->load()) return;
            LogCoordinatorLine("hotword extraction finished ok=" + std::string(ok ? "1" : "0") +
                               " candidates=" + std::to_string(words.size()));
            if (!ok || words.empty()) return;
            RecordAndNotifyHotwordCandidates(words);
        });
}

void VoiceStickCoordinator::RecordAndNotifyHotwordCandidates(const std::vector<std::string>& words) {
    const auto path = config_.ConfigPath().parent_path() / "hotword_candidates.json";
    std::vector<std::string> suggestions;
    {
        std::lock_guard lock(hotword_candidates_mutex_);
        if (!hotword_candidates_loaded_) {
            hotword_candidates_ = LoadHotwordCandidates(path);
            hotword_candidates_loaded_ = true;
        }
        suggestions = RecordHotwordCandidates(hotword_candidates_, words);
        for (const auto& word : suggestions) hotword_candidates_.notified.insert(word);
        SaveHotwordCandidates(path, hotword_candidates_);
    }

    if (!suggestions.empty()) {
        const auto language = EffectiveUiLanguage(config_.ui_language);
        std::string joined;
        for (std::size_t i = 0; i < suggestions.size(); ++i) {
            if (i != 0) joined += ", ";
            joined += suggestions[i];
        }
        LogCoordinatorLine("hotword candidates suggested: " + joined);
        ui_->ShowNotification(Tr(StringId::kHotwordCandidateNotifyTitle, language),
                              joined + Tr(StringId::kHotwordCandidateNotifyBodySuffix, language));
    }
}

void VoiceStickCoordinator::BeginWaitingForAudioEnd(std::string_view reason) {
    if (waiting_for_audio_end_.load()) return;
    waiting_for_audio_end_.store(true);
    LogCoordinatorLine(std::string("waiting for audio END") +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    EnterFinalizing("waiting_audio_end");
    ScheduleAudioEndTimeout(active_session_id_, active_device_id_);
}

void VoiceStickCoordinator::ScheduleAudioEndTimeout(std::optional<std::uint32_t> session_id,
                                                    std::optional<std::string> device_id) {
    const auto generation = audio_end_wait_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation, session_id, device_id = std::move(device_id)] {
        std::this_thread::sleep_for(kAudioEndTimeout);
        if (!alive->load()) return;
        std::lock_guard lock(audio_mutex_);
        if (audio_end_wait_generation_.load() != generation ||
            !waiting_for_audio_end_.load() ||
            active_session_id_ != session_id ||
            active_device_id_ != device_id) {
            return;
        }
        LogCoordinatorLine("audio END timeout; finalizing buffered audio");
        SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
    }).detach();
}

void VoiceStickCoordinator::CancelAudioEndTimeout() {
    waiting_for_audio_end_.store(false);
    audio_end_wait_generation_.fetch_add(1);
}

// recording 硬超时兜底：button_down 进 recording 时调度，button_up/audio_end/取消/断连
// 经 EnterReady/EnterFinalizing 取消。超时未收到结束信号则按当前输出模式走停止路径，
// 覆盖 button_up 与 audio_end 同时丢失致永久卡 listening。锁内仅校验 generation 并读取
// 停止路径所需状态，释放锁后调 Stop（StopWechatInputMethodSession 内部获取 audio_mutex_，
// 持锁调用会死锁）。
void VoiceStickCoordinator::ScheduleRecordingHardTimeout() {
    const auto generation = recording_hard_timeout_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation]() {
        std::this_thread::sleep_for(recording_hard_timeout_);
        if (!alive->load()) return;
        bool stale = false;
        bool wechat_active = false;
        {
            std::lock_guard lock(audio_mutex_);
            if (recording_hard_timeout_generation_.load() != generation) {
                return;
            }
            // 仍在录音且 generation 未变 = button_up/audio_end 都没到 = 卡死。
            stale = active_session_id_.has_value();
            if (stale) {
                wechat_active = wechat_input_method_active_;
            }
        }
        if (!stale) return;
        LogCoordinatorLine("recording hard timeout; canceling stuck session");
        if (wechat_active) {
            // wechat 模式：走专用停止路径（停 renderer/松热键/切回设备/清 wechat_active）+ EnterReady。
            StopWechatInputMethodSession();
            EnterReady("wechat_hard_timeout");
        } else {
            CancelShortRecording();
        }
    }).detach();
}

void VoiceStickCoordinator::CancelRecordingHardTimeout() {
    recording_hard_timeout_generation_.fetch_add(1);
}

// 音频流停滞兜底：focused_app 录音中固件 25fps 持续发帧，超过 audio_stall_timeout_ 一帧未收
// 说明 button_up 与 audio_end 双丢或链路卡死。此时走 audio_end 等待路径收尾（给迟到的 END 帧
// kAudioEndTimeout 机会后按已有缓冲 finalize），不再干等 120s 硬超时。每 500ms 检查一次；
// 状态离开 kRecording 或 generation 变化即退出。
void VoiceStickCoordinator::ScheduleRecordingStallWatchdog() {
    last_audio_frame_ms_.store(SteadyNowMs());
    const auto generation = recording_stall_generation_.fetch_add(1) + 1;
    // 捕获调度时的会话 id：wechat 会话同样置 kRecording（"wechat_primary_down"）但不走
    // 该 watchdog，若上一个 focused 会话的残留线程在 wechat 录音期间醒来，凭会话 id
    // 不匹配 + wechat 激活态双重校验退出，避免误触发 BeginWaitingForAudioEnd 污染 wechat 会话。
    const auto session_id = active_session_id_;
    std::thread([this, alive = alive_, generation, session_id]() {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!alive->load()) return;
            {
                std::lock_guard lock(audio_mutex_);
                if (recording_stall_generation_.load() != generation ||
                    session_state_ != SessionState::kRecording ||
                    active_session_id_ != session_id ||
                    wechat_input_method_active_) {
                    return;
                }
                if (SteadyNowMs() - last_audio_frame_ms_.load() < audio_stall_timeout_.count()) {
                    continue;
                }
            }
            LogCoordinatorLine("audio stall timeout; finalizing buffered audio");
            BeginWaitingForAudioEnd("audio_stall");
            return;
        }
    }).detach();
}

// finalizing 闲置兜底：等 ASR final / LLM 翻译或精修期间，链路层对 receive 超时静默重等
// （asr_client_win ReceiveOneReusable），服务端不回 SessionFinished 时会永久卡 Processing。
// 这里按「无进展时长」判活：ASR partial/segment 与精修 token 都会刷新活动时间，连续
// finalizing_timeout_ 无任何进展才兜底——有 ASR 原文回退粘贴原文，否则报错进 error 态。
// 每 200ms 检查一次；离开 kFinalizing 或 generation 变化即退出，无需显式 cancel。
void VoiceStickCoordinator::ScheduleFinalizingWatchdog() {
    finalizing_last_activity_ms_.store(SteadyNowMs());
    const auto generation = finalizing_watchdog_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation]() {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!alive->load()) return;
            std::string fallback_text;
            {
                std::lock_guard lock(audio_mutex_);
                if (finalizing_watchdog_generation_.load() != generation ||
                    session_state_ != SessionState::kFinalizing) {
                    return;
                }
                if (SteadyNowMs() - finalizing_last_activity_ms_.load() <
                    finalizing_timeout_.count()) {
                    continue;
                }
                fallback_text = finalizing_fallback_text_;
            }
            if (!fallback_text.empty()) {
                // LLM 翻译/精修无响应：回退粘贴 ASR 原文，不丢本次输入。
                LogCoordinatorLine("finalizing watchdog timeout; pasting unrefined final text");
                EnterPendingConfirmation(fallback_text, "finalizing_watchdog");
            } else {
                // ASR 服务端始终未回 final：报错退出（用户确认后回 ready），不永久卡住。
                LogCoordinatorLine("finalizing watchdog timeout; no final text, aborting");
                FinishWithAsrError("ASR response timeout");
            }
            return;
        }
    }).detach();
}

void VoiceStickCoordinator::TouchFinalizingWatchdog() {
    finalizing_last_activity_ms_.store(SteadyNowMs());
}

void VoiceStickCoordinator::LogWechatLatency(std::string_view stage) {
    if (!wechat_latency_anchor_.has_value()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *wechat_latency_anchor_).count();
    LogCoordinatorLine("wechat latency: " + std::string(stage) + " +" +
                       std::to_string(elapsed) + "ms");
}

void VoiceStickCoordinator::FinishWithAsrError(const std::string& message) {
    CancelAudioEndTimeout();
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
    const bool should_press_enter = config_.auto_enter;
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("paste_complete");
    input_injector_->Paste(text, should_press_enter);
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation(std::optional<std::string> device_id) {
    if (!pending_paste_state_.IsIdle() || session_state_ != SessionState::kReady ||
        active_session_id_.has_value() || !last_recoverable_text_.has_value()) {
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
    if (active_session_id_.has_value()) {
        if (active_device_id_ == device_id) {
            CancelRecognitionInProgress();
        }
        return;
    }
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
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
        EnterReady("cancel_recognition_wechat");
        return;
    }
    active_session_id_.reset();
    active_session_started_at_ = {};
    CancelAudioEndTimeout();
    CancelStreamingRefinement();
    asr_->Cancel();
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_recognition");
}

void VoiceStickCoordinator::CancelActiveCycleIfDeviceDisconnected() {
    if (is_shutdown_) return;
    // 断连设备的体感态必须清理，否则残留激活会拦截重连后的主键录音。
    for (auto it = air_mouse_active_devices_.begin(); it != air_mouse_active_devices_.end();) {
        if (!ble_->IsConnected(*it)) {
            const std::string disconnected_id = *it;
            it = air_mouse_active_devices_.erase(it);
            air_mouse_states_.erase(disconnected_id);
            ble_->SendAirMouseEnabled(false, disconnected_id);
            LogCoordinatorLine("air mouse disabled on VS-" + disconnected_id + " (disconnected)");
            if (on_air_mouse_active_changed) on_air_mouse_active_changed(!air_mouse_states_.empty());
        } else {
            ++it;
        }
    }
    for (auto it = subtitle_cycles_.begin(); it != subtitle_cycles_.end();) {
        const auto& device_id = it->first.first;
        if (!ble_->IsConnected(device_id)) {
            if (it->second->asr) it->second->asr->Cancel();
            it->second->debug_audio_recorder.Discard();
            ui_->HideOverlay();
            active_subtitle_sessions_.erase(device_id);
            it = subtitle_cycles_.erase(it);
        } else {
            ++it;
        }
    }
    if (active_device_id_.has_value() && !ble_->IsConnected(*active_device_id_)) {
        if (IsWechatInputMethodActive()) {
            // wechat 模式断连必须走专用停止路径，否则 renderer/热键/wechat_active 残留，
            // 重连后 button_up 条件不匹配、button_down 被残留 active 忽略，卡在 Recording。
            StopWechatInputMethodSession();
            EnterReady("wechat_device_disconnected");
            return;
        }
        if (waiting_for_audio_end_.load()) {
            SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
            return;
        }
        asr_->Cancel();
        pending_paste_state_ = {};
        active_session_id_.reset();
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("device_disconnected");
    }
}

void VoiceStickCoordinator::CancelStreamingRefinement() {
    if (refinement_cancel_token_) {
        refinement_cancel_token_->store(true);
        refinement_cancel_token_.reset();
    }
}

void VoiceStickCoordinator::FinishRecognitionCycle() {
    CancelAudioEndTimeout();
    CancelStreamingRefinement();
    asr_started_ = false;
    sent_final_audio_chunk_ = false;
    pasted_final_text_ = false;
    finalizing_fallback_text_.clear();
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
    CancelRecordingHardTimeout();
    SetSessionState(SessionState::kReady, reason);
    ui_->SetStatus("Ready");
    SendUiStateForActiveDevice("ready");
    if (hide_overlay) ui_->HideOverlay();
    active_device_id_.reset();
}

void VoiceStickCoordinator::EnterFinalizing(std::string_view reason) {
    CancelRecordingHardTimeout();
    SetSessionState(SessionState::kFinalizing, reason);
    ScheduleFinalizingWatchdog();
    ui_->SetStatus("Processing");
    SendUiStateForActiveDevice("thinking");
}

void VoiceStickCoordinator::EnterPendingConfirmation(const std::string& text, std::string_view reason) {
    // 只在 kFinalizing 下接受粘贴完成：watchdog 兜底或用户取消已离开 finalizing 后，
    // 迟到的 translate/refine 完成回调不得二次粘贴。
    if (session_state_ != SessionState::kFinalizing) {
        LogCoordinatorLine("ignore stale pending confirmation reason=" + std::string(reason));
        return;
    }
    CompletePendingPaste(text);
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

OutputProfile VoiceStickCoordinator::OutputProfileForDevice(const std::optional<std::string>& device_id) const {
    return config_.OutputProfileForDevice(device_id);
}

InteractionMode VoiceStickCoordinator::InteractionModeToSend() const {
    // wechat 模式按其专属触发模式（trigger_mode，与全局 interaction_mode 解耦）决定下发：
    //   hold -> hold_to_talk_instant（按下即录音跳过 300ms 阈值，降低弹框延迟）
    //   click -> click_to_talk
    // 非 wechat 模式（focused_app/字幕）仍下发全局 interaction_mode（托盘菜单控制），
    // 不被 wechat 的点按式选择污染。
    if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) {
        return config_.wechat_input_method.trigger_mode == InteractionMode::kHoldToTalk
                   ? InteractionMode::kHoldToTalkInstant
                   : InteractionMode::kClickToTalk;
    }
    return config_.interaction_mode;
}

OverlayThemeColor VoiceStickCoordinator::ThemeColorForDevice(const std::string& device_id) const {
    return ThemeColorForConfig(config_, device_id);
}

OverlayThemeColor VoiceStickCoordinator::ThemeColorForConfig(const AppConfig& config,
                                                             const std::string& device_id) {
    auto it = config.device_theme_colors.find(device_id);
    return it == config.device_theme_colors.end() ? DefaultOverlayThemeColor() : it->second;
}

bool VoiceStickCoordinator::ShouldUseDefiniteSegments(const OutputProfile& profile) const {
    return profile.target == OutputTarget::kSubtitle &&
           config_.interaction_mode == InteractionMode::kClickToTalk;
}

double VoiceStickCoordinator::CurrentRecordingDurationSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - active_session_started_at_).count();
}

bool VoiceStickCoordinator::ShouldDiscardWechatRecording() const {
    // 与 focused_app/subtitle 路径对齐：零帧或短于最小录音时长的会话不落盘调试音频，
    // 避免 hold_to_talk_instant 模式下无意点按 / button_up 抢跑产生仅含 ogg 头的极小文件。
    if (received_audio_frames_ == 0) return true;
    return CurrentRecordingDurationSeconds() < kMinimumRecordingDurationSeconds;
}

std::optional<std::string> VoiceStickCoordinator::ResolveHotkeyTargetDevice() const {
    if (active_device_id_.has_value() &&
        std::find(connected_device_ids_.begin(), connected_device_ids_.end(), *active_device_id_) !=
            connected_device_ids_.end()) {
        return active_device_id_;
    }
    for (const auto& device_id : paired_device_ids_) {
        if (std::find(connected_device_ids_.begin(), connected_device_ids_.end(), device_id) !=
            connected_device_ids_.end()) {
            return device_id;
        }
    }
    if (!connected_device_ids_.empty()) {
        return connected_device_ids_.front();
    }
    return std::nullopt;
}

void VoiceStickCoordinator::HandleGlobalHotkeyPressed() {
    if (hotkey_is_down_) {
        LogApp("hotkey pressed but already down, skipping");
        return;
    }

    LogApp("hotkey pressed, resolving target device...");
    LogApp("  connected_device_ids: " + std::to_string(connected_device_ids_.size()));
    for (const auto& id : connected_device_ids_) {
        LogApp("    - VS-" + id);
    }
    LogApp("  active_device_id: " + (active_device_id_.has_value() ? "VS-" + *active_device_id_ : "none"));

    auto target_device = ResolveHotkeyTargetDevice();
    if (!target_device) {
        if (paired_device_ids_.empty()) {
            ui_->SetStatus("Hotkey: pair a VoiceStick first");
            if (config_.debug_audio_cache) {
                ui_->ShowNotification("热键触发失败", "请先配对 VoiceStick 设备");
            }
        } else {
            ui_->SetStatus("Hotkey: VoiceStick not connected; press the main button to wake it");
            if (config_.debug_audio_cache) {
                ui_->ShowNotification("热键触发失败", "设备可能已休眠，请按主键唤醒后重试。");
            }
        }
        LogApp("hotkey pressed but no connected device");
        return;
    }

    LogApp("  resolved target device: VS-" + *target_device);

    const auto request_id = next_hotkey_request_id_++;
    if (config_.interaction_mode == InteractionMode::kHoldToTalk) {
        hotkey_is_down_ = true;
        hotkey_active_device_id_ = target_device;
    }
    LogApp("  sending remote_button_down to VS-" + *target_device + ", request_id=" + std::to_string(request_id));
    ble_->SendRemoteButton(RemoteButtonAction::kDown, "primary", target_device, request_id);
    ui_->SetStatus("Recording (hotkey) on VS-" + *target_device);
    if (config_.debug_audio_cache) {
        ui_->ShowNotification("热键已触发", "正在 VS-" + *target_device + " 上启动录音，松开热键结束识别");
    }
    LogApp("hotkey pressed, starting recording on VS-" + *target_device);
}

void VoiceStickCoordinator::HandleGlobalHotkeyReleased() {
    if (config_.interaction_mode == InteractionMode::kClickToTalk) {
        return;
    }

    if (!hotkey_is_down_) return;

    auto target_device = hotkey_active_device_id_;
    hotkey_is_down_ = false;
    hotkey_active_device_id_.reset();

    if (target_device && ble_->IsConnected(*target_device)) {
        const auto request_id = next_hotkey_request_id_++;
        ble_->SendRemoteButton(RemoteButtonAction::kUp, "primary", target_device, request_id);
        LogApp("hotkey released, stopping recording on VS-" + *target_device);
    }
}

} // namespace voicestick
