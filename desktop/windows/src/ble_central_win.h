#pragma once

#include "voice_stick_coordinator.h"
#include "xiaomi_atvv_session.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
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
                             const std::string& name,
                             DeviceClass device_class) override;
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
    void SendEncoderLedColor(const std::string& color,
                             const std::optional<std::string>& device_id) override;
    void SendEncoderRecordingGate(bool enabled,
                                  const std::optional<std::string>& device_id) override;
    void SendAirMouseEnabled(bool enabled,
                             const std::optional<std::string>& device_id) override;
    void SendImuWakeSensitivity(int threshold_lsb,
                                const std::optional<std::string>& device_id) override;
    void RequestBatteryStatus(const std::optional<std::string>& device_id) override;
    void SendPowerLogCommand(const std::string& device_id, ByteVector payload) override;
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

    // ---- 小米遥控器 2 Pro（ATVV）接线 ----
    // MIC_OPEN 时刻写出点（steady_clock epoch ms）：ValueChanged 回调线程直接写、
    // F5 抑制钩子（voice_f5_suppressor）读。须在 Start 前设置，之后不再变更。
    void SetXiaomiMicOpenSink(std::atomic<std::int64_t>* sink) { xiaomi_mic_open_sink_ = sink; }
    // ATVV 会话参数解析器（interaction_mode/gain_db/double_click_ms；按设备）。
    // 在 UI 线程调用（会话创建时），实现侧读配置无竞态。须在 Start 前设置。
    void SetXiaomiSessionOptionsResolver(
        std::function<XiaomiAtvvSession::Options(const std::string& device_id)> resolver) {
        xiaomi_options_resolver_ = std::move(resolver);
    }
    // ATVV 会话 Tick 泵：UI 线程 50ms 定时器驱动（长按阈值/尾包宽限/双击窗/CAPS 超时）。
    void TickXiaomiSessions();

