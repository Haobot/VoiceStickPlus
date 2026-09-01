#pragma once

#include "pair_device_helper.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

class PairDeviceDialog {
public:
    PairDeviceDialog(HINSTANCE instance,
                     HWND owner,
                     UiLanguage language,
                     std::vector<std::string> existing_device_ids,
                     std::function<void(std::string, std::uint64_t, BluetoothAddressKind, std::string)> on_pair,
                     std::function<void(std::string, std::optional<DeviceInfo>)> on_pair_completed);
    ~PairDeviceDialog();

    void Show();
    HWND hwnd() const { return hwnd_; }
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices);
    void SetDeviceInfo(const DeviceInfo& info);
    void SetPairingError(const std::string& device_id, const std::string& message);
    void SetManualPairHandler(std::function<void(std::string)> handler) { on_pair_manual_ = std::move(handler); }

    std::function<void(std::string device_id)> on_pair_timeout;

private:
    struct PairingDevice {
        PairingCandidate candidate;
    };

    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildContent();
    void StartScan();
    void StopScan();
    void RestartScanIfNeeded();
    void PairManualDeviceId();
    void HandleAdvertisement(
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher& watcher,
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    void RebuildList();
    void PairSelectedDevice();
    void Close();
    void BeginPairing(const PairingDevice& device);
    // 小米遥控器：先尝试 WinRT 应用内 Bond 配对（ATVV GATT 需要 OS Bond），成功后
    // 经 kXiaomiBondedMessage 回到 UI 线程继续 on_pair_ 流程；失败给出系统蓝牙
    // 设置引导文案。
    winrt::fire_and_forget AttemptXiaomiOsPairing(PairingDevice device);
    void HandlePairingConnected();
    void HandlePairingSucceeded(const DeviceInfo& info);
    void HandlePairingError(const std::string& message);
    void HandlePairingTimeout();
    void HandlePairingFinalize();
    void FinalizePairing(std::optional<DeviceInfo> info);
    bool IsExistingDevice(const std::string& device_id) const;
    std::wstring Utf16(const std::string& text) const;
    int Dp(int px) const;
    void RebuildUi();
    void DestroyControls();
    std::uint64_t NowMs() const;

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND status_label_ = nullptr;
    HWND device_list_ = nullptr;
    HWND pair_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HWND manual_id_label_ = nullptr;
    HWND manual_id_edit_ = nullptr;
    HFONT ui_font_ = nullptr;
    UiLanguage language_ = UiLanguage::kEnglish;
    UINT dpi_ = 96;
    std::vector<BYTE> dialog_template_;
    std::optional<std::string> pairing_device_id_;
    // 正在配对的设备类别：决定状态文案 ID 前缀（VS-/RC-）与配对完成后的收尾路径。
    DeviceClass pairing_device_class_ = DeviceClass::kStickS3;
    // 小米遥控器 OS Bond 进行中的候选设备：kXiaomiBondedMessage 到达后用它继续
    // on_pair_ 连接流程（fire_and_forget 协程不直接回调 UI 线程外成员）。
    std::optional<PairingDevice> pending_pair_device_;
    std::vector<std::string> existing_device_ids_;
    std::function<void(std::string, std::uint64_t, BluetoothAddressKind, std::string)> on_pair_;
    std::function<void(std::string, std::optional<DeviceInfo>)> on_pair_completed_;
    std::function<void(std::string)> on_pair_manual_;
    bool pairing_finalized_ = false;
    std::uint64_t received_advertisement_count_ = 0;
    std::uint64_t candidate_count_ = 0;
    std::uint64_t scan_restart_count_ = 0;
    std::vector<PairingDevice> devices_;
    std::vector<RetainedPairingCandidate> retained_named_candidates_;
    std::mutex mutex_;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher_{nullptr};
    winrt::event_token received_token_{};
};

} // namespace voicestick
