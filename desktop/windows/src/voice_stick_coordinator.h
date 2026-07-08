#pragma once

#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "ble_protocol.h"
#include "default_audio_device_controller.h"
#include "debug_audio_recorder.h"
#include "device_switch_state.h"
#include "firmware_manifest.h"
#include "llm_translation_client.h"
#include "llm_refinement_client.h"
#include "ogg_opus_muxer.h"
#include "pcm_ring_buffer.h"
#include "virtual_mic_renderer.h"
#include "wasapi_virtual_mic_renderer.h"
#include "wechat_input_method_hotkey.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace voicestick {

enum class RemoteButtonAction {
    kDown,
    kUp,
};

struct ConnectedDevice {
    std::string id;
    std::string name;
};

struct DeviceInfo {
    std::string device_id;
    std::string hardware;
    std::string firmware_version;
};

struct FirmwareUpdateProgress {
    int written_bytes = 0;
    int total_bytes = 0;
    bool is_device_confirmed = false;
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
    virtual void SendInteractionMode(InteractionMode mode,
                                     const std::optional<std::string>& device_id) = 0;
    virtual void SendShowImuDebug(bool enabled,
                                  const std::optional<std::string>& device_id) = 0;
    virtual void SendTapEnabled(bool enabled,
                                const std::optional<std::string>& device_id) = 0;
    virtual void SendTapSensitivity(int level,
                                    const std::optional<std::string>& device_id) = 0;
    // 开关体感鼠标模式：enabled=true 时固件校准陀螺仪零偏并开始上报 motion 帧。
    virtual void SendAirMouseEnabled(bool enabled,
                                     const std::optional<std::string>& device_id) = 0;
    virtual void SendImuWakeSensitivity(int threshold_lsb,
                                        const std::optional<std::string>& device_id) = 0;
    virtual void RequestBatteryStatus(const std::optional<std::string>& device_id) = 0;
    virtual void SendRemoteButton(RemoteButtonAction action,
                                  const std::string& button,
                                  const std::optional<std::string>& device_id,
                                  std::uint32_t request_id) = 0;
    virtual void UpdateFirmware(ByteVector image,
                                const std::string& device_id,
                                std::function<void(FirmwareUpdateProgress)> progress,
                                std::function<void(bool, std::string)> completion) = 0;
    virtual void CancelFirmwareUpdate() = 0;
    virtual bool IsConnected(const std::string& device_id) const = 0;
    virtual void CancelPendingConnect(const std::string& device_id) {}
    virtual void Shutdown() {}

    std::function<void(std::vector<ConnectedDevice>)> on_connection_change;
    std::function<void(std::string, std::string)> on_connection_error;
    std::function<void(std::string)> on_scan_error;
    std::function<void(std::string, StateEvent)> on_state_event;
    std::function<void(std::string, AudioFrame)> on_audio_frame;
    // 体感鼠标运动帧回调：(device_id, MotionEvent)。
    std::function<void(std::string, MotionEvent)> on_motion_event;
};

class AsrClient {
public:
    virtual ~AsrClient() = default;
    virtual bool Start(AsrSessionOptions options = {}) = 0;
    virtual void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) = 0;
    virtual void Cancel() = 0;
    virtual std::string LastStartError() const { return {}; }

    std::function<void(std::string)> on_partial;
    std::function<void(AsrSegment)> on_segment;
    std::function<void(std::string)> on_final;
    std::function<void(std::string)> on_error;
    std::function<void(std::string, std::string)> on_upgrade_url;
};

