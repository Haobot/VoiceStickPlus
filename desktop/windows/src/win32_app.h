#pragma once

#include "app_config.h"
#include "cmd_line.h"
#include "firmware_update_dialog.h"
#include "global_hotkey_win.h"
#include "hotkey_settings_dialog.h"
#include "input_injector_win.h"
#include "onboarding_dialog.h"
#include "overlay_window.h"
#include "pair_device_dialog.h"
#include "selection_hotword_manager.h"
#include "settings_dialog.h"
#include "air_mouse_tuning_window.h"
#include "subtitle_window.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace voicestick {

// WM_COPYDATA.dwData 标识：命令行 --ota 实例转发给已运行实例的 OTA 请求。'VSOT'。
constexpr ULONG_PTR kOtaCopyDataId = 0x56534F54;

} // namespace voicestick
#include <vector>

namespace voicestick {

struct DeviceBattery {
    int level_percent = 0;
    bool charging = false;
    bool usb_powered = false;
};

class Win32App : public VoiceStickUi {
public:
    explicit Win32App(HINSTANCE instance);
    int Run();

    void SetStatus(const std::string& status) override;
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override;
    void SetDeviceInfo(const DeviceInfo& info) override;
    void SetDeviceBattery(const std::string& device_id, int level_percent,
                           bool charging, bool usb_powered) override;
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) override;
    void SetPairingError(const std::string& device_id, const std::string& message) override;
    void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                  const std::string& current_version,
                                  const std::string& latest_version,
                                  bool is_below_minimum) override;
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override;
    void SetHasRecoverableInput(bool has_recoverable_input) override;
    void ShowListening(const std::optional<std::string>& device_id) override;
    void ShowPartial(const std::string& text, const std::optional<std::string>& device_id) override;
    void AppendPartial(const std::string& text, const std::optional<std::string>& device_id) override;
    void ShowRefining(const std::string& text, const std::optional<std::string>& device_id) override;
    void ShowFinalCountdown(const std::string& text,
                            const std::optional<std::string>& device_id,
                            std::function<void()> on_complete) override;
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) override;
    void ShowError(const std::string& text,
                   const std::optional<std::string>& device_id,
                   std::function<void()> on_complete) override;
    void ShowCloudUpgrade(const std::string& message,
                          const std::string& url,
                          const std::optional<std::string>& device_id) override;
    void HideOverlay(std::function<void()> on_hidden = {}) override;
    void ShowSubtitle(const std::string& text,
                      const std::string& device_id,
                      OverlayThemeColor color) override;
    void HideSubtitles() override;
    void ShowNotification(const std::string& title, const std::string& body) override;
    // 无运行实例时由命令行入口(--ota)注入的待处理 OTA 请求，连上设备后自动触发。
    void SetPendingOtaRequest(std::string file_path,
                              std::optional<std::string> device_id);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    bool CreateWindowInternal();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void RebuildTooltip();
    void UpdateTrayIcon();
    void RequestConnectedBatteryStatus();
    void RegisterTaskbarMessage();
    bool ShowOnboardingIfNeeded();
    bool ShowOnboarding();
    void ShowPairDeviceDialog();
    void ShowSettings();
    void ShowAirMouseTuning();
    void SaveInputOptions();
    void SyncLaunchAtLogin();
    // 根据 config_.selection_hotword_enabled 同步划词监测器启用状态与语言。
    void SyncSelectionHotword();
    void SaveDeviceThemeColor(const std::string& device_id, OverlayThemeColor color);
    void SaveDeviceThemeSize(const std::string& device_id, OverlayThemeSize size);
    void SaveDeviceOverlayPosition(const std::string& device_id, OverlayPosition position);
    void SaveDeviceOutputProfile(const std::string& device_id, OutputProfile profile);
    void ApplyOverlayStyle(const std::optional<std::string>& device_id);
    void StartFirmwareUpdate(const std::string& device_id);
    void StartFirmwareUpdateFromFile(const std::string& device_id);
    // 用给定本地 bin 路径发起 BLE OTA：自动选已连接设备（device_id 为空时取第一个）。
    void StartOtaFromFile(const std::string& file_path,
                          const std::optional<std::string>& device_id);
    void PairDevice(const std::string& device_id, std::uint64_t bluetooth_address,
                    BluetoothAddressKind address_kind, const std::string& name);
    void PairDeviceByManualId(const std::string& device_id);
    void HandlePairingCompleted(const std::string& device_id, std::optional<DeviceInfo> info);
    std::wstring Utf16(const std::string& text) const;
    void DispatchToUi(std::function<void()> action);
    void ShutdownAndQuit();
    // 以管理员身份重启自身：ShellExecuteW runas 触发 UAC，新 High 实例启动后旧实例清理退出。
    void RelaunchElevatedAndQuit();

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    DWORD ui_thread_id_ = 0;
    UINT taskbar_created_message_ = 0;
    AppConfig config_;
    InputInjectorWin input_injector_;
    std::unique_ptr<GlobalHotkeyWin> global_hotkey_;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;
    std::unique_ptr<PairDeviceDialog> pair_device_dialog_;
    std::unique_ptr<SettingsDialog> settings_dialog_;
    std::unique_ptr<AirMouseTuningWindow> air_mouse_tuning_window_;
    std::unique_ptr<FirmwareUpdateDialog> firmware_update_dialog_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<SubtitleWindow> subtitles_;
    std::unique_ptr<SelectionHotwordManager> selection_hotword_manager_;
    class BleCentralWin* ble_central_ = nullptr;
    std::string status_ = "Ready";
    std::vector<ConnectedDevice> connected_devices_;
    std::vector<std::string> paired_device_ids_;
    std::map<std::string, DeviceInfo> device_info_map_;
    std::map<std::string, DeviceBattery> device_battery_map_;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_map_;
    std::optional<PairedDeviceEntry> pending_pairing_entry_;
    std::optional<OtaCliRequest> pending_ota_request_;
    bool has_recoverable_input_ = false;
    bool is_shutting_down_ = false;
    static constexpr UINT_PTR kAirMouseTimerId = 100;
    static constexpr UINT kAirMouseTickIntervalMs = 16;  // ~60Hz
    // 休眠恢复后延迟重启 BLE 的定时器：SetTimer 对同一 id 重复设置会重置计时器，
    // 天然对 PBT_APMRESUMEAUTOMATIC 与紧随其后的 PBT_APMRESUMESUSPEND 去抖。
    static constexpr UINT_PTR kResumeRestartTimerId = 101;
    static constexpr UINT kResumeRestartDelayMs = 1500;
    bool air_mouse_timer_active_ = false;
    std::chrono::steady_clock::time_point last_battery_status_request_{};
};

} // namespace voicestick