private:
    struct DeviceSession {
        std::uint64_t bluetooth_address = 0;
        ConnectedDevice device;
        DeviceClass device_class = DeviceClass::kStickS3;
        winrt::Windows::Devices::Bluetooth::BluetoothLEDevice ble_device{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession gatt_session{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic audio_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic state_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic control_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_rx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_state_characteristic{nullptr};
        // 小米 ATVV 侧成员（device_class==kXiaomiRemote2Pro 时有效）：TX 主机写命令，
        // Audio/Control 为遥控器 notify；battery 为标准 Battery Service 0x2A19（可选，
        // 失败不阻断连接）；probe 为心跳保活读特征（battery 优先，否则 GAP 0x2A00）。
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic xiaomi_tx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic xiaomi_audio_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic xiaomi_control_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService xiaomi_battery_service{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic battery_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic probe_characteristic{nullptr};
        // ATVV 会话状态机（纯逻辑）。公开入口线程契约为单线程串行；集成层除 UI 线程
        // 分发外还有断连线程的 Stop，统一用 xiaomi_mutex 串行化（访问前判空，
        // CloseSession 置空后晚到的分发自然跳过）。
        std::mutex xiaomi_mutex;
        std::unique_ptr<XiaomiAtvvSession> xiaomi_atvv_session;
        winrt::event_token audio_value_changed_token{};
        winrt::event_token state_value_changed_token{};
        winrt::event_token ota_state_value_changed_token{};
        winrt::event_token xiaomi_audio_value_changed_token{};
        winrt::event_token xiaomi_control_value_changed_token{};
        winrt::event_token battery_value_changed_token{};
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
    // 退避延迟后重启扫描（独立线程睡眠，epoch 校验防止 Shutdown 后踩空）。
    void ScheduleDelayedScanRestart(int delay_ms);
    void StopScan();
    // 订阅蓝牙无线电 StateChanged：系统蓝牙开关切换（设置/操作中心）会杀死
    // 广告 watcher 与全部链路，无线电恢复（On）时立即 RestartForResume()
    // 重建扫描与会话，不等扫描静默看门狗的秒级~分钟级超时。
    winrt::fire_and_forget InitRadioWatcherAsync();
    void HandleAdvertisement(const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher& watcher,
                              const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    winrt::fire_and_forget ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                              BluetoothAddressKind address_kind,
                                              std::string local_name,
                                              std::string device_id,
                                              DeviceClass device_class);
    winrt::fire_and_forget WriteControlPayloadAsync(std::shared_ptr<DeviceSession> session, ByteVector payload);
    // ---- 小米 ATVV 会话辅助 ----
    // 串行驱动 ATVV 会话并分发产出动作：entry 在 xiaomi_mutex 下执行（会话为空跳过），
    // 动作分发在锁外进行（写 TX 异步发；StateEvent/AudioFrame 走既有回调）。
    // 时钟语义：now_ms 取 UI dispatch 执行时刻（NowSteadyMs() 现取）而非 GATT notify
    // 到达时刻；notify→dispatch 的队列延迟使会话内时间轴整体同向平移，长按阈值/
    // 双击窗/尾包宽限等相对时长判定近似守恒，不为此付出锁内取时钟的额外同步。
    void DriveXiaomiSession(const std::shared_ptr<DeviceSession>& session,
                            const std::function<std::vector<XiaomiAtvvAction>(XiaomiAtvvSession&)>& entry);
    void DispatchXiaomiActions(const std::shared_ptr<DeviceSession>& session,
                               std::vector<XiaomiAtvvAction> actions);
    winrt::fire_and_forget WriteXiaomiTxAsync(std::shared_ptr<DeviceSession> session, ByteVector payload);
    // 可选 Battery Service（0x180F/0x2A19，读+notify）：失败只记日志不阻断连接。
    // 无电量特征时用 GAP 0x2A00 设备名读作心跳保活特征。
    winrt::Windows::Foundation::IAsyncAction SetupXiaomiBatteryAsync(std::shared_ptr<DeviceSession> session,
                                                                     std::string device_id);
    // 心跳保活：读 probe_characteristic 强制链路层收发；成功刷新 last_rx_ms，
    // Unreachable/异常按链路已死拆除（与 StickS3 心跳写同一语义）。
    winrt::fire_and_forget ProbeXiaomiSessionAsync(std::shared_ptr<DeviceSession> session);
    // 断连/关闭前尽力发 MIC_CLOSE（session Stop 产出的写 TX），随后复位状态机。
    void StopXiaomiSessionBestEffort(const std::shared_ptr<DeviceSession>& session);
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
    // 扫描健康看门狗（心跳线程周期调用）：清理滞留超时的在途连接 claim，
    // 并检测广告 watcher 静默失效（有配对设备待发现却长时间零广告）后重建扫描。
    void CheckScanHealth();
    static ByteVector BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer);
    void PublishConnections();
    // 多设备连接快照诊断：打印每个已配对设备的会话状态（ready/session但未就绪/无会话），
    // 辅助定位多设备场景下偶发 Pairing 的根因（哪台断了、哪台还连着、watcher 重建时机）。
    void LogConnectionSnapshot(std::string_view reason);

    void DispatchToUiThread(std::function<void()> callback);

    HWND dispatch_hwnd_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex dispatch_mutex_;
    std::queue<std::function<void()>> dispatch_queue_;
    std::set<std::string> paired_device_ids_;
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions_by_device_id_;
    std::shared_ptr<FirmwareUpdateSession> firmware_update_session_;
    // 在途连接占用：key=蓝牙地址，value=claim 时间戳。ConnectDeviceAsync 若在
    // 无超时的 WinRT co_await 上永久挂起，claim 永不释放、后续广播全被否决；
    // CheckScanHealth() 据此时间戳强制释放滞留超时的 claim。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> connecting_addresses_;
    std::set<std::string> cancelled_device_ids_;
    // 连接失败后的退避期：key=蓝牙地址，value=可以重新尝试连接的最早时间点。
    // 避免 tight-loop（失败→扫描→立即重试→再失败）。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> connect_cooldown_until_;
    // 僵尸链路安定窗：key=蓝牙地址，value=可以重新尝试连接的最早时间点。
    // 快速重启场景下等 Windows 拆除旧链路（详见 ble_central_win.cc kReconnectSettleDelay）。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> reconnect_settle_until_;
    // 经安定窗路径放行的地址 → {打标时间, 已用免退避重试次数}。
    // 其连接失败多为僵尸链路尚未拆完，fail 时在窗口期内免 5s 退避，
    // 让下一条广播立即触发重试（覆盖连按重启产生的多重僵尸）。
    std::map<std::uint64_t, std::pair<std::chrono::steady_clock::time_point, int>>
        zombie_suspect_marks_;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher_{nullptr};
    winrt::event_token received_token_{};
    winrt::event_token stopped_token_{};
    // 蓝牙无线电状态监视（见 InitRadioWatcherAsync）。
    winrt::Windows::Devices::Radios::Radio bluetooth_radio_{nullptr};
    winrt::event_token radio_state_token_{};
    // 应用自己执行 radio reset（陈旧 bond 恢复）期间置位：StateChanged 处理器
    // 据此跳过自建重置引发的重建（该路径已显式 StartScan，避免与在途连接争抢）。
    std::atomic<bool> self_radio_reset_{false};
    std::chrono::steady_clock::time_point scan_started_at_{};
    // watcher 存活证明：HandleAdvertisement 收到任意广告包即刷新（steady_clock
    // epoch 毫秒）。CheckScanHealth() 用它检测 watcher 静默失效；StartScan()
    // 成功时写入当前时间作为基线。
    std::atomic<std::int64_t> last_adv_received_ms_{0};
    // 看门狗触发扫描重建的节流：上次由 CheckScanHealth() 重建的时间点。
    std::chrono::steady_clock::time_point last_scan_watchdog_restart_at_{};
    // watcher 异常停止后的退避重启状态：radio 坏状态下无退避会形成每秒数百次
    // 的扫描热循环，挤掉活跃 BLE 连接（2026-08-22 真机事故，见 StartScan 注释）。
    std::atomic<int> scan_restart_streak_{0};
    std::atomic<std::int64_t> last_scan_stop_steady_ms_{0};
    // 扫描代数：每次 StartScan/Shutdown 递增，使在途的延迟重启线程失效。
    std::atomic<std::uint64_t> scan_epoch_{0};
    // claim 被拒日志的限流（key=蓝牙地址，value=上次记录的 steady_clock epoch
    // 毫秒）：正常重连中广告风暴期每秒数十次拒绝，逐条记录会刷屏。
    std::map<std::uint64_t, std::int64_t> claim_denied_log_ms_;
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool heartbeat_stop_ = false;
    // 小米 MIC_OPEN 时刻写出点与 ATVV 会话参数解析器（Start 前由 App 层注入）。
    std::atomic<std::int64_t>* xiaomi_mic_open_sink_ = nullptr;
    std::function<XiaomiAtvvSession::Options(const std::string& device_id)> xiaomi_options_resolver_;

public:
    static constexpr UINT WM_BLE_DISPATCH = WM_APP + 100;
    void ProcessDispatchedCallbacks();
};

} // namespace voicestick