class VoiceStickUi {
public:
    virtual ~VoiceStickUi() = default;
    virtual void SetStatus(const std::string& status) = 0;
    virtual void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) = 0;
    virtual void SetDeviceInfo(const DeviceInfo& info) = 0;
    virtual void SetDeviceBattery(const std::string& device_id, int level_percent,
                                   bool charging, bool usb_powered) = 0;
    virtual void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) = 0;
    virtual void SetPairingError(const std::string& device_id, const std::string& message) = 0;
    virtual void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                          const std::string& current_version,
                                          const std::string& latest_version,
                                          bool is_below_minimum) = 0;
    virtual void SetPairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void SetHasRecoverableInput(bool has_recoverable_input) = 0;
    virtual void ShowListening(const std::optional<std::string>& device_id) = 0;
    virtual void ShowPartial(const std::string& text, const std::optional<std::string>& device_id) = 0;
    // 流式精修追加：与 ShowPartial 类似但不触发文字滚动过渡动画，供流式 token 高频追加使用。
    virtual void AppendPartial(const std::string& text, const std::optional<std::string>& device_id) = 0;
    // 进入精修态：切到 kRefining 模式并立即显示 ASR 原文（带闪烁光标），让用户在 LLM
    // 首 token 到达前就能看到识别结果，消除"卡住空白"感。精修流式 token 随后经 AppendPartial 覆盖。
    virtual void ShowRefining(const std::string& text, const std::optional<std::string>& device_id) = 0;
    virtual void ShowFinalCountdown(const std::string& text,
                                    const std::optional<std::string>& device_id,
                                    std::function<void()> on_complete) = 0;
    virtual void ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) = 0;
    virtual void ShowError(const std::string& text,
                           const std::optional<std::string>& device_id,
                           std::function<void()> on_complete) = 0;
    virtual void ShowCloudUpgrade(const std::string& message,
                                  const std::string& url,
                                  const std::optional<std::string>& device_id) = 0;
    virtual void HideOverlay(std::function<void()> on_hidden = {}) = 0;
    virtual void ShowSubtitle(const std::string& text,
                              const std::string& device_id,
                              OverlayThemeColor color) = 0;
    virtual void HideSubtitles() = 0;
    virtual void ShowNotification(const std::string& title, const std::string& body) = 0;
};

class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual void Paste(const std::string& text, bool press_enter) = 0;
    virtual void SendEnter() = 0;
    // 注入一次下方向键，用于敲击手势在候选/选项间向下切换。
    virtual void SendArrowDown() = 0;
    // 体感鼠标：相对移动光标 (dx 右为正, dy 下为正)。
    virtual void MoveMouse(int dx, int dy) = 0;
    // 体感鼠标：模拟鼠标左键单击（按下+抬起）。
    virtual void ClickLeftButton() = 0;
};

// 探测前台窗口所属进程是否高于本进程完整性。高权限前台时 SendInput 注入会被 UIPI 静默丢弃，
// 协调器据此提醒用户提权运行。接口在 core，平台实现（Win32 OpenProcess）由外壳注入。
class IForegroundProcessProbe {
public:
    virtual ~IForegroundProcessProbe() = default;
    // 前台进程高于本进程完整性时返回 true，并填入其可执行文件名（如 "Weixin.exe"）供提醒文案。
    virtual bool IsForegroundHigherIntegrity(std::wstring& process_name) = 0;
};

class VoiceStickCoordinator {
public:
    VoiceStickCoordinator(AppConfig config,
                          std::unique_ptr<BleCentral> ble,
                          std::unique_ptr<AsrClient> asr,
                          VoiceStickUi* ui,
                          InputInjector* input_injector,
                          std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory = {},
                          std::function<std::unique_ptr<IVirtualMicRenderer>(const IVirtualMicRenderer::Options&)> wechat_renderer_factory = {},
                          std::function<std::unique_ptr<IWechatInputMethodHotkey>(const std::string&)> wechat_hotkey_factory = {},
                          std::function<std::unique_ptr<IDefaultAudioDeviceController>()> wechat_device_switcher_factory = {},
                          std::filesystem::path device_switch_state_path = {},
                          std::chrono::milliseconds recording_hard_timeout = kRecordingHardTimeout);
    ~VoiceStickCoordinator();

