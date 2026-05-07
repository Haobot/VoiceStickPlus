#pragma once

#include "app_config.h"
#include "input_injector_win.h"
#include "overlay_window.h"
#include "pair_device_dialog.h"
#include "settings_dialog.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

class Win32App : public VoiceStickUi {
public:
    explicit Win32App(HINSTANCE instance);
    int Run();

    void SetStatus(const std::string& status) override;
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override;
    void SetDeviceInfo(const DeviceInfo& info) override;
    void SetPairingError(const std::string& device_id, const std::string& message) override;
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override;
    void SetHasRecoverableInput(bool has_recoverable_input) override;
    void ShowListening() override;
    void ShowPartial(const std::string& text) override;
    void ShowFinalCountdown(const std::string& text, std::function<void()> on_complete) override;
    void ShowPausedFinal(const std::string& text) override;
    void ShowError(const std::string& text, std::function<void()> on_complete) override;
    void HideOverlay(std::function<void()> on_hidden = {}) override;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    bool CreateWindowInternal();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void RebuildTooltip();
    void RegisterTaskbarMessage();
    void ShowPairDeviceDialog();
    void ShowSettings();
    void PairDevice(const std::string& device_id, std::uint64_t bluetooth_address,
                    BluetoothAddressKind address_kind, const std::string& name);
    void HandlePairingCompleted(const std::string& device_id, std::optional<DeviceInfo> info);
    void ShowNotification(const std::string& title, const std::string& body);
    void OpenPath(const std::filesystem::path& path);
    std::wstring Utf16(const std::string& text) const;
    void DispatchToUi(std::function<void()> action);

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    DWORD ui_thread_id_ = 0;
    UINT taskbar_created_message_ = 0;
    AppConfig config_;
    InputInjectorWin input_injector_;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;
    std::unique_ptr<PairDeviceDialog> pair_device_dialog_;
    std::unique_ptr<SettingsDialog> settings_dialog_;
    std::unique_ptr<OverlayWindow> overlay_;
    class BleCentralWin* ble_central_ = nullptr;
    std::string status_ = "Ready";
    std::vector<ConnectedDevice> connected_devices_;
    std::vector<std::string> paired_device_ids_;
    std::map<std::string, DeviceInfo> device_info_map_;
    std::optional<PairedDeviceEntry> pending_pairing_entry_;
    bool has_recoverable_input_ = false;
};

} // namespace voicestick
