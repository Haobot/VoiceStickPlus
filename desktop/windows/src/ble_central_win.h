#pragma once

#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

namespace voicestick {

class BleCentralWin : public BleCentral {
public:
    explicit BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd = nullptr);
    ~BleCentralWin() override;

    void Start() override;
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override;
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name) override;
    void SendUiState(const std::string& state,
                       const std::string& text,
                       const std::optional<std::string>& device_id) override;
    void SendInteractionMode(InteractionMode mode,
                             const std::optional<std::string>& device_id) override;
    void SendShowImuDebug(bool enabled,
                          const std::optional<std::string>& device_id) override;
    void SendTapEnabled(bool enabled,
                        const std::optional<std::string>& device_id) override;
    void SendTapSensitivity(int level,
                            const std::optional<std::string>& device_id) override;
    void SendAirMouseEnabled(bool enabled,
                             const std::optional<std::string>& device_id) override;
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override;
    void RequestBatteryStatus(const std::optional<std::string>& device_id) override;
    void SendRemoteButton(RemoteButtonAction action,
                          const std::string& button,
                          const std::optional<std::string>& device_id,
                          std::uint32_t request_id) override;
    void UpdateFirmware(ByteVector image,
                        const std::string& device_id,
                        std::function<void(FirmwareUpdateProgress)> progress,
                        std::function<void(bool, std::string)> completion) override;
    void CancelFirmwareUpdate() override;
    bool IsConnected(const std::string& device_id) const override;
    void CancelPendingConnect(const std::string& device_id) override;
    void Shutdown() override;

    // 系统休眠/恢复或蓝牙无线电状态变化后调用：BluetoothLEAdvertisementWatcher
    // 会静默失效（仍报告 Started 却不再投递广告包），必须彻底重建扫描与所有
    // 会话才能恢复。
    void RestartForResume();

private:
    struct DeviceSession {
        std::uint64_t bluetooth_address = 0;
        ConnectedDevice device;
        winrt::Windows::Devices::Bluetooth::BluetoothLEDevice ble_device{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession gatt_session{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic audio_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic state_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic control_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_rx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_state_characteristic{nullptr};
        winrt::event_token audio_value_changed_token{};
        winrt::event_token state_value_changed_token{};
        winrt::event_token ota_state_value_changed_token{};
        winrt::event_token connection_status_token{};
        winrt::event_token gatt_services_changed_token{};
        winrt::event_token session_status_token{};
        bool audio_subscribed = false;
        bool state_subscribed = false;
        bool ota_state_subscribed = false;
        bool ready = false;
        // 心跳探活：任意入站 GATT 流量（audio/state/ota_state notify）刷新的时间戳
        //（steady_clock epoch 毫秒）。心跳线程据此判定对端静默消失的僵尸会话。
        std::atomic<std::int64_t> last_rx_ms{0};
    };

    struct FirmwareUpdateSession {
        std::string device_id;
        std::uint32_t transfer_id = 0;
        ByteVector image;
        std::function<void(FirmwareUpdateProgress)> progress;
        std::function<void(bool, std::string)> completion;
        std::atomic<std::uint32_t> device_confirmed_written{0};
        bool cancel_requested = false;
    };

    void StartScan();
    void StopScan();
    void HandleAdvertisement(const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher& watcher,
                              const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    winrt::fire_and_forget ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                              BluetoothAddressKind address_kind,
                                              std::string local_name,
                                              std::string device_id);
    winrt::fire_and_forget WriteControlPayloadAsync(std::shared_ptr<DeviceSession> session, ByteVector payload);
    winrt::Windows::Foundation::IAsyncOperation<bool> EnsureOtaCharacteristicsAsync(
        std::shared_ptr<DeviceSession> session,
        std::string device_id);
    winrt::fire_and_forget UpdateFirmwareAsync(std::shared_ptr<DeviceSession> session,
                                               std::shared_ptr<FirmwareUpdateSession> update_session);
    void HandleFirmwareOtaStateEvent(const std::string& device_id, const FirmwareOtaStateEvent& event);
    void FinishFirmwareUpdate(std::shared_ptr<FirmwareUpdateSession> update_session,
                              bool success,
                              const std::string& message);
    void HandleDeviceDisconnected(const std::string& device_id, std::shared_ptr<DeviceSession> session);
    void CloseSession(std::shared_ptr<DeviceSession> session);
    void CloseSessions();
    // 周期心跳：向每个已连接会话写 battery_status_request 强制链路层收发，
    // 并用入站流量时间戳判定僵尸会话（对端静默消失、WinRT 断连事件未投递时
    // 的兜底通道）。
    void StartHeartbeat();
    void StopHeartbeat();
    void HeartbeatLoop();
    void ProbeSessions();
    static ByteVector BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer);
    void PublishConnections();

    void DispatchToUiThread(std::function<void()> callback);

    HWND dispatch_hwnd_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex dispatch_mutex_;
    std::queue<std::function<void()>> dispatch_queue_;
    std::set<std::string> paired_device_ids_;
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions_by_device_id_;
    std::shared_ptr<FirmwareUpdateSession> firmware_update_session_;
    std::set<std::uint64_t> connecting_addresses_;
    std::set<std::string> cancelled_device_ids_;
    // 连接失败后的退避期：key=蓝牙地址，value=可以重新尝试连接的最早时间点。
    // 避免 tight-loop（失败→扫描→立即重试→再失败）。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> connect_cooldown_until_;
    // 僵尸链路安定窗：key=蓝牙地址，value=可以重新尝试连接的最早时间点。
    // 快速重启场景下等 Windows 拆除旧链路（详见 ble_central_win.cc kReconnectSettleDelay）。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> reconnect_settle_until_;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher_{nullptr};
    winrt::event_token received_token_{};
    std::chrono::steady_clock::time_point scan_started_at_{};
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool heartbeat_stop_ = false;

public:
    static constexpr UINT WM_BLE_DISPATCH = WM_APP + 100;
    void ProcessDispatchedCallbacks();
};

} // namespace voicestick