    void Start();
    void Shutdown();
    // 注入前台进程完整性探测实现。未注入（nullptr）时跳过 UIPI 提权提醒。须在 Start 前调用。
    void SetForegroundProbe(std::unique_ptr<IForegroundProcessProbe> probe);
    void UpdateConfig(AppConfig config);
    // 热调参：仅更新运行期 air_mouse 参数（轻量，不存盘不重建 LLM）。调参窗口即时调。
    void UpdateAirMouseParams(const AirMouseParams& params);
    // 取当前运行期 air_mouse 参数（调参窗口初始值）。
    AirMouseParams GetAirMouseParamsForTuning() const { return live_air_mouse_params_; }
    void ReconnectPairedDevices();
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name);
    void ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids);
    void RemovePairedDevice(const std::string& device_id);
    void CancelPendingConnect(const std::string& device_id);
    bool RestoreLastInputConfirmation();
    void CheckFirmwareUpdatesNow();
    void CheckFirmwareAfterPairing(const std::string& device_id);
    void UpdateFirmwareFromLatest(const std::string& device_id,
                                  std::function<void(FirmwareUpdateProgress)> progress,
                                  std::function<void(bool, std::string)> completion);
    void CancelFirmwareUpdate();

    static OverlayThemeColor ThemeColorForConfig(const AppConfig& config, const std::string& device_id);

    void HandleGlobalHotkeyPressed();
    void HandleGlobalHotkeyReleased();
    // 体感鼠标 60Hz tick：由平台层定时器驱动，对每个激活设备做速度环 step 并注入光标位移。
    void AirMouseTick();
    // 体感鼠标激活态变化通知（true=有设备进入体感，false=全部退出）。平台层据此启停定时器。
    std::function<void(bool)> on_air_mouse_active_changed;
    // 查询某设备是否处于体感鼠标模式（供托盘菜单提示，避免用户不知情下主键变鼠标左键）。
    bool IsAirMouseActive(const std::string& device_id) const;

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

    struct SubtitleCycle {
        std::string device_id;
        std::uint32_t session_id = 0;
        std::chrono::steady_clock::time_point started_at;
        std::unique_ptr<AsrClient> asr;
        OggOpusMuxer ogg_muxer{16000, 1};
        DebugAudioRecorder debug_audio_recorder{false, {}};
        int received_audio_frames = 0;
        std::optional<std::uint32_t> last_audio_seq;
        std::vector<ByteVector> buffered_ogg_chunks;
        bool asr_started = false;
        bool sent_final_audio_chunk = false;
        bool finished_final_text = false;
        bool waiting_for_audio_end = false;
        std::uint64_t audio_end_wait_generation = 0;
    };

    void ConfigureAsrCallbacks();
    void ConfigureSubtitleAsrCallbacks(SubtitleCycle* cycle);
    void HandleStateEvent(const StateEvent& event, const std::string& device_id);
    void HandleButtonDown(const StateEvent& event, const std::string& device_id);
    void HandleButtonUp(const StateEvent& event, const std::string& device_id);
    void HandleButtonClick(const StateEvent& event, const std::string& device_id);
    void HandleButtonDoubleClick(const StateEvent& event, const std::string& device_id);
    void HandleTapEvent(const StateEvent& event, const std::string& device_id);
    void HandleMotionEvent(const MotionEvent& event, const std::string& device_id);
    // 体感鼠标模式是否对该设备开启。返回切换后的状态（true=进入，false=退出）。
    bool ToggleAirMouse(const std::string& device_id);
    void HandleSecondaryButtonClick(const std::string& device_id);
    void HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandlePrimaryButtonUp(const std::string& device_id);
    void HandleAudioFrame(const AudioFrame& frame, const std::string& device_id);
    void HandleWechatInputMethodPrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                   const std::string& device_id);
    void HandleWechatInputMethodPrimaryButtonUp(const std::string& device_id);
    void HandleWechatInputMethodAudioFrame(const AudioFrame& frame, const std::string& device_id);
    bool StartWechatInputMethodSession(std::optional<std::uint32_t> session_id,
                                       const std::string& device_id);
    void StopWechatInputMethodSession();
    // 检测前台是否高权限进程，若是则气泡提醒（按进程名去重防打扰）。返回 true 表示已检测到高权限
    // 前台、调用方应跳过本次会话启动（SendInput 必被 UIPI 丢弃，启动无意义且会留空转残留）。
    bool MaybeWarnForegroundElevated(const std::string& device_id);
    // 兜底落盘本次 wechat 会话的调试音频：未收到 audio_end 则补 EOS 页再 Finish。
    void FinishWechatInputMethodRecording();
    bool IsWechatInputMethodActive() const;
    void HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandleSubtitlePrimaryButtonUp(const std::string& device_id);
    void HandleSubtitleAudioFrame(const AudioFrame& frame, const std::string& device_id);
    void BeginWaitingForSubtitleAudioEnd(SubtitleCycle* cycle, std::string_view reason);
    void ScheduleSubtitleAudioEndTimeout(const std::string& device_id, std::uint32_t session_id);
    void CancelSubtitleAudioEndTimeout(SubtitleCycle* cycle);
    void FinishSubtitleAudioInput(SubtitleCycle* cycle);
    void SendSubtitleFinalOggChunkIfNeeded(const std::string& device_id, std::uint32_t session_id);
    void SendOrBufferSubtitleOggChunk(SubtitleCycle* cycle, const ByteVector& chunk,
                                      bool is_last, bool can_start_asr);
    bool StartSubtitleAsrAndFlushBufferedChunks(SubtitleCycle* cycle, bool last_chunk_is_final);
    void HandleDefiniteSegment(const AsrSegment& segment);
    void HandleSubtitleDefiniteSegment(const AsrSegment& segment, const std::string& device_id);
    void FinishSubtitleCycleWithFinalText(const std::string& device_id, std::uint32_t session_id,
                                          const std::string& text);
    void FinishSubtitleCycleWithError(const std::string& device_id, std::uint32_t session_id,
                                      const std::string& message);
    void CancelSubtitleCycle(const std::string& device_id, std::string_view reason);
    void FinishSubtitleCycle(const std::string& device_id, std::uint32_t session_id, bool hide_overlay);
    bool ShouldHideOverlayForFinishedSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool CanUpdateOverlayForSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool ShouldSendPartialToDevice() const;
    bool ShouldSendSubtitlePartialToDevice(const SubtitleCycle* cycle) const;
    SubtitleCycle* FindSubtitleCycle(const std::string& device_id, std::uint32_t session_id);
    SubtitleCycle* FindActiveSubtitleCycle(const std::string& device_id);
    bool IsActiveSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool HasActiveSubtitleSession(const std::string& device_id) const;
    void ClearActiveSubtitleSession(const std::string& device_id, std::uint32_t session_id);
    void CancelSubtitleCyclesForDevice(const std::string& device_id, std::string_view reason);
    void ShowSubtitleText(const std::string& text, const OutputProfile& profile, const std::string& device_id,
                          std::function<void(bool)> completion = {});
    void TransformText(const std::string& text, const OutputProfile& profile,
                       std::function<void(bool, std::string)> completion);
    void BeginWaitingForAudioEnd(std::string_view reason);
    void ScheduleAudioEndTimeout(std::optional<std::uint32_t> session_id,
                                 std::optional<std::string> device_id);
    void CancelAudioEndTimeout();
    // recording 硬超时兜底：button_down 进 recording 时启动，button_up/audio_end/断连/取消时取消。
    // 超时未收到结束信号则 CancelShortRecording 回 ready，覆盖 button_up 与 audio_end 同时丢失的卡死。
    void ScheduleRecordingHardTimeout();
    void CancelRecordingHardTimeout();
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
    void CancelStreamingRefinement();
    void UpdateDeviceFirmwareInfo(const StateEvent& event, const std::string& device_id);
    void CheckFirmwareUpdatesIfNeeded(bool force, bool show_errors);
    void RefreshFirmwareAvailability();
    void SetFirmwareChecking(bool is_checking);
    bool ShouldShowFirmwareUpdatePromptAfterPairing(const std::string& device_id,
                                                    const DeviceFirmwareInfo& info);
    bool IsWaitingForFinalText() const;
    void SetSessionState(SessionState state, std::string_view reason);
    void EnterReady(std::string_view reason, bool hide_overlay = true);
    void EnterFinalizing(std::string_view reason);
    void EnterPendingConfirmation(const std::string& text, std::string_view reason);
    void EnterPausedConfirmation(const std::string& text, std::string_view reason);
    void EnterError(const std::string& message, std::string_view reason);
    void RefreshDeviceUiState(const std::string& device_id);
    void SendUiStateForActiveDevice(const std::string& state, const std::string& text = "");
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
    // 返回要下发给固件的交互模式：wechat 模式 + hold_to_talk 时派生为 kHoldToTalkInstant
    // （按下即录音，跳过 300ms 阈值，降低按下到弹框延迟），其余按用户配置原样下发。
    InteractionMode InteractionModeToSend() const;
    OverlayThemeColor ThemeColorForDevice(const std::string& device_id) const;
    bool ShouldUseDefiniteSegments(const OutputProfile& profile) const;
    double CurrentRecordingDurationSeconds() const;
    std::optional<std::string> ResolveHotkeyTargetDevice() const;

    AppConfig config_;
    std::unique_ptr<BleCentral> ble_;
    std::unique_ptr<AsrClient> asr_;
    std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory_;
    LLMTranslationClient translator_;
    LLMRefinementClient refiner_;
    VoiceStickUi* ui_;
    InputInjector* input_injector_;
    std::mutex audio_mutex_;
    OggOpusMuxer ogg_muxer_{16000, 1};
    DebugAudioRecorder debug_audio_recorder_;
    SessionState session_state_ = SessionState::kReady;
    std::optional<std::uint32_t> active_session_id_;
    std::optional<std::string> active_device_id_;
    std::chrono::steady_clock::time_point active_session_started_at_;
    // 敲击注入方向键的节流时间戳：两次注入最短间隔 500ms，避免连击导致光标连续下移。
    std::chrono::steady_clock::time_point last_tap_inject_at_{};
    // 体感鼠标当前处于激活态的设备集合（按 device_id）。空表示无设备在体感态。
    std::set<std::string> air_mouse_active_devices_;
    // 体感鼠标每设备运动学状态（速度 v + 相对角度 theta + 最近 omega 采样与时间戳）。
    struct AirMouseDeviceState {
        AirMouseKinState kin;
        std::int16_t last_omega_x = 0;
        std::int16_t last_omega_y = 0;
        std::chrono::steady_clock::time_point last_omega_t;
        double theta_x = 0.0;  // 相对中立姿态的偏转角（角速度积分）
        double theta_y = 0.0;
    };
    std::map<std::string, AirMouseDeviceState> air_mouse_states_;
    AirMouseParams live_air_mouse_params_;  // 运行期参数（AirMouseTick 用，热调参面板经 UpdateAirMouseParams 即时改）
    AirMouseParams AirMouseParamsFromConfig() const;
    static constexpr std::chrono::milliseconds kAirMouseTickInterval{16};   // ~60Hz
    // omega 超龄视为静止。固件约 50Hz（20ms/帧），桌面 60Hz（16.7ms）；
    // 取 ≥3× 帧周期（60ms）以容忍 50/60Hz 抖动与偶发丢帧，绝不误触发归零（P1 时间基准统一）。
    static constexpr std::chrono::milliseconds kAirMouseOmegaStaleAge{80};
    // 角度控制模型常量：相对角度限幅与中立死区。
    static constexpr double kAirMouseMaxTheta = 100.0;      // theta 上限，防积分异常累积
    static constexpr double kAirMouseAngleDeadzone = 0.5;   // |theta| 与 |omega| 均小于此值时归零
    static constexpr double kAirMouseOmegaDeadzone = 2.0;   // omega 死区（固件 REPORT_GAIN=4 单位，≈0.5dps）
    int received_audio_frames_ = 0;
    std::optional<std::uint32_t> last_audio_seq_;
    std::vector<ByteVector> buffered_ogg_chunks_;
    bool asr_started_ = false;
    bool sent_final_audio_chunk_ = false;
    bool pasted_final_text_ = false;
    std::atomic_bool waiting_for_audio_end_{false};
    std::atomic_uint64_t audio_end_wait_generation_{0};
    // recording 硬超时兜底（可经构造注入，测试用短值；默认 kRecordingHardTimeout）。
    std::chrono::milliseconds recording_hard_timeout_{kRecordingHardTimeout};
    std::atomic_uint64_t recording_hard_timeout_generation_{0};
    PendingPasteState pending_paste_state_;
    std::optional<std::string> last_recoverable_text_;
    std::optional<std::string> last_recoverable_device_id_;
    std::vector<std::string> paired_device_ids_;
    std::vector<std::string> connected_device_ids_;
    bool hotkey_is_down_ = false;
    std::optional<std::string> hotkey_active_device_id_;
    std::uint32_t next_hotkey_request_id_ = 1;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id_;
    std::optional<FirmwareManifest> latest_firmware_manifest_;
    std::chrono::steady_clock::time_point last_firmware_manifest_check_at_{};
    bool has_last_firmware_manifest_check_at_ = false;
    bool firmware_manifest_check_in_flight_ = false;
    std::set<std::string> pending_firmware_update_prompt_device_ids_;
    FirmwareManifestClient firmware_manifest_client_;
    std::mutex firmware_mutex_;
    std::shared_ptr<std::atomic_bool> alive_{std::make_shared<std::atomic_bool>(true)};
    std::shared_ptr<std::atomic_bool> refinement_cancel_token_;
    std::thread firmware_manifest_thread_;
    bool is_showing_asr_error_ = false;
    bool is_shutdown_ = false;
    std::map<std::pair<std::string, std::uint32_t>, std::unique_ptr<SubtitleCycle>> subtitle_cycles_;
    std::map<std::string, std::uint32_t> active_subtitle_sessions_;
    // wechat_input_method 模式下的当前会话资源。
    std::unique_ptr<AudioOpusDecoder> wechat_decoder_;
    std::unique_ptr<PcmRingBuffer> wechat_ring_buffer_;
    std::unique_ptr<IVirtualMicRenderer> wechat_renderer_;
    std::unique_ptr<IWechatInputMethodHotkey> wechat_hotkey_;
    // 前台进程完整性探测（外壳注入；nullptr 时跳过 UIPI 提权提醒）。
    std::unique_ptr<IForegroundProcessProbe> foreground_probe_;
    // 已提醒过提权的前台进程名（按进程名去重，避免同一高权限程序重复弹气泡）。
    std::optional<std::wstring> elevation_warned_process_;
    bool wechat_input_method_active_ = false;
    // 是否已收到本次 wechat 会话的 audio_end 帧；用于 Stop 时判断是否需补 EOS 页。
    bool wechat_audio_end_received_ = false;
    // 是否已对本次会话发送 SendDown（首帧 Opus 解码成功才发送）；决定 Stop 是否配对 SendUp。
    bool wechat_hotkey_sent_down_ = false;
    // 工厂注入（测试用 fake 解耦真实 WASAPI/SendInput）；默认空→make_unique 真实实现。
    std::function<std::unique_ptr<IVirtualMicRenderer>(const IVirtualMicRenderer::Options&)> wechat_renderer_factory_;
    std::function<std::unique_ptr<IWechatInputMethodHotkey>(const std::string&)> wechat_hotkey_factory_;
    // 自动切换默认录音设备：录音期切 eConsole 到 CABLE Output，松开切回。角色分离只切 eConsole，
    // eCommunications 保持真实麦不动。factory 为空时生产 COM 实现后续接入（暂 nullptr 降级）。
    std::function<std::unique_ptr<IDefaultAudioDeviceController>()> wechat_device_switcher_factory_;
    std::unique_ptr<IDefaultAudioDeviceController> wechat_device_switcher_;
    // Start 时记录的原 eConsole 默认设备 ID（有值=当前已切到 CABLE，Stop 切回此 ID）。
    std::optional<std::wstring> saved_default_capture_id_;
    // 持久化切换状态供崩溃自愈；空时用 config 目录推导。Start 检测残留 Restore，
    // 切换成功后 Save，切回后 Clear。
    std::filesystem::path device_switch_state_path_;
    std::filesystem::path DeviceSwitchStatePath() const;
    void RecoverDeviceSwitchStateIfNeeded();
    static constexpr double kMinimumRecordingDurationSeconds = 0.5;
    static constexpr std::chrono::milliseconds kAudioEndTimeout{1000};
    // recording 硬上限：button_up 与 audio_end 都丢失时的兜底，避免永久卡 listening。
    static constexpr std::chrono::seconds kRecordingHardTimeout{120};
    static constexpr std::chrono::hours kFirmwareManifestCacheDuration{24};
};

} // namespace voicestick
