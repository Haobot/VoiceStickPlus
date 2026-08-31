#include "ble_central_win.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace voicestick {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothError;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcherStoppedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattClientCharacteristicConfigurationDescriptorValue;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSessionStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSessionStatusChangedEventArgs;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteOption;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceUnpairingResultStatus;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::DataWriter;

constexpr int kServiceDiscoveryAttempts = 6;
constexpr std::chrono::milliseconds kServiceDiscoveryRetryDelay{1000};
constexpr std::chrono::milliseconds kServiceDiscoveryTimeout{8000};
constexpr std::chrono::milliseconds kCharacteristicDiscoveryTimeout{5000};
constexpr std::chrono::milliseconds kDeviceReopenDelay{800};
constexpr std::chrono::milliseconds kConnectionSettleDelay{100};
constexpr std::chrono::milliseconds kValueChangedHandlerSettleDelay{100};
constexpr std::chrono::milliseconds kDeviceInfoSettleDelay{100};

// 心跳探活：周期性向每个已连接会话写 battery_status_request（固件收到必回
// battery_status，见 firmware/main/main.c 的 APP_EVENT_BATTERY_STATUS_REQUEST），
// 并跟踪入站流量时间戳；超过 kHeartbeatTimeout 无任何入站即判定僵尸会话并拆除
// 重连。这是对端静默消失、WinRT 断连事件未投递时的兜底通道；该写入不会重启固件
// 的 5 分钟待机断电计时器，不会改变设备省电行为。
constexpr std::chrono::seconds kHeartbeatInterval{30};
constexpr std::chrono::milliseconds kHeartbeatTimeout{90000};

// 僵尸链路安定窗：设备快速重启（手动复位/OTA/崩溃）后，Windows 仍持有旧链路
// （对端静默消失时断连事件不投递）。此时立即重连会挂在僵尸链路上——OS 认为
// "已连接"（link-layer connected after 0ms polls=0），首个空口 ATT 操作挂起，
// 直到旧链路监督超时（实测重启后 ~3.5-4.0s）OS 宣告断连，订阅被取消，再叠加
// 5s 失败退避，全程 ~11s（两次真机复现，见 Doc/Expe/ble-zombie-link-reboot-reconnect.md）。
// 拆旧会话时若设备"刚刚还活跃"（last_rx 在 kZombieFreshThreshold 内），说明这是
// 快速重启场景，延迟 kReconnectSettleDelay 等 OS 埋掉僵尸链路再连。
// 判出僵尸时已主动 Close gatt_session/ble_device（栈立即发 LL_TERMINATE），
// 不需要等被动监督超时的 3.5-4s；撞未死僵尸由 kSubscribeTimeout 兜底（见订阅处）。
// 深睡唤醒的会话安静了数分钟（last_rx 远超阈值），僵尸链路早已死亡，不进窗口，
// 保持快速回连路径。
constexpr std::chrono::milliseconds kReconnectSettleDelay{1500};
constexpr std::int64_t kZombieFreshThresholdMs{45000};

// 链上首个 ATT 操作（state 订阅）的应用层超时。正常几十 ms 完成；撞上未死
// 僵尸链路时 OS 要 ~3.5-4s 才宣告断连，这里 2.5s 提前取消并走失败路径，
// 配合 zombie_suspect 免退避把最坏回连压在 ~5.5s 而不是 ~10s。
constexpr std::chrono::milliseconds kSubscribeTimeout{2500};

// zombie_suspect 免退避重试的窗口与上限：连按重启会产生多重僵尸，
// 单次免退避不够；但无限免退避会让持续失败的设备 tight-loop，
// 故限 15s 窗内最多 3 次，超出回落正常 5s 退避。
constexpr std::chrono::milliseconds kZombieSuspectWindow{15000};
constexpr int kZombieSuspectMaxFreeRetries = 3;

// watcher 异常停止的退避重启：3s 内再次异常停止视为连续失败，指数退避
// 1s→2s→4s→…→30s 封顶。radio 坏状态下无退避会形成每秒数百次扫描热循环。
constexpr std::int64_t kScanRestartStreakWindowMs{3000};
constexpr int kScanRestartMaxBackoffMs{30000};

// 扫描健康看门狗：BluetoothLEAdvertisementWatcher 会在蓝牙无线电状态变化
// （包括应用自己触发的 radio reset）或驱动异常后静默失效——仍报告 Started
// 却不再投递任何广告包，设备持续广播而主机永远收不到，只能重启进程恢复
// （见 Doc/Expe/ble-watcher-silent-death-pairing-stuck.md）。无线电开关切换
// 由 StateChanged 事件秒级覆盖（见 InitRadioWatcherAsync），本看门狗是其余
// 失效场景的兜底：心跳线程按 kScanSilenceTimeout 检测并重建 watcher；
// kScanWatchdogMinRestartInterval 节流，避免 RF 静默环境（设备深睡且周围
// 无其他 BLE 设备）下空转刷日志。
constexpr std::chrono::seconds kScanSilenceTimeout{60};
constexpr std::chrono::seconds kScanWatchdogMinRestartInterval{120};

// 在途连接 claim 的滞留上限：ConnectDeviceAsync 若在任一无超时的 WinRT
// co_await 上永久挂起（既不 fail 也不 ready），claim 永不释放，该地址的
// 后续广播全被 try_claim_connect 否决，形成重连自我封锁。最坏正常连接
// 耗时实测 ~65s（6 次服务发现重试 + radio reset），120s 留出充足余量。
constexpr std::chrono::seconds kConnectClaimTimeout{120};

// HRESULT_FROM_WIN32(ERROR_BAD_COMMAND): Windows surfaces this for our
// scenario when the OS thinks the device is already paired/bonded but the
// remote refuses or rolled its keys. We special-case it to suggest unpairing.
constexpr std::int32_t kErrorBadCommand = static_cast<std::int32_t>(0x80070016);
constexpr std::int32_t kErrorTimeout = static_cast<std::int32_t>(0x800705B4);

long long ElapsedMs(std::chrono::steady_clock::time_point start) {
    if (start == std::chrono::steady_clock::time_point{}) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

std::int64_t NowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

BluetoothAddressType ToBluetoothAddressType(BluetoothAddressKind kind) {
    switch (kind) {
    case BluetoothAddressKind::kPublic:
        return BluetoothAddressType::Public;
    case BluetoothAddressKind::kRandom:
        return BluetoothAddressType::Random;
    case BluetoothAddressKind::kUnspecified:
    default:
        return BluetoothAddressType::Unspecified;
    }
}

const char* AddressKindName(BluetoothAddressKind kind) {
    switch (kind) {
    case BluetoothAddressKind::kPublic:
        return "public";
    case BluetoothAddressKind::kRandom:
        return "random";
    case BluetoothAddressKind::kUnspecified:
    default:
        return "unspecified";
    }
}

std::string Utf8FromHstring(const winrt::hstring& value) {
    return winrt::to_string(value);
}

ByteVector BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    DataReader reader = DataReader::FromBuffer(buffer);
    ByteVector bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) reader.ReadBytes(bytes);
    return bytes;
}

struct AdvertisementIdentity {
    std::string local_name;
    bool has_voice_stick_service = false;
};

AdvertisementIdentity AdvertisementIdentityFrom(
    const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisement& advertisement) {
    AdvertisementIdentity identity;
    identity.local_name = Utf8FromHstring(advertisement.LocalName());

    ByteVector ad_data;
    for (const auto& section : advertisement.DataSections()) {
        const auto data = BytesFromBuffer(section.Data());
        if (data.size() > 0xff - 1) continue;
        ad_data.push_back(static_cast<std::uint8_t>(data.size() + 1));
        ad_data.push_back(section.DataType());
        ad_data.insert(ad_data.end(), data.begin(), data.end());
    }
    if (identity.local_name.empty()) {
        identity.local_name = BleProtocol::LocalNameFromAdvertisementData(ad_data).value_or(std::string());
    }
    identity.has_voice_stick_service = BleProtocol::HasVoiceStickServiceUuid(ad_data);
    return identity;
}

bool HasNotify(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::Notify) ==
           GattCharacteristicProperties::Notify;
}

bool HasWriteWithoutResponse(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::WriteWithoutResponse) ==
           GattCharacteristicProperties::WriteWithoutResponse;
}

bool HasWrite(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::Write) ==
           GattCharacteristicProperties::Write;
}

winrt::Windows::Storage::Streams::IBuffer BufferFromBytes(std::span<const std::uint8_t> payload) {
    DataWriter writer;
    ByteVector bytes(payload.begin(), payload.end());
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}

std::uint32_t RandomTransferId() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
    return dist(rng);
}

std::string FormatBluetoothAddress(std::uint64_t address) {
    char buffer[18]{};
    snprintf(buffer, sizeof(buffer), "%02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
             (address >> 40) & 0xff,
             (address >> 32) & 0xff,
             (address >> 24) & 0xff,
             (address >> 16) & 0xff,
             (address >> 8) & 0xff,
             address & 0xff);
    return buffer;
}

std::string FormatHresult(std::int32_t code) {
    char buffer[16]{};
    snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(code));
    return buffer;
}

bool CanReadAdvertisementAddressType() {
    static const bool available =
        winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs",
            L"BluetoothAddressType");
    return available;
}

std::string ScanStartFailureMessage(const winrt::hresult_error& error) {
    std::string message = "Bluetooth LE scan failed (HRESULT=" + FormatHresult(error.code()) + ")";
    const auto detail = winrt::to_string(error.message());
    if (!detail.empty()) message += ": " + detail;
    message += ". Turn on Bluetooth in Windows Settings, then restart VoiceStick or update paired devices.";
    return message;
}

std::string GattStatusName(GattCommunicationStatus status) {
    switch (status) {
    case GattCommunicationStatus::Success:
        return "Success";
    case GattCommunicationStatus::Unreachable:
        return "Unreachable";
    case GattCommunicationStatus::ProtocolError:
        return "ProtocolError";
    case GattCommunicationStatus::AccessDenied:
        return "AccessDenied";
    default:
        return "Unknown";
    }
}

void LogBleLine(const std::string& message) {
    LogBle(message);
}

std::string PreviewBytes(std::span<const std::uint8_t> bytes, std::size_t limit = 96) {
    std::string out;
    if (bytes.empty()) return out;
    const std::size_t take = std::min(bytes.size(), limit);
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        const auto byte = bytes[i];
        // Show printable ASCII (the device_info is JSON) and dot-substitute
        // anything else so the log stays readable.
        out.push_back((byte >= 0x20 && byte < 0x7f) ? static_cast<char>(byte) : '.');
    }
    if (bytes.size() > take) out += "...";
    return out;
}

std::string HexDump(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 3);
    static const char* hex = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) out.push_back(' ');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0f]);
    }
    return out;
}

} // namespace

BleCentralWin::BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd)
    : dispatch_hwnd_(dispatch_hwnd),
      paired_device_ids_(paired_device_ids.begin(), paired_device_ids.end()) {}

void BleCentralWin::DispatchToUiThread(std::function<void()> callback) {
    if (!dispatch_hwnd_) {
        callback();
        return;
    }
    {
        std::lock_guard lock(dispatch_mutex_);
        dispatch_queue_.push(std::move(callback));
    }
    PostMessage(dispatch_hwnd_, WM_BLE_DISPATCH, 0, 0);
}

void BleCentralWin::ProcessDispatchedCallbacks() {
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(dispatch_mutex_);
        pending.swap(dispatch_queue_);
    }
    while (!pending.empty()) {
        pending.front()();
        pending.pop();
    }
}

BleCentralWin::~BleCentralWin() {
    Shutdown();
}

void BleCentralWin::Start() {
    StartHeartbeat();
    InitRadioWatcherAsync();
    StartScan();
}

void BleCentralWin::Shutdown() {
    // 使在途的延迟扫描重启线程失效（防止 Shutdown 后 StartScan 踩空）。
    scan_epoch_.fetch_add(1, std::memory_order_release);
    StopHeartbeat();
    StopScan();
    if (bluetooth_radio_) {
        try {
            bluetooth_radio_.StateChanged(radio_state_token_);
        } catch (...) {
        }
        radio_state_token_ = {};
        bluetooth_radio_ = nullptr;
    }

    std::vector<std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session && session->ready) sessions.push_back(session);
        }
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
    }

    CloseSessions();
    PublishConnections();
}

void BleCentralWin::RestartForResume() {
    // 系统休眠/恢复或蓝牙无线电状态变化后，BluetoothLEAdvertisementWatcher
    // 会静默失效：仍报告 Started 却不再投递任何广告包。休眠期间链路也已断开，
    // 残留的 DeviceSession 实为假连接。这里彻底停掉扫描、清理所有连接态与
    // 退避/取消标记、关闭残留会话，再重新 StartScan，让 watcher 与链路都从
    // 干净状态重建——否则设备持续广播而主机永远收不到，表现为「正在扫描」却
    // 连不上、设备端卡在 Pairing。
    LogBleLine("restart-for-resume: power state changed; tearing down scan and sessions");
    StopScan();
    {
        std::lock_guard lock(mutex_);
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
        connect_cooldown_until_.clear();
        reconnect_settle_until_.clear();
        zombie_suspect_marks_.clear();
    }
    CloseSessions();
    PublishConnections();
    StartScan();
}

winrt::fire_and_forget BleCentralWin::InitRadioWatcherAsync() {
    using winrt::Windows::Devices::Radios::Radio;
    using winrt::Windows::Devices::Radios::RadioKind;
    using winrt::Windows::Devices::Radios::RadioState;
    try {
        auto radios = co_await Radio::GetRadiosAsync();
        for (const auto& radio : radios) {
            if (radio.Kind() != RadioKind::Bluetooth) continue;
            bluetooth_radio_ = radio;
            radio_state_token_ = bluetooth_radio_.StateChanged(
                [this](const Radio& sender, const winrt::Windows::Foundation::IInspectable&) {
                    RadioState state = RadioState::Unknown;
                    try {
                        state = sender.State();
                    } catch (...) {
                    }
                    LogBleLine("bluetooth radio state = " +
                               std::to_string(static_cast<int>(state)));
                    if (state != RadioState::On) return;
                    if (self_radio_reset_.load(std::memory_order_relaxed)) {
                        // 应用自己的 radio reset（陈旧 bond 恢复）已在其路径上
                        // 显式 StartScan，这里跳过，避免拆掉在途连接的 claim 与会话。
                        return;
                    }
                    // 系统蓝牙开关切换会杀死 watcher 与全部链路：无线电恢复时
                    // 立即整体重建，秒级回连，不等扫描静默看门狗超时。
                    LogBleLine("bluetooth radio back on; rebuilding scan and sessions");
                    DispatchToUiThread([this] { RestartForResume(); });
                });
            LogBleLine("bluetooth radio watcher subscribed");
            co_return;
        }
        LogBleLine("bluetooth radio watcher: no Bluetooth radio found");
    } catch (const winrt::hresult_error& error) {
        LogBleLine("bluetooth radio watcher subscribe failed: hr=" + FormatHresult(error.code()));
    } catch (...) {
        LogBleLine("bluetooth radio watcher subscribe failed: unknown error");
    }
}

void BleCentralWin::UpdatePairedDeviceIds(const std::vector<std::string>& ids) {
    {
        std::lock_guard lock(mutex_);
        paired_device_ids_ = std::set<std::string>(ids.begin(), ids.end());
        connecting_addresses_.clear();
    }
    CloseSessions();
    PublishConnections();
    LogBleLine("paired device list updated; restarting scan");
    StartScan();
}

void BleCentralWin::ConnectPairedDevice(const std::string& device_id,
                                        std::uint64_t bluetooth_address,
                                        BluetoothAddressKind address_kind,
                                        const std::string& name) {
    LogBleLine("direct connect enter VS-" + device_id + " (pre-lock)");
    {
        std::lock_guard lock(mutex_);
        paired_device_ids_.insert(device_id);
        if (sessions_by_device_id_.contains(device_id)) {
            LogBleLine("direct connect skipped: VS-" + device_id + " is already connected");
            return;
        }
        if (connecting_addresses_.contains(bluetooth_address)) {
            LogBleLine("direct connect skipped: " + FormatBluetoothAddress(bluetooth_address) +
                       " is already connecting");
            return;
        }
        connecting_addresses_.emplace(bluetooth_address, std::chrono::steady_clock::now());
    }
    LogBleLine("direct connect requested VS-" + device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind,
                       name.empty() ? "VS-" + device_id : name, device_id);
}

void BleCentralWin::SendUiState(const std::string& state,
                                const std::string& text,
                                const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::UiStatePayload(state, text);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            } else {
                LogBleLine("send ui_state skipped state=" + state +
                           " dev=VS-" + *device_id +
                           " text_len=" + std::to_string(text.size()));
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
            LogBleLine("send ui_state broadcast state=" + state +
                       " targets=" + std::to_string(targets.size()) +
                       " text_len=" + std::to_string(text.size()));
        }
    }

    for (auto& session : targets) {
        LogBleLine("send ui_state state=" + state +
                   " dev=VS-" + session->device.id +
                   " text_len=" + std::to_string(text.size()));
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendInteractionMode(InteractionMode mode,
                                        const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::InteractionModePayload(InteractionModeName(mode));
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendShowImuDebug(bool enabled,
                                     const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::ShowImuDebugPayload(enabled);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendTapEnabled(bool enabled,
                                   const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::TapEnabledPayload(enabled);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendEncoderLedColor(const std::string& color,
                                        const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::EncoderLedColorPayload(color);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendEncoderRecordingGate(bool enabled,
                                             const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::EncoderRecordingGatePayload(enabled);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendAirMouseEnabled(bool enabled,
                                        const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::AirMouseEnabledPayload(enabled);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendImuWakeSensitivity(int threshold_lsb,
                                           const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::ImuWakeSensitivityPayload(threshold_lsb);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendTapSensitivity(int level,
                                       const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::TapSensitivityPayload(level);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::RequestBatteryStatus(const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::BatteryStatusRequestPayload();
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        LogBleLine("request battery_status dev=VS-" + session->device.id);
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendPowerLogCommand(const std::string& device_id, ByteVector payload) {
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it != sessions_by_device_id_.end() && it->second->ready) {
            targets.push_back(it->second);
        }
    }
    if (targets.empty()) {
        LogBleLine("power_log command skipped (not connected) dev=VS-" + device_id);
        return;
    }
    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), std::move(payload));
    }
}

void BleCentralWin::SendRemoteButton(RemoteButtonAction action,
                                     const std::string& button,
                                     const std::optional<std::string>& device_id,
                                     std::uint32_t request_id) {
    std::string_view action_name = (action == RemoteButtonAction::kDown) ? "down" : "up";
    auto payload = BleProtocol::RemoteButtonPayload(action_name, button, "global_hotkey", request_id);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            } else {
                LogBleLine("send remote_button_" + std::string(action_name) +
                           " skipped dev=VS-" + *device_id);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
            LogBleLine("send remote_button_" + std::string(action_name) +
                       " broadcast target_count=" + std::to_string(targets.size()));
        }
    }

    for (auto& session : targets) {
        LogBleLine("send remote_button_" + std::string(action_name) +
                   " dev=VS-" + session->device.id +
                   " button=" + button +
                   " request_id=" + std::to_string(request_id) +
                   " payload_len=" + std::to_string(payload.size()));
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

bool BleCentralWin::IsConnected(const std::string& device_id) const {
    std::lock_guard lock(mutex_);
    auto it = sessions_by_device_id_.find(device_id);
    return it != sessions_by_device_id_.end() && it->second->ready;
}

void BleCentralWin::UpdateFirmware(ByteVector image,
                                   const std::string& device_id,
                                   std::function<void(FirmwareUpdateProgress)> progress,
                                   std::function<void(bool, std::string)> completion) {
    std::shared_ptr<DeviceSession> session;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_) {
            completion(false, "A firmware update is already running.");
            return;
        }
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready) {
            completion(false, "No VoiceStick is connected.");
            return;
        }
        session = it->second;
        if (image.size() > 3 * 1024 * 1024) {
            completion(false, "Firmware image is larger than the OTA partition.");
            return;
        }
        firmware_update_session_ = std::make_shared<FirmwareUpdateSession>();
        firmware_update_session_->device_id = device_id;
        firmware_update_session_->transfer_id = RandomTransferId();
        firmware_update_session_->image = std::move(image);
        firmware_update_session_->progress = std::move(progress);
        firmware_update_session_->completion = std::move(completion);
    }
    if (firmware_update_session_->progress) {
        firmware_update_session_->progress(FirmwareUpdateProgress{
            0, static_cast<int>(firmware_update_session_->image.size()), true});
    }
    UpdateFirmwareAsync(std::move(session), firmware_update_session_);
}

void BleCentralWin::CancelFirmwareUpdate() {
    std::shared_ptr<FirmwareUpdateSession> update_session;
    std::shared_ptr<DeviceSession> device_session;
    {
        std::lock_guard lock(mutex_);
        update_session = firmware_update_session_;
        if (!update_session) return;
        update_session->cancel_requested = true;
        auto it = sessions_by_device_id_.find(update_session->device_id);
        if (it != sessions_by_device_id_.end()) device_session = it->second;
    }
    if (device_session && device_session->ota_rx_characteristic) {
        auto payload = BleProtocol::OtaAbortPayload(update_session->transfer_id);
        try {
            device_session->ota_rx_characteristic.WriteValueAsync(
                BufferFromBytes(payload), GattWriteOption::WriteWithoutResponse);
        } catch (...) {}
    }
    FinishFirmwareUpdate(update_session, false, "Firmware update cancelled.");
}

void BleCentralWin::CancelPendingConnect(const std::string& device_id) {
    std::lock_guard lock(mutex_);
    cancelled_device_ids_.insert(device_id);
    LogBleLine("cancel requested for VS-" + device_id);
}

void BleCentralWin::StartScan() {
    // 任何新的扫描启动都使在途的延迟重启失效，防止双重扫描。
    scan_epoch_.fetch_add(1, std::memory_order_release);
    StopScan();
    bool has_paired_devices = false;
    {
        std::lock_guard lock(mutex_);
        has_paired_devices = !paired_device_ids_.empty();
    }
    if (!has_paired_devices) {
        LogBleLine("scan skipped: no paired devices");
        PublishConnections();
        return;
    }
    watcher_ = BluetoothLEAdvertisementWatcher();
    watcher_.ScanningMode(BluetoothLEScanningMode::Active);
    // No AdvertisementFilter here: the firmware puts its 128-bit service UUID
    // in the ADV PDU but the LocalName "VS-XXXX" only in the scan response,
    // and WinRT's per-PDU filter would drop the scan response so we'd never
    // see the device id. Filter on device_id in HandleAdvertisement instead.
    received_token_ = watcher_.Received({this, &BleCentralWin::HandleAdvertisement});
    // watcher 被系统停止（无线电关开、驱动异常等）时会收到 Stopped；非正常
    // 停止直接重建扫描，否则设备持续广播而无人接收，永远卡在 Pairing 屏
    // （见 Doc/Expe/ble-watcher-silent-death-pairing-stuck.md）。
    stopped_token_ = watcher_.Stopped(
        [this](const BluetoothLEAdvertisementWatcher&,
               const BluetoothLEAdvertisementWatcherStoppedEventArgs& args) {
            const auto error = args.Error();
            LogBleLine("watcher stopped error=" + std::to_string(static_cast<int>(error)));
            if (error == BluetoothError::Success) return;
            DispatchToUiThread([this] {
                {
                    std::lock_guard lock(mutex_);
                    if (paired_device_ids_.empty()) return;
                }
                // 指数退避重启：radio 坏状态下（error=9）无条件立即重启会形成
                // 每秒数百次的扫描热循环，持续轰炸 radio 并挤掉活跃 BLE 连接
                //（2026-08-22 真机事故：单日 438 万条 error=9 日志，连接每
                // ~60s 被远端终止，reason=0x13）。首退避 1s，封顶 30s。
                const std::int64_t now = NowSteadyMs();
                const int streak =
                    (now - last_scan_stop_steady_ms_.load(std::memory_order_relaxed) <
                     kScanRestartStreakWindowMs)
                        ? scan_restart_streak_.load(std::memory_order_relaxed) + 1
                        : 1;
                scan_restart_streak_.store(streak, std::memory_order_relaxed);
                last_scan_stop_steady_ms_.store(now, std::memory_order_relaxed);
                const int delay_ms = std::min(
                    kScanRestartMaxBackoffMs, 1000 << std::min(streak - 1, 5));
                LogBleLine("watcher stopped unexpectedly; restart in " +
                           std::to_string(delay_ms) + "ms (streak=" +
                           std::to_string(streak) + ")");
                ScheduleDelayedScanRestart(delay_ms);
            });
        });
    try {
        watcher_.Start();
        scan_started_at_ = std::chrono::steady_clock::now();
        last_adv_received_ms_.store(NowSteadyMs(), std::memory_order_relaxed);
        LogBleLine("scan started");
    } catch (const winrt::hresult_error& error) {
        const auto message = ScanStartFailureMessage(error);
        LogBleLine("scan start failed: " + message);
        try {
            watcher_.Received(received_token_);
            watcher_.Stopped(stopped_token_);
        } catch (...) {
        }
        stopped_token_ = {};
        watcher_ = nullptr;
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    } catch (...) {
        const std::string message = "Bluetooth LE scan failed with an unknown error.";
        LogBleLine("scan start failed: unknown error");
        try {
            watcher_.Received(received_token_);
            watcher_.Stopped(stopped_token_);
        } catch (...) {
        }
        stopped_token_ = {};
        watcher_ = nullptr;
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    }
}

void BleCentralWin::ScheduleDelayedScanRestart(int delay_ms) {
    const auto epoch = scan_epoch_.load(std::memory_order_relaxed);
    std::thread([this, epoch, delay_ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        // Shutdown 或期间任何 StartScan 都会推进代数，使本线程失效。
        if (scan_epoch_.load(std::memory_order_acquire) != epoch) return;
        DispatchToUiThread([this, epoch] {
            if (scan_epoch_.load(std::memory_order_acquire) != epoch) return;
            LogBleLine("scan restart after backoff");
            StartScan();
        });
    }).detach();
}

void BleCentralWin::StopScan() {
    if (watcher_) {
        try {
            watcher_.Received(received_token_);
            watcher_.Stopped(stopped_token_);
            watcher_.Stop();
        } catch (const winrt::hresult_error& error) {
            LogBleLine("scan stop failed: hr=" + FormatHresult(error.code()));
        } catch (...) {
            LogBleLine("scan stop failed: unknown error");
        }
        stopped_token_ = {};
        watcher_ = nullptr;
        scan_started_at_ = {};
        LogBleLine("scan stopped");
    }
}

void BleCentralWin::HandleAdvertisement(const BluetoothLEAdvertisementWatcher&,
                                        const BluetoothLEAdvertisementReceivedEventArgs& args) {
    // 任意广告包（不限配对设备）都是 watcher 存活证明，供 CheckScanHealth()
    // 检测 watcher 静默失效。
    last_adv_received_ms_.store(NowSteadyMs(), std::memory_order_relaxed);
    const auto identity = AdvertisementIdentityFrom(args.Advertisement());
    const auto bluetooth_address = args.BluetoothAddress();
    auto device_id = BleProtocol::DeviceIdFromName(identity.local_name);
    if (!device_id.has_value() && identity.has_voice_stick_service) {
        device_id = BleProtocol::DeviceIdFromBluetoothAddress(bluetooth_address);
    }
    if (!device_id.has_value()) return;
    BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
    if (CanReadAdvertisementAddressType()) {
        try {
            switch (args.BluetoothAddressType()) {
            case BluetoothAddressType::Public:
                address_kind = BluetoothAddressKind::kPublic;
                break;
            case BluetoothAddressType::Random:
                address_kind = BluetoothAddressKind::kRandom;
                break;
            default:
                break;
            }
        } catch (...) {
            // Windows 10 2019 builds do not expose this property. Treating
            // the address type as unspecified keeps the old BLE stack on the
            // one-argument FromBluetoothAddressAsync path.
        }
    }
    // 占用该地址的连接权：已在连接中、僵尸链路安定窗内或失败退避期时返回 false。
    // 调用者必须持有 mutex_。
    auto try_claim_connect = [this, bluetooth_address]() {
        if (connecting_addresses_.contains(bluetooth_address)) return false;
        bool settle_passed = false;
        auto settle = reconnect_settle_until_.find(bluetooth_address);
        if (settle != reconnect_settle_until_.end()) {
            if (std::chrono::steady_clock::now() < settle->second) {
                return false; // 僵尸链路安定窗内，等 OS 拆除旧链路
            }
            reconnect_settle_until_.erase(settle);
            settle_passed = true;
        }
        auto it = connect_cooldown_until_.find(bluetooth_address);
        if (it != connect_cooldown_until_.end()) {
            if (std::chrono::steady_clock::now() < it->second) {
                return false; // 仍在退避期内
            }
            connect_cooldown_until_.erase(it);
        }
        // 经安定窗放行且未被退避拦截的连接：失败时免退避快速重试（见 fail lambda）。
        if (settle_passed) {
            // 打标必须在 cooldown 检查之后，否则拦截返回 false 会残留标记。
            zombie_suspect_marks_[bluetooth_address] =
                {std::chrono::steady_clock::now(), 0};
        }
        connecting_addresses_.emplace(bluetooth_address, std::chrono::steady_clock::now());
        return true;
    };

    bool claimed = false;
    bool stale_session = false;
    bool stale_recently_alive = false;
    {
        std::lock_guard lock(mutex_);
        if (!paired_device_ids_.contains(*device_id)) return;
        auto session_it = sessions_by_device_id_.find(*device_id);
        stale_session = session_it != sessions_by_device_id_.end();
        if (stale_session && session_it->second) {
            const auto last_rx = session_it->second->last_rx_ms.load(std::memory_order_relaxed);
            stale_recently_alive =
                last_rx > 0 && (NowSteadyMs() - last_rx) < kZombieFreshThresholdMs;
        }
        if (!stale_session) claimed = try_claim_connect();
    }
    if (stale_session) {
        // 固件只在未连接时广播（连接成功即停广播，断连后才恢复广播），
        // 因此"已配对设备带着本地已就绪会话重新广播"本身就是旧链路已死的
        // 铁证。典型场景：设备 deep sleep 唤醒重启，而 WinRT 没有对静默
        // 消失的对端投递 ConnectionStatusChanged 断连事件。若不在这里拆
        // 掉陈旧会话，旧会话会一直否决后续广告，重连永远不发生（设备停在
        // pairing 屏，主机却显示已连接）。
        LogBleLine("advertisement from paired VS-" + *device_id +
                   " while session still registered; dropping stale session and reconnecting");
        HandleDeviceDisconnected(*device_id, nullptr);
        if (stale_recently_alive) {
            // 快速重启场景：设备秒级前还在收发，Windows 侧的僵尸链路尚未被
            // 宣告死亡。立即连接会挂在僵尸链路上，由 kSubscribeTimeout 截断并
            // 走 zombie_suspect 免退避重试（最坏 ~2.5s+重试）；但安定窗等 OS
            // 拆完旧链路再连通常一次成功，比重试路径更快更稳。
            std::lock_guard lock(mutex_);
            reconnect_settle_until_[bluetooth_address] =
                std::chrono::steady_clock::now() + kReconnectSettleDelay;
            LogBleLine("reconnect settle VS-" + *device_id + ": delaying " +
                       std::to_string(kReconnectSettleDelay.count()) +
                       "ms for OS to tear down the zombie link");
            return;
        }
        std::lock_guard lock(mutex_);
        claimed = try_claim_connect();
    }
    if (!claimed) {
        // claim 被拒（已在连接中/安定窗/退避期）：广告风暴期每秒发生数十次，
        // 属正常路径，只限流记录，消除「广播到了却无声无息」的诊断盲区。
        const auto now_ms = NowSteadyMs();
        bool should_log = false;
        {
            std::lock_guard lock(mutex_);
            auto& last_log = claim_denied_log_ms_[bluetooth_address];
            if (now_ms - last_log > 60000) {
                last_log = now_ms;
                should_log = true;
            }
        }
        if (should_log) {
            LogBleLine("connect claim denied VS-" + *device_id + " address=" +
                       FormatBluetoothAddress(bluetooth_address) +
                       " (already connecting, settle window, or cooldown)");
        }
        return;
    }

    LogBleLine("advertisement matched VS-" + *device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind) +
               " scan_to_adv_ms=" + std::to_string(ElapsedMs(scan_started_at_)));
    ConnectDeviceAsync(bluetooth_address, address_kind, identity.local_name, *device_id);
}

namespace {

winrt::Windows::Foundation::IAsyncAction WaitMs(std::chrono::milliseconds delay) {
    using winrt::Windows::Foundation::TimeSpan;
    co_await winrt::resume_after(TimeSpan{delay});
}

std::string UnpairStatusName(DeviceUnpairingResultStatus status) {
    switch (status) {
    case DeviceUnpairingResultStatus::Unpaired: return "Unpaired";
    case DeviceUnpairingResultStatus::AlreadyUnpaired: return "AlreadyUnpaired";
    case DeviceUnpairingResultStatus::OperationAlreadyInProgress: return "OperationAlreadyInProgress";
    case DeviceUnpairingResultStatus::AccessDenied: return "AccessDenied";
    case DeviceUnpairingResultStatus::Failed: return "Failed";
    default: return "Unknown";
    }
}

// Clears any stale Windows pairing/bond record so that subsequent GATT
// connections do not attempt to encrypt with a long-term key the peripheral
// no longer holds (which manifests as ESP32 NimBLE BLE_HS_EENCRYPT_KEY_SZ).
winrt::Windows::Foundation::IAsyncOperation<bool> TryUnpairAsync(winrt::hstring device_id) {
    try {
        auto info = co_await DeviceInformation::CreateFromIdAsync(device_id);
        if (!info) {
            LogBleLine("unpair: DeviceInformation not found");
            co_return false;
        }
        auto pairing = info.Pairing();
        if (!pairing) {
            LogBleLine("unpair: no pairing interface");
            co_return false;
        }
        LogBleLine("unpair: IsPaired=" + std::string(pairing.IsPaired() ? "true" : "false") +
                   " CanPair=" + std::string(pairing.CanPair() ? "true" : "false"));
        // Always attempt UnpairAsync even if IsPaired() returns false:
        // the Windows API "paired" state does not always reflect the
        // controller-level bond/LTK cache.
        auto result = co_await pairing.UnpairAsync();
        LogBleLine("unpair: result=" + UnpairStatusName(result.Status()));
        co_return result.Status() == DeviceUnpairingResultStatus::Unpaired ||
               result.Status() == DeviceUnpairingResultStatus::AlreadyUnpaired;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("unpair: exception hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("unpair: unknown exception");
        co_return false;
    }
}

winrt::Windows::Foundation::IAsyncOperation<bool> TryResetBluetoothRadioAsync() {
    using winrt::Windows::Devices::Radios::Radio;
    using winrt::Windows::Devices::Radios::RadioKind;
    using winrt::Windows::Devices::Radios::RadioState;
    using winrt::Windows::Devices::Radios::RadioAccessStatus;
    try {
        auto access = co_await Radio::RequestAccessAsync();
        if (access != RadioAccessStatus::Allowed) {
            LogBleLine("radio reset: access denied");
            co_return false;
        }
        auto radios = co_await Radio::GetRadiosAsync();
        for (const auto& radio : radios) {
            if (radio.Kind() == RadioKind::Bluetooth) {
                LogBleLine("radio reset: turning off");
                co_await radio.SetStateAsync(RadioState::Off);
                co_await WaitMs(std::chrono::milliseconds(2000));
                LogBleLine("radio reset: turning on");
                co_await radio.SetStateAsync(RadioState::On);
                co_await WaitMs(std::chrono::milliseconds(3000));
                LogBleLine("radio reset: complete");
                co_return true;
            }
        }
        LogBleLine("radio reset: no Bluetooth radio found");
        co_return false;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("radio reset: failed hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("radio reset: unknown error");
        co_return false;
    }
}

bool IsLikelyStaleBondError(std::int32_t hresult) {
    // Common HRESULTs we have observed when Windows trips over a stale bond
    // or has been left in an inconsistent state by a previous attempt.
    // A timeout also frequently indicates a stale bond: GetGattServicesAsync
    // hangs indefinitely when Windows holds a long-term key the peripheral
    // no longer recognises.
    //
    // Additions beyond the original four were selected from real-world
    // WinRT BLE traces: E_ACCESS_DENIED when the controller-level encryption
    // handshake fails; E_ELEMENT_NOT_FOUND when the OS GATT cache references
    // a stale attribute database; ERROR_GEN_FAILURE when the BLE radio
    // returns a generic hardware failure after repeated encryption errors;
    // ERROR_OUTOFMEMORY when the OS BLE stack is in a degraded state
    // following bond-related retries.
    return hresult == kErrorBadCommand ||                                           // 0x80070016  ERROR_BAD_COMMAND
           hresult == kErrorTimeout ||                                               // 0x800705B4  ERROR_TIMEOUT
           hresult == static_cast<std::int32_t>(0x800710DF) ||                       // ERROR_DEVICE_NOT_AVAILABLE
           hresult == static_cast<std::int32_t>(0x8007048F) ||                       // ERROR_DEVICE_NOT_CONNECTED
           hresult == static_cast<std::int32_t>(0x80070005) ||                       // E_ACCESS_DENIED
           hresult == static_cast<std::int32_t>(0x80070490) ||                       // E_ELEMENT_NOT_FOUND
           hresult == static_cast<std::int32_t>(0x8007001F) ||                       // ERROR_GEN_FAILURE
           hresult == static_cast<std::int32_t>(0x8007000E);                         // ERROR_OUTOFMEMORY
}

} // namespace

winrt::fire_and_forget BleCentralWin::ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                                         BluetoothAddressKind address_kind,
                                                         std::string local_name,
                                                         std::string device_id) {
    auto session = std::make_shared<DeviceSession>();
    session->bluetooth_address = bluetooth_address;
    session->device = ConnectedDevice{device_id, local_name.empty() ? "VS-" + device_id : local_name};

    auto detach_device_handlers = [device_id](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->ble_device) return;
        if (s->connection_status_token.value != 0) {
            try {
                s->ble_device.ConnectionStatusChanged(s->connection_status_token);
            } catch (...) {
            }
            s->connection_status_token = {};
        }
        if (s->gatt_services_changed_token.value != 0) {
            try {
                s->ble_device.GattServicesChanged(s->gatt_services_changed_token);
            } catch (...) {
            }
            s->gatt_services_changed_token = {};
        }
    };

    auto detach_session_status_handler = [](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->gatt_session || s->session_status_token.value == 0) return;
        try {
            s->gatt_session.SessionStatusChanged(s->session_status_token);
        } catch (...) {
        }
        s->session_status_token = {};
    };

    auto fail = [this, bluetooth_address, device_id, session, detach_device_handlers,
                 detach_session_status_handler](const std::string& message) {
        int zombie_free_retry = 0;
        {
            std::lock_guard lock(mutex_);
            auto mark = zombie_suspect_marks_.find(bluetooth_address);
            if (mark != zombie_suspect_marks_.end() &&
                std::chrono::steady_clock::now() - mark->second.first <
                    kZombieSuspectWindow &&
                mark->second.second < kZombieSuspectMaxFreeRetries) {
                // 窗口期内前几次失败免退避：僵尸未拆完时退避只会拖延回连，
                // 下一条广播（20-30ms 一条）立即重试即可。
                zombie_free_retry = ++mark->second.second;
            } else {
                if (mark != zombie_suspect_marks_.end()) {
                    zombie_suspect_marks_.erase(mark);
                }
                // 连接失败后设置 5 秒退避期，防止扫描→立即重试→再失败的
                // tight-loop。5 秒足以让 Windows BLE 栈从异常状态中恢复。
                connect_cooldown_until_[bluetooth_address] =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
        }
        detach_device_handlers(session);
        detach_session_status_handler(session);
        if (session && session->gatt_session) {
            try {
                session->gatt_session.MaintainConnection(false);
                session->gatt_session.Close();
            } catch (...) {
            }
            session->gatt_session = nullptr;
        }
        if (session && session->ble_device) {
            try {
                session->ble_device.Close();
            } catch (...) {
            }
            session->ble_device = nullptr;
        }
        LogBleLine("connect failed VS-" + device_id + " address=" +
                   FormatBluetoothAddress(bluetooth_address) + " reason=" + message +
                   (zombie_free_retry > 0
                        ? " [zombie-suspect: no cooldown, immediate retry #" +
                              std::to_string(zombie_free_retry) + "]"
                        : ""));
        if (on_connection_error) on_connection_error(device_id, message);
    };

    auto open_device = [&](BluetoothAddressType type) -> winrt::Windows::Foundation::IAsyncOperation<BluetoothLEDevice> {
        if (type == BluetoothAddressType::Unspecified) {
            return BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address);
        }
        return BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address, type);
    };

    auto attach_device_handlers = [this, device_id](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->ble_device) return;
        s->connection_status_token = s->ble_device.ConnectionStatusChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(s)](
                const BluetoothLEDevice& sender, const winrt::Windows::Foundation::IInspectable&) {
                const auto status = sender.ConnectionStatus();
                LogBleLine("connection status VS-" + device_id + " = " +
                           (status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected"));
                if (status == BluetoothConnectionStatus::Disconnected) {
                    HandleDeviceDisconnected(device_id, weak_session.lock());
                }
            });
        // GattServicesChanged fires when Windows invalidates its system-wide
        // GATT service cache for this peripheral (very common with unpaired
        // devices and ESP32/NimBLE peripherals). Logging it helps diagnose
        // why a subsequent service-discovery call may need to be retried.
        s->gatt_services_changed_token = s->ble_device.GattServicesChanged(
            [device_id](const BluetoothLEDevice&, const winrt::Windows::Foundation::IInspectable&) {
                LogBleLine("GattServicesChanged VS-" + device_id);
            });
    };

    // 订阅 GattSession.SessionStatusChanged 作为断连检测的第二通道：
    // ConnectionStatusChanged 对对端静默消失的场景可能永不投递（见
    // Doc/Expe/ble-stale-session-reconnect-deadlock-2026-07-17.md）。
    // 会话转为 Closed 即按断连处理，走与 ConnectionStatusChanged 相同的拆除路径；
    // 若会话未注册（连接尚未就绪或已被拆除），HandleDeviceDisconnected 自然空转。
    auto attach_session_status_handler = [this, device_id](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->gatt_session) return;
        try {
            s->session_status_token = s->gatt_session.SessionStatusChanged(
                [this, device_id, weak_session = std::weak_ptr<DeviceSession>(s)](
                    const GattSession&, const GattSessionStatusChangedEventArgs& args) {
                    const auto status = args.Status();
                    LogBleLine("gatt session status VS-" + device_id + " = " +
                               (status == GattSessionStatus::Active ? "active" : "closed") +
                               " error=" + std::to_string(static_cast<int>(args.Error())));
                    if (status == GattSessionStatus::Closed) {
                        HandleDeviceDisconnected(device_id, weak_session.lock());
                    }
                });
        } catch (const winrt::hresult_error& error) {
            LogBleLine("session status subscribe failed VS-" + device_id +
                       " hr=" + FormatHresult(error.code()));
            s->session_status_token = {};
        }
    };

    try {
        const auto connect_started_at = std::chrono::steady_clock::now();
        auto stage_started_at = connect_started_at;
        auto log_stage = [&](const std::string& stage, const std::string& extra = {}) {
            std::string line = "connect stage VS-" + device_id + " stage=" + stage +
                               " t=" + std::to_string(ElapsedMs(connect_started_at)) + "ms" +
                               " dt=" + std::to_string(ElapsedMs(stage_started_at)) + "ms";
            if (!extra.empty()) line += " " + extra;
            LogBleLine(line);
            stage_started_at = std::chrono::steady_clock::now();
        };
        const auto address_type = ToBluetoothAddressType(address_kind);
        LogBleLine("connecting VS-" + device_id + " address=" + FormatBluetoothAddress(bluetooth_address) +
                   " kind=" + AddressKindName(address_kind));
        log_stage("connect_begin", "address=" + FormatBluetoothAddress(bluetooth_address) +
                                   " kind=" + AddressKindName(address_kind));

        // Skip the pre-emptive unpair that was previously done here.
        // Now that the firmware enables NimBLE bonding, the Windows bond
        // (LTK) should be kept so that reconnection after a Stick or host
        // reboot can skip the pairing exchange and establish encryption
        // directly.  If the bond is stale (e.g. firmware was reflashed
        // without preserving NVS), the service-discovery retry loop below
        // will detect the failure and call TryUnpairAsync as a fallback.

        log_stage("open_device_begin");
        session->ble_device = co_await open_device(address_type);
        if (!session->ble_device) {
            fail("Windows could not open the BLE device. Make sure the device is advertising and try again.");
            co_return;
        }
        LogBleLine("BluetoothLEDevice opened VS-" + device_id +
                   " device_id=" + winrt::to_string(session->ble_device.DeviceId()));
        log_stage("open_device_done", "device_id=" + winrt::to_string(session->ble_device.DeviceId()));
        attach_device_handlers(session);

        // Give the controller a brief moment to actually establish the link
        // before triggering service discovery.
        log_stage("settle_begin", "delay_ms=" + std::to_string(kConnectionSettleDelay.count()));
        co_await WaitMs(kConnectionSettleDelay);
        log_stage("settle_done");

        try {
            log_stage("gatt_session_begin");
            session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                session->ble_device.BluetoothDeviceId());
            if (session->gatt_session) {
                session->gatt_session.MaintainConnection(true);
                attach_session_status_handler(session);
                LogBleLine("GattSession created+maintained VS-" + device_id +
                           " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
                log_stage("gatt_session_done", "max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
            } else {
                log_stage("gatt_session_done", "session=null");
            }
        } catch (const winrt::hresult_error& error) {
            LogBleLine("early GattSession unavailable VS-" + device_id + ": " +
                       FormatHresult(error.code()));
            log_stage("gatt_session_failed", "hr=" + FormatHresult(error.code()));
        }

        // Give MaintainConnection time to establish the link-layer
        // connection before triggering service discovery.
        constexpr int kConnectionPollIntervalMs = 100;
        constexpr int kConnectionPollMaxMs = 4000;
        constexpr int kConnectionPollAttempts = kConnectionPollMaxMs / kConnectionPollIntervalMs;
        log_stage("wait_connected_begin", "poll_interval_ms=" + std::to_string(kConnectionPollIntervalMs));
        int polls = 0;
        for (; polls < kConnectionPollAttempts &&
               session->ble_device.ConnectionStatus() != BluetoothConnectionStatus::Connected;
             ++polls) {
            co_await WaitMs(std::chrono::milliseconds(kConnectionPollIntervalMs));
        }
        if (session->ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
            LogBleLine("link-layer connected VS-" + device_id +
                       " after " + std::to_string(polls * kConnectionPollIntervalMs) + "ms");
        }
        auto pre_status = session->ble_device.ConnectionStatus();
        log_stage("wait_connected_done", "polls=" + std::to_string(polls) +
                                         " status=" + std::string(pre_status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected"));
        LogBleLine("pre-discovery status VS-" + device_id + " = " +
                   (pre_status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected") +
                   " max_pdu_size=" + (session->gatt_session
                       ? std::to_string(session->gatt_session.MaxPduSize()) : "n/a"));

        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceServicesResult service_result{nullptr};
        bool unpair_attempted = false;
        for (int attempt = 1; attempt <= kServiceDiscoveryAttempts; ++attempt) {
            {
                bool cancelled = false;
                {
                    std::lock_guard lock(mutex_);
                    cancelled = cancelled_device_ids_.contains(device_id);
                }
                if (cancelled) {
                    fail("cancelled");
                    co_return;
                }
            }
            const auto cache_mode = (attempt == 1)
                                        ? BluetoothCacheMode::Cached
                                        : BluetoothCacheMode::Uncached;

            std::int32_t throw_hresult = 0;
            std::string throw_message;
            try {
                log_stage("service_discovery_begin", "attempt=" + std::to_string(attempt) +
                                                     " mode=" + std::string(cache_mode == BluetoothCacheMode::Uncached ? "uncached" : "cached"));
                auto async_op = session->ble_device.GetGattServicesAsync(cache_mode);
                // 轮询粒度 100ms：cached 发现常在几十毫秒内完成，500ms 粒度每次连接
                // 会白等近半秒（实测 dt=513/1027ms 均为轮询量化）。
                constexpr int kPollMs = 100;
                const int max_polls = static_cast<int>(kServiceDiscoveryTimeout.count()) / kPollMs;
                for (int p = 0; p < max_polls; ++p) {
                    co_await WaitMs(std::chrono::milliseconds(kPollMs));
                    if (async_op.Status() != winrt::Windows::Foundation::AsyncStatus::Started) break;
                    bool cancelled = false;
                    {
                        std::lock_guard lock(mutex_);
                        cancelled = cancelled_device_ids_.contains(device_id);
                    }
                    if (cancelled) {
                        async_op.Cancel();
                        fail("cancelled");
                        co_return;
                    }
                }
                if (async_op.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
                    async_op.Cancel();
                    throw_hresult = kErrorTimeout;
                    throw_message = "service discovery timed out";
                    log_stage("service_discovery_timeout", "attempt=" + std::to_string(attempt));
                } else {
                    service_result = async_op.GetResults();
                }
            } catch (const winrt::hresult_error& error) {
                throw_hresult = error.code();
                throw_message = winrt::to_string(error.message());
                log_stage("service_discovery_throw", "attempt=" + std::to_string(attempt) +
                                                    " hr=" + FormatHresult(throw_hresult));
            }

            if (throw_hresult != 0) {
                LogBleLine("service discovery threw VS-" + device_id +
                           " attempt=" + std::to_string(attempt) +
                           " hr=" + FormatHresult(throw_hresult) +
                           " message=" + throw_message);
                if (!unpair_attempted && IsLikelyStaleBondError(throw_hresult)) {
                    unpair_attempted = true;
                    LogBleLine("attempting to remove stale Windows pairing for VS-" + device_id);
                    co_await TryUnpairAsync(session->ble_device.DeviceId());

                    // 仅 OS 级 unpair 不够——Windows BTHLE 驱动在
                    // controller 级别独立缓存加密密钥。必须重置
                    // Bluetooth radio 清空硬件 key cache，与下方的
                    // Unreachable 恢复路径保持一致。
                    LogBleLine("stale bond: tearing down device handles before radio reset");
                    detach_device_handlers(session);
                    detach_session_status_handler(session);
                    if (session->gatt_session) {
                        try { session->gatt_session.MaintainConnection(false); session->gatt_session.Close(); } catch (...) {}
                        session->gatt_session = nullptr;
                    }
                    try { session->ble_device.Close(); } catch (...) {}
                    session->ble_device = nullptr;

                    // 标记自建重置，StateChanged(On) 处理器据此跳过重建
                    //（本路径末尾已显式 StartScan）。
                    self_radio_reset_.store(true, std::memory_order_relaxed);
                    if (co_await TryResetBluetoothRadioAsync()) {
                        LogBleLine("stale bond: radio reset succeeded, reopening device");
                    } else {
                        LogBleLine("stale bond: radio reset skipped/failed, reopening after delay");
                        co_await WaitMs(kDeviceReopenDelay);
                    }
                    co_await WaitMs(std::chrono::milliseconds(500));
                    self_radio_reset_.store(false, std::memory_order_relaxed);
                    // 无线电关开会杀死广告 watcher（静默失效，见 ble_central_win.h
                    // RestartForResume 注释）：无论本次重连成败都必须重建扫描，
                    // 否则失败后设备的广播将无人接收，卡 Pairing 只能重启进程。
                    DispatchToUiThread([this] { StartScan(); });

                    session->ble_device = co_await open_device(address_type);
                    if (!session->ble_device) {
                        fail("Windows could not reopen the BLE device after stale bond recovery.");
                        co_return;
                    }
                    attach_device_handlers(session);
                    try {
                        session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                            session->ble_device.BluetoothDeviceId());
                        if (session->gatt_session) {
                            session->gatt_session.MaintainConnection(true);
                            attach_session_status_handler(session);
                        }
                    } catch (...) {}
                    co_await WaitMs(kConnectionSettleDelay);
                    continue;
                }
                if (attempt < kServiceDiscoveryAttempts) {
                    // Tear the device object down completely and re-open it.
                    // Reusing a BluetoothLEDevice that has already returned
                    // 0x80070016 keeps producing the same error indefinitely;
                    // a fresh handle re-runs the OS connection state machine.
                    LogBleLine("recycling BluetoothLEDevice VS-" + device_id);
                    detach_device_handlers(session);
                    detach_session_status_handler(session);
                    try {
                        session->ble_device.Close();
                    } catch (...) {
                    }
                    session->ble_device = nullptr;
                    co_await WaitMs(kDeviceReopenDelay);
                    session->ble_device = co_await open_device(address_type);
                    if (!session->ble_device) {
                        fail("Windows could not reopen the BLE device after a transient failure.");
                        co_return;
                    }
                    attach_device_handlers(session);
                    try {
                        session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                            session->ble_device.BluetoothDeviceId());
                        if (session->gatt_session) {
                            session->gatt_session.MaintainConnection(true);
                            attach_session_status_handler(session);
                        }
                    } catch (...) {}
                    co_await WaitMs(kConnectionSettleDelay);
                    continue;
                }

                std::string hint = " (HRESULT=" + FormatHresult(throw_hresult) + ")";
                if (throw_hresult == kErrorBadCommand) {
                    hint += ". Toggle Bluetooth off and back on from the Windows "
                            "Action Center to flush the OS GATT cache, then retry. "
                            "If the device is listed in \"Bluetooth & devices\" "
                            "settings, remove it first.";
                }
                fail("Windows BLE refused the connection" + hint);
                co_return;
            }

            const auto status = service_result.Status();
            const auto services = service_result.Services();
            const auto count = services.Size();
            LogBleLine("service discovery attempt " + std::to_string(attempt) +
                       " VS-" + device_id + " mode=" +
                       (cache_mode == BluetoothCacheMode::Uncached ? "uncached" : "cached") +
                       " status=" + GattStatusName(status) +
                       " count=" + std::to_string(count));
            log_stage("service_discovery_done", "attempt=" + std::to_string(attempt) +
                                                 " mode=" + std::string(cache_mode == BluetoothCacheMode::Uncached ? "uncached" : "cached") +
                                                 " status=" + GattStatusName(status) +
                                                 " count=" + std::to_string(count));

            if (status == GattCommunicationStatus::Success && count > 0) {
                const winrt::guid wanted{BleProtocol::service_uuid};
                for (uint32_t i = 0; i < count; ++i) {
                    auto candidate = services.GetAt(i);
                    if (candidate.Uuid() == wanted) {
                        session->service = candidate;
                        break;
                    }
                }
                if (session->service) break;
                LogBleLine("VoiceStick service UUID not present in result for VS-" + device_id);
            }

            if (attempt == kServiceDiscoveryAttempts) {
                fail("VoiceStick service discovery failed after retries (status=" +
                     GattStatusName(status) + ", services=" + std::to_string(count) +
                     "). Toggle Bluetooth off and back on, then try pairing again.");
                co_return;
            }

            // Unreachable usually means the peripheral rejected the encrypted
            // link (stale bond / LTK mismatch). Unpair, reset the Bluetooth
            // radio to flush the controller-level key cache, then recycle.
            if (status == GattCommunicationStatus::Unreachable && !unpair_attempted) {
                unpair_attempted = true;
                LogBleLine("Unreachable: removing stale Windows pairing for VS-" + device_id);
                co_await TryUnpairAsync(session->ble_device.DeviceId());

                LogBleLine("Unreachable: tearing down device handles before radio reset");
                detach_device_handlers(session);
                detach_session_status_handler(session);
                if (session->gatt_session) {
                    try { session->gatt_session.MaintainConnection(false); session->gatt_session.Close(); } catch (...) {}
                    session->gatt_session = nullptr;
                }
                try { session->ble_device.Close(); } catch (...) {}
                session->ble_device = nullptr;

                // 标记自建重置，StateChanged(On) 处理器据此跳过重建
                //（本路径末尾已显式 StartScan）。
                self_radio_reset_.store(true, std::memory_order_relaxed);
                if (co_await TryResetBluetoothRadioAsync()) {
                    LogBleLine("Unreachable: radio reset succeeded, reopening device");
                } else {
                    LogBleLine("Unreachable: radio reset skipped/failed, recycling anyway");
                    co_await WaitMs(kDeviceReopenDelay);
                }
                co_await WaitMs(std::chrono::milliseconds(500));
                self_radio_reset_.store(false, std::memory_order_relaxed);
                // 无线电关开会杀死广告 watcher（静默失效，见 ble_central_win.h
                // RestartForResume 注释）：无论本次重连成败都必须重建扫描，
                // 否则失败后设备的广播将无人接收，卡 Pairing 只能重启进程。
                DispatchToUiThread([this] { StartScan(); });

                session->ble_device = co_await open_device(address_type);
                if (!session->ble_device) {
                    fail("Windows could not reopen the BLE device after unpairing.");
                    co_return;
                }
                attach_device_handlers(session);
                try {
                    session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                        session->ble_device.BluetoothDeviceId());
                    if (session->gatt_session) {
                        session->gatt_session.MaintainConnection(true);
                        attach_session_status_handler(session);
                    }
                } catch (...) {}
                co_await WaitMs(kConnectionSettleDelay);
                continue;
            }

            co_await WaitMs(kServiceDiscoveryRetryDelay);
        }

        LogBleLine("service discovered VS-" + device_id);
        log_stage("service_discovered");

        if (!session->gatt_session) {
            try {
                session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                    session->ble_device.BluetoothDeviceId());
                if (session->gatt_session) {
                    session->gatt_session.MaintainConnection(true);
                    attach_session_status_handler(session);
                    LogBleLine("GattSession (late) maintained VS-" + device_id +
                               " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
                }
            } catch (const winrt::hresult_error& error) {
                LogBleLine("GattSession unavailable VS-" + device_id + ": " +
                           FormatHresult(error.code()));
            }
        } else {
            LogBleLine("GattSession confirmed VS-" + device_id +
                       " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
        }

        // Characteristic discovery uses wait_for() which blocks; move off
        // the STA so we don't deadlock the UI message pump.
        co_await winrt::resume_background();

        using winrt::Windows::Foundation::AsyncStatus;
        using GattCharsResult = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicsResult;

        auto discover_characteristic = [&](const winrt::guid& uuid,
                                           const char* label,
                                           BluetoothCacheMode cache_mode) -> GattCharsResult {
            log_stage("characteristic_discovery_begin", "label=" + std::string(label));
            auto op = session->service.GetCharacteristicsForUuidAsync(uuid, cache_mode);
            if (op.wait_for(kCharacteristicDiscoveryTimeout) == AsyncStatus::Started) {
                op.Cancel();
                LogBleLine(std::string(label) + " characteristic discovery timed out VS-" + device_id);
                log_stage("characteristic_discovery_timeout", "label=" + std::string(label));
                return nullptr;
            }
            auto result = op.GetResults();
            log_stage("characteristic_discovery_done", "label=" + std::string(label) +
                                                        " status=" + GattStatusName(result.Status()) +
                                                        " count=" + std::to_string(result.Characteristics().Size()));
            return result;
        };

        // 特征发现先走 Cached：GATT 表跨版本稳定，重连时跳过 3 次空口往返（慢链路况
        // 下实测可省 ~1.7s）。任一失败/为空则三个全部改 Uncached 重试，兜底固件表变更
        // 或系统缓存失效的场景。
        auto audio_result = discover_characteristic(winrt::guid{BleProtocol::audio_uuid}, "audio_tx", BluetoothCacheMode::Cached);
        auto state_result = discover_characteristic(winrt::guid{BleProtocol::state_uuid}, "state_tx", BluetoothCacheMode::Cached);
        auto control_result = discover_characteristic(winrt::guid{BleProtocol::control_uuid}, "control_rx", BluetoothCacheMode::Cached);
        const auto char_ok = [](const GattCharsResult& r) {
            return r && r.Status() == GattCommunicationStatus::Success && r.Characteristics().Size() > 0;
        };
        if (!char_ok(audio_result) || !char_ok(state_result) || !char_ok(control_result)) {
            LogBleLine("cached characteristic discovery incomplete VS-" + device_id +
                       "; retrying uncached");
            audio_result = discover_characteristic(winrt::guid{BleProtocol::audio_uuid}, "audio_tx", BluetoothCacheMode::Uncached);
            state_result = discover_characteristic(winrt::guid{BleProtocol::state_uuid}, "state_tx", BluetoothCacheMode::Uncached);
            control_result = discover_characteristic(winrt::guid{BleProtocol::control_uuid}, "control_rx", BluetoothCacheMode::Uncached);
        }
        if (!audio_result || audio_result.Status() != GattCommunicationStatus::Success || audio_result.Characteristics().Size() == 0) {
            fail("audio_tx discovery failed: " + (audio_result ? GattStatusName(audio_result.Status()) : std::string("timeout")));
            co_return;
        }
        if (!state_result || state_result.Status() != GattCommunicationStatus::Success || state_result.Characteristics().Size() == 0) {
            fail("state_tx discovery failed: " + (state_result ? GattStatusName(state_result.Status()) : std::string("timeout")));
            co_return;
        }
        if (!control_result || control_result.Status() != GattCommunicationStatus::Success || control_result.Characteristics().Size() == 0) {
            fail("control_rx discovery failed: " + (control_result ? GattStatusName(control_result.Status()) : std::string("timeout")));
            co_return;
        }

        session->audio_characteristic = audio_result.Characteristics().GetAt(0);
        session->state_characteristic = state_result.Characteristics().GetAt(0);
        session->control_characteristic = control_result.Characteristics().GetAt(0);
        if (!HasNotify(session->audio_characteristic) || !HasNotify(session->state_characteristic) ||
            !HasWriteWithoutResponse(session->control_characteristic)) {
            fail("required GATT characteristic properties are missing");
            co_return;
        }

        session->audio_value_changed_token = session->audio_characteristic.ValueChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                const GattCharacteristic&, const auto& args) {
                if (auto s = weak_session.lock()) {
                    s->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                }
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                auto frame = BleProtocol::ParseAudioFrame(bytes);
                if (frame.has_value()) {
                    DispatchToUiThread([this, device_id, f = std::move(*frame)]() {
                        if (on_audio_frame) on_audio_frame(device_id, f);
                    });
                }
            });
        session->state_value_changed_token = session->state_characteristic.ValueChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                const GattCharacteristic&, const auto& args) {
                if (auto s = weak_session.lock()) {
                    s->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                }
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                // 先按帧类型分流：0x11 为体感鼠标 motion 二进制帧，高频且不写日志避免刷屏。
                if (bytes.size() >= 2 && bytes[0] == 1 &&
                    bytes[1] == BleProtocol::state_type_motion) {
                    auto motion = BleProtocol::ParseMotionFrame(bytes);
                    if (motion.has_value()) {
                        DispatchToUiThread([this, device_id, m = *motion]() {
                            if (on_motion_event) on_motion_event(device_id, m);
                        });
                    }
                    return;
                }
                LogBleLine("state notify VS-" + device_id +
                           " len=" + std::to_string(bytes.size()) +
                           " preview=" + PreviewBytes(bytes));
                auto event = BleProtocol::ParseStateEvent(bytes);
                if (!event.has_value()) {
                    // power_log 分片帧无 "event" 字段，ParseStateEvent 返回 nullopt；
                    // 先按分片解析，成功则走 on_power_log_fragment 分发。
                    auto fragment = BleProtocol::ParsePowerLogFragment(bytes);
                    if (fragment.has_value()) {
                        DispatchToUiThread([this, device_id, f = std::move(*fragment)]() {
                            if (on_power_log_fragment) on_power_log_fragment(device_id, f);
                        });
                        return;
                    }
                    // power_mgmt 状态帧（供电态自动关机开关）走独立分发。
                    auto usb_auto_off = BleProtocol::ParsePowerMgmtEvent(bytes);
                    if (usb_auto_off.has_value()) {
                        DispatchToUiThread([this, device_id, v = *usb_auto_off]() {
                            if (on_power_mgmt_state) on_power_mgmt_state(device_id, v);
                        });
                        return;
                    }
                    LogBleLine("state notify VS-" + device_id + " parse failed hex=" + HexDump(bytes));
                    return;
                }
                LogBleLine("state event VS-" + device_id + " type=" + event->event +
                           (event->firmware_version.empty()
                                ? std::string()
                                : " firmware=" + event->firmware_version));
                DispatchToUiThread([this, device_id, e = std::move(*event)]() {
                    if (on_state_event) on_state_event(device_id, e);
                });
            });
        // Give the Windows BTHLE driver a brief moment to wire the
        // ValueChanged handlers in before we ask for notifications. Without
        // this gap the very first notification can race past the handler.
        co_await WaitMs(kValueChangedHandlerSettleDelay);

        // Subscribe to state first so the firmware delivers device_info as
        // soon as possible; the audio CCCD write is heavier (Windows seems
        // to delay the next ATT op for a few hundred ms after enabling
        // notifications on a high-throughput characteristic), and putting
        // state second has been observed to push device_info out by ~1s.
        LogBleLine("subscribing state notifications VS-" + device_id);
        log_stage("state_subscribe_begin");
        auto state_op = session->state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        // winrt::when_any 要求各分支同类型（cppwinrt 无 IAsyncOperation/IAsyncAction
        // 混合重载），把订阅操作包一层 IAsyncAction 再与定时器竞速。包装里吞掉
        // 取消/失败时 co_await 抛出的异常，结果仍以 state_op.Status()/GetResults()
        // 为准——否则超时取消后 when_any 内部的 fire_and_forget 分支会 terminate。
        auto state_wait = [](decltype(state_op) op)
            -> winrt::Windows::Foundation::IAsyncAction {
            try { co_await op; } catch (...) {}
        }(state_op);
        co_await winrt::when_any(state_wait, WaitMs(kSubscribeTimeout));
        if (state_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            try { state_op.Cancel(); } catch (...) {}
            fail("state subscribe timeout after " +
                 std::to_string(kSubscribeTimeout.count()) + "ms");
            co_return;
        }
        const auto state_subscribe = state_op.GetResults();
        LogBleLine("state subscribe VS-" + device_id +
                   " status=" + GattStatusName(state_subscribe));
        log_stage("state_subscribe_done", "status=" + GattStatusName(state_subscribe));

        // Let device_info ride out on the air before we issue another ATT op
        // (CCCD writes serialize the ATT channel and can delay the firmware's
        // outgoing notification by tens to hundreds of ms).
        co_await WaitMs(kDeviceInfoSettleDelay);

        LogBleLine("subscribing audio notifications VS-" + device_id);
        log_stage("audio_subscribe_begin");
        auto audio_op = session->audio_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        // 与 state 订阅同款超时（见 kSubscribeTimeout 注释）：链路若恰好死在
        // 两次订阅之间，裸 co_await 会永久挂起，claim 永不释放、重连自我封锁。
        auto audio_wait = [](decltype(audio_op) op)
            -> winrt::Windows::Foundation::IAsyncAction {
            try { co_await op; } catch (...) {}
        }(audio_op);
        co_await winrt::when_any(audio_wait, WaitMs(kSubscribeTimeout));
        if (audio_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            try { audio_op.Cancel(); } catch (...) {}
            fail("audio subscribe timeout after " +
                 std::to_string(kSubscribeTimeout.count()) + "ms");
            co_return;
        }
        const auto audio_subscribe = audio_op.GetResults();
        LogBleLine("audio subscribe VS-" + device_id +
                   " status=" + GattStatusName(audio_subscribe));
        log_stage("audio_subscribe_done", "status=" + GattStatusName(audio_subscribe));

        if (audio_subscribe != GattCommunicationStatus::Success ||
            state_subscribe != GattCommunicationStatus::Success) {
            fail("notification subscription failed: audio=" + GattStatusName(audio_subscribe) +
                 " state=" + GattStatusName(state_subscribe));
            co_return;
        }

        session->audio_subscribed = true;
        session->state_subscribed = true;
        session->ready = true;
        session->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            sessions_by_device_id_[device_id] = session;
            connecting_addresses_.erase(bluetooth_address);
            zombie_suspect_marks_.erase(bluetooth_address);
        }
        PublishConnections();
        LogBleLine("connected VS-" + device_id);
        LogConnectionSnapshot("connected");
        log_stage("ready");
        SendUiState("ready", "", device_id);
    } catch (const winrt::hresult_error& error) {
        std::string message = "WinRT error " + FormatHresult(error.code()) +
                              ": " + winrt::to_string(error.message());
        if (error.code() == kErrorBadCommand) {
            message += ". Open Windows \"Bluetooth & devices\" settings, remove any "
                       "existing VS-" + device_id + " entry, then retry pairing.";
        }
        fail(message);
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("unknown BLE exception");
    }
}

winrt::fire_and_forget BleCentralWin::WriteControlPayloadAsync(std::shared_ptr<DeviceSession> session, ByteVector payload) {
    if (!session || !session->ready || !session->control_characteristic) co_return;
    const std::string device_id = session->device.id;
    GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
    try {
        DataWriter writer;
        writer.WriteBytes(payload);
        status = co_await session->control_characteristic.WriteValueAsync(
            writer.DetachBuffer(), GattWriteOption::WriteWithoutResponse);
    } catch (const winrt::hresult_error& error) {
        // 写入抛异常说明 GATT 对象已不可用（句柄失效/设备对象被关闭/协议栈
        // 重置）：只要会话仍注册在案，就按链路已死处理，拆除后走扫描重连。
        LogBleLine("control write threw VS-" + device_id + " hr=" + FormatHresult(error.code()) +
                   "; tearing down session");
        HandleDeviceDisconnected(device_id, session);
        co_return;
    } catch (...) {
        LogBleLine("control write threw VS-" + device_id + " unknown exception");
        co_return;
    }
    if (status == GattCommunicationStatus::Success) co_return;
    LogBleLine("control write failed VS-" + device_id + " status=" + GattStatusName(status));
    if (status == GattCommunicationStatus::Unreachable) {
        // 对端不可达（链路静默死亡的铁证）：立即拆除会话走扫描重连，不再等
        // 可能永不投递的 WinRT 断连事件。ProtocolError/AccessDenied 只记日志：
        // 链路仍活着，是 ATT 层拒绝，不应误拆。
        HandleDeviceDisconnected(device_id, session);
    }
}

winrt::Windows::Foundation::IAsyncOperation<bool> BleCentralWin::EnsureOtaCharacteristicsAsync(
    std::shared_ptr<DeviceSession> session,
    std::string device_id) {
    using winrt::Windows::Foundation::AsyncStatus;
    using GattCharsResult = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicsResult;

    if (!session || !session->service) co_return false;
    if (session->ota_rx_characteristic && session->ota_state_characteristic &&
        session->ota_state_subscribed) {
        co_return true;
    }

    co_await winrt::resume_background();
    auto discover_ota_characteristic = [&](const winrt::guid& uuid,
                                           const char* label) -> GattCharsResult {
        LogBleLine("OTA lazy characteristic discovery begin VS-" + device_id + " label=" + label);
        auto op = session->service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode::Uncached);
        if (op.wait_for(kCharacteristicDiscoveryTimeout) == AsyncStatus::Started) {
            op.Cancel();
            LogBleLine("OTA lazy characteristic discovery timed out VS-" + device_id + " label=" + label);
            return nullptr;
        }
        auto result = op.GetResults();
        LogBleLine("OTA lazy characteristic discovery done VS-" + device_id +
                   " label=" + label +
                   " status=" + GattStatusName(result.Status()) +
                   " count=" + std::to_string(result.Characteristics().Size()));
        return result;
    };

    if (!session->ota_rx_characteristic) {
        auto ota_rx_result = discover_ota_characteristic(winrt::guid{BleProtocol::ota_rx_uuid}, "ota_rx");
        if (!ota_rx_result || ota_rx_result.Status() != GattCommunicationStatus::Success ||
            ota_rx_result.Characteristics().Size() == 0) {
            co_return false;
        }
        auto characteristic = ota_rx_result.Characteristics().GetAt(0);
        if (!HasWrite(characteristic) && !HasWriteWithoutResponse(characteristic)) {
            co_return false;
        }
        session->ota_rx_characteristic = characteristic;
    }

    if (!session->ota_state_characteristic) {
        auto ota_state_result = discover_ota_characteristic(winrt::guid{BleProtocol::ota_state_uuid}, "ota_state");
        if (!ota_state_result || ota_state_result.Status() != GattCommunicationStatus::Success ||
            ota_state_result.Characteristics().Size() == 0) {
            co_return false;
        }
        auto characteristic = ota_state_result.Characteristics().GetAt(0);
        if (!HasNotify(characteristic)) {
            co_return false;
        }
        session->ota_state_characteristic = characteristic;
    }

    if (session->ota_state_value_changed_token.value == 0) {
        session->ota_state_value_changed_token = session->ota_state_characteristic.ValueChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                const GattCharacteristic&, const auto& args) {
                if (auto s = weak_session.lock()) {
                    s->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                }
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                auto event = BleProtocol::ParseFirmwareOtaStateEvent(bytes);
                if (!event.has_value()) {
                    LogBleLine("ota state notify VS-" + device_id + " parse failed");
                    return;
                }
                DispatchToUiThread([this, device_id, e = std::move(*event)]() {
                    HandleFirmwareOtaStateEvent(device_id, e);
                });
            });
    }

    if (!session->ota_state_subscribed) {
        LogBleLine("subscribing OTA state notifications VS-" + device_id);
        auto status = co_await session->ota_state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("ota state subscribe VS-" + device_id + " status=" + GattStatusName(status));
        if (status != GattCommunicationStatus::Success) {
            co_return false;
        }
        session->ota_state_subscribed = true;
    }

    co_return true;
}

winrt::fire_and_forget BleCentralWin::UpdateFirmwareAsync(
    std::shared_ptr<DeviceSession> session,
    std::shared_ptr<FirmwareUpdateSession> update_session) {
    try {
        if (!session || !update_session ||
            !(co_await EnsureOtaCharacteristicsAsync(session, update_session->device_id))) {
            FinishFirmwareUpdate(update_session, false, "The connected firmware does not expose BLE OTA.");
            co_return;
        }

        auto write_payload = [&](const ByteVector& payload, GattWriteOption option)
            -> winrt::Windows::Foundation::IAsyncOperation<GattCommunicationStatus> {
            return session->ota_rx_characteristic.WriteValueAsync(BufferFromBytes(payload), option);
        };
        const bool ota_supports_write_without_response =
            HasWriteWithoutResponse(session->ota_rx_characteristic);

        LogBleLine("OTA begin VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto begin = BleProtocol::OtaBeginPayload(
            static_cast<std::uint32_t>(update_session->image.size()),
            update_session->transfer_id);
        auto status = co_await write_payload(begin, GattWriteOption::WriteWithResponse);
        if (status != GattCommunicationStatus::Success) {
            FinishFirmwareUpdate(update_session, false, "BLE OTA begin failed: " + GattStatusName(status));
            co_return;
        }

        const std::size_t max_pdu = session->gatt_session ? session->gatt_session.MaxPduSize() : 247;
        const std::size_t chunk_size = std::max<std::size_t>(
            20, std::min<std::size_t>(max_pdu > 15 ? max_pdu - 15 : 20, 244));
        const std::size_t max_in_flight = 48 * 1024;
        LogBleLine("OTA data VS-" + update_session->device_id +
                   " chunk_size=" + std::to_string(chunk_size) +
                   " max_pdu=" + std::to_string(max_pdu) +
                   " write_without_response=" +
                   (ota_supports_write_without_response ? "true" : "false"));
        std::size_t offset = 0;
        std::size_t last_progress = 0;
        while (offset < update_session->image.size()) {
            if (update_session->cancel_requested) co_return;
            if (ota_supports_write_without_response &&
                offset > update_session->device_confirmed_written.load() + max_in_flight) {
                co_await winrt::resume_after(std::chrono::milliseconds(20));
                continue;
            }
            const auto end = std::min(offset + chunk_size, update_session->image.size());
            auto payload = BleProtocol::OtaDataPayload(
                update_session->transfer_id,
                static_cast<std::uint32_t>(offset),
                std::span<const std::uint8_t>(update_session->image.data() + offset, end - offset));
            status = co_await write_payload(
                payload,
                ota_supports_write_without_response
                    ? GattWriteOption::WriteWithoutResponse
                    : GattWriteOption::WriteWithResponse);
            if (status != GattCommunicationStatus::Success) {
                LogBleLine("OTA write failed VS-" + update_session->device_id +
                           " offset=" + std::to_string(offset) +
                           " status=" + GattStatusName(status));
                FinishFirmwareUpdate(update_session, false, "BLE OTA write failed: " + GattStatusName(status));
                co_return;
            }
            offset = end;
            if (offset - last_progress >= 64 * 1024 || offset == update_session->image.size()) {
                last_progress = offset;
                LogBleLine("OTA sent VS-" + update_session->device_id +
                           " written=" + std::to_string(offset) +
                           "/" + std::to_string(update_session->image.size()));
                if (update_session->progress) {
                    update_session->progress(FirmwareUpdateProgress{
                        static_cast<int>(offset),
                        static_cast<int>(update_session->image.size()),
                        false});
                }
            }
        }

        auto final_wait_started = std::chrono::steady_clock::now();
        while (update_session->device_confirmed_written.load() < update_session->image.size()) {
            if (update_session->cancel_requested) co_return;
            if (std::chrono::steady_clock::now() - final_wait_started > std::chrono::seconds(10)) {
                LogBleLine("OTA final device progress timed out VS-" + update_session->device_id +
                           " confirmed=" +
                           std::to_string(update_session->device_confirmed_written.load()) +
                           "/" + std::to_string(update_session->image.size()));
                FinishFirmwareUpdate(update_session, false,
                                     "Device stopped confirming OTA progress.");
                co_return;
            }
            co_await winrt::resume_after(std::chrono::milliseconds(20));
        }

        LogBleLine("OTA end VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto end = BleProtocol::OtaEndPayload(
            update_session->transfer_id,
            static_cast<std::uint32_t>(update_session->image.size()));
        status = co_await write_payload(end, GattWriteOption::WriteWithResponse);
        if (status != GattCommunicationStatus::Success) {
            LogBleLine("OTA end failed VS-" + update_session->device_id +
                       " status=" + GattStatusName(status));
            FinishFirmwareUpdate(update_session, false, "BLE OTA end failed: " + GattStatusName(status));
        }
    } catch (const winrt::hresult_error& error) {
        FinishFirmwareUpdate(update_session, false,
                             "BLE OTA failed: " + FormatHresult(error.code()) +
                                 ": " + winrt::to_string(error.message()));
    } catch (...) {
        FinishFirmwareUpdate(update_session, false, "BLE OTA failed.");
    }
}

void BleCentralWin::HandleFirmwareOtaStateEvent(const std::string& device_id,
                                                const FirmwareOtaStateEvent& event) {
    std::shared_ptr<FirmwareUpdateSession> update_session;
    {
        std::lock_guard lock(mutex_);
        update_session = firmware_update_session_;
    }
    if (!update_session || update_session->device_id != device_id) return;
    if (event.transfer_id.has_value() && *event.transfer_id != update_session->transfer_id) return;

    if (event.event == "progress") {
        if (event.written.has_value() && event.size.has_value() && update_session->progress) {
            update_session->device_confirmed_written.store(*event.written);
            LogBleLine("OTA device progress VS-" + device_id +
                       " written=" + std::to_string(*event.written) +
                       "/" + std::to_string(*event.size));
            update_session->progress(FirmwareUpdateProgress{
                static_cast<int>(*event.written),
                static_cast<int>(*event.size),
                true});
        }
    } else if (event.event == "done") {
        LogBleLine("OTA device done VS-" + device_id);
        if (update_session->progress) {
            update_session->progress(FirmwareUpdateProgress{
                static_cast<int>(update_session->image.size()),
                static_cast<int>(update_session->image.size()),
                true});
        }
        FinishFirmwareUpdate(update_session, true, {});
    } else if (event.event == "error") {
        LogBleLine("OTA device error VS-" + device_id +
                   " code=" + (event.code.empty() ? "unknown" : event.code));
        FinishFirmwareUpdate(update_session, false,
                             "Device rejected OTA: " + (event.code.empty() ? "unknown" : event.code));
    }
}

void BleCentralWin::FinishFirmwareUpdate(std::shared_ptr<FirmwareUpdateSession> update_session,
                                         bool success,
                                         const std::string& message) {
    if (!update_session) return;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_ != update_session) return;
        firmware_update_session_.reset();
    }
    if (update_session->completion) {
        DispatchToUiThread([completion = std::move(update_session->completion), success, message] {
            completion(success, message);
        });
    }
}

void BleCentralWin::HandleDeviceDisconnected(const std::string& device_id,
                                              std::shared_ptr<DeviceSession> session) {
    std::shared_ptr<DeviceSession> removed;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end()) return;
        if (session && it->second != session) return;
        removed = std::move(it->second);
        sessions_by_device_id_.erase(it);
        if (removed) connecting_addresses_.erase(removed->bluetooth_address);
    }
    std::shared_ptr<FirmwareUpdateSession> update_session;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_ && firmware_update_session_->device_id == device_id) {
            update_session = firmware_update_session_;
        }
    }
    if (update_session) {
        FinishFirmwareUpdate(update_session, false, "Device disconnected during firmware update.");
    }
    if (removed) CloseSession(std::move(removed));
    LogBleLine("device disconnected VS-" + device_id + "; restarting scan for reconnection");
    LogConnectionSnapshot("disconnected");
    DispatchToUiThread([this] {
        PublishConnections();
        StartScan();
    });
}

void BleCentralWin::CloseSession(std::shared_ptr<DeviceSession> session) {
    if (!session) return;
    if (session->audio_characteristic && session->audio_value_changed_token.value != 0) {
        try { session->audio_characteristic.ValueChanged(session->audio_value_changed_token); } catch (...) {}
    }
    if (session->state_characteristic && session->state_value_changed_token.value != 0) {
        try { session->state_characteristic.ValueChanged(session->state_value_changed_token); } catch (...) {}
    }
    if (session->ota_state_characteristic && session->ota_state_value_changed_token.value != 0) {
        try { session->ota_state_characteristic.ValueChanged(session->ota_state_value_changed_token); } catch (...) {}
    }
    if (session->ble_device && session->connection_status_token.value != 0) {
        try { session->ble_device.ConnectionStatusChanged(session->connection_status_token); } catch (...) {}
    }
    if (session->ble_device && session->gatt_services_changed_token.value != 0) {
        try { session->ble_device.GattServicesChanged(session->gatt_services_changed_token); } catch (...) {}
    }
    if (session->gatt_session && session->session_status_token.value != 0) {
        try { session->gatt_session.SessionStatusChanged(session->session_status_token); } catch (...) {}
    }
    if (session->gatt_session) {
        try {
            session->gatt_session.MaintainConnection(false);
            session->gatt_session.Close();
        } catch (...) {}
        session->gatt_session = nullptr;
    }
    if (session->service) {
        try { session->service.Close(); } catch (...) {}
        session->service = nullptr;
    }
    if (session->ble_device) {
        try { session->ble_device.Close(); } catch (...) {}
        session->ble_device = nullptr;
    }
    session->audio_characteristic = nullptr;
    session->state_characteristic = nullptr;
    session->control_characteristic = nullptr;
    session->ota_rx_characteristic = nullptr;
    session->ota_state_characteristic = nullptr;
    session->ready = false;
}

void BleCentralWin::CloseSessions() {
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        sessions.swap(sessions_by_device_id_);
    }
    for (auto& [_, session] : sessions) {
        CloseSession(std::move(session));
    }
}

void BleCentralWin::StartHeartbeat() {
    {
        std::lock_guard lock(heartbeat_mutex_);
        if (heartbeat_thread_.joinable()) return;
        heartbeat_stop_ = false;
    }
    heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
}

void BleCentralWin::StopHeartbeat() {
    {
        std::lock_guard lock(heartbeat_mutex_);
        heartbeat_stop_ = true;
    }
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

void BleCentralWin::HeartbeatLoop() {
    std::unique_lock lock(heartbeat_mutex_);
    while (!heartbeat_stop_) {
        if (heartbeat_cv_.wait_for(lock, kHeartbeatInterval, [this] { return heartbeat_stop_; })) break;
        lock.unlock();
        CheckScanHealth();
        ProbeSessions();
        lock.lock();
    }
}

void BleCentralWin::CheckScanHealth() {
    // Claim 滞留清理：ConnectDeviceAsync 若在任一无超时的 WinRT co_await 上
    // 永久挂起（既不 fail 也不 ready），claim 永不释放，该地址的后续广播全被
    // try_claim_connect 否决，重连自我封锁（设备卡 Pairing，重启设备无用，
    // 只有重启进程能恢复）。最坏正常连接实测 ~65s，超时强制释放兜底。
    std::vector<std::uint64_t> expired_claims;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [address, claimed_at] : connecting_addresses_) {
            if (now - claimed_at > kConnectClaimTimeout) expired_claims.push_back(address);
        }
        for (const auto address : expired_claims) connecting_addresses_.erase(address);
    }
    for (const auto address : expired_claims) {
        LogBleLine("connect claim expired after " +
                   std::to_string(kConnectClaimTimeout.count()) +
                   "s (hung connect coroutine?); releasing address=" +
                   FormatBluetoothAddress(address));
    }

    // watcher 静默失效检测：有配对设备待发现、扫描在跑、却长时间收不到任何
    // 广告包（任意设备的广告都算存活证明）→ 判定 watcher 假活并重建。
    {
        std::lock_guard lock(mutex_);
        bool needs_discovery = false;
        for (const auto& id : paired_device_ids_) {
            auto it = sessions_by_device_id_.find(id);
            if (it == sessions_by_device_id_.end() || !it->second->ready) {
                needs_discovery = true;
                break;
            }
        }
        if (!needs_discovery || watcher_ == nullptr) return;
    }
    const auto silent_ms =
        NowSteadyMs() - last_adv_received_ms_.load(std::memory_order_relaxed);
    if (silent_ms < std::chrono::duration_cast<std::chrono::milliseconds>(
                        kScanSilenceTimeout).count()) return;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_scan_watchdog_restart_at_ < kScanWatchdogMinRestartInterval) return;
        last_scan_watchdog_restart_at_ = now;
    }
    LogBleLine("scan watchdog: no advertisements for " +
               std::to_string(silent_ms / 1000) +
               "s with paired device undiscovered; restarting watcher");
    LogConnectionSnapshot("scan_watchdog_rebuild");
    DispatchToUiThread([this] { StartScan(); });
}

void BleCentralWin::ProbeSessions() {
    std::vector<std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session->ready) sessions.push_back(session);
        }
    }
    if (sessions.empty()) return;
    const auto now_ms = NowSteadyMs();
    const auto timeout_ms = kHeartbeatTimeout.count();
    const auto payload = BleProtocol::BatteryStatusRequestPayload();
    for (auto& session : sessions) {
        // 便宜预检：ConnectionStatus 属性已翻成 Disconnected 但事件未投递时，
        // 不必再等心跳超时。属性访问本身抛异常同样按链路已死处理。
        bool link_gone = false;
        try {
            link_gone = session->ble_device &&
                session->ble_device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected;
        } catch (...) {
            link_gone = true;
        }
        const auto last_rx = session->last_rx_ms.load(std::memory_order_relaxed);
        const auto silent_ms = last_rx > 0 ? now_ms - last_rx : -1;
        const bool silent_too_long = silent_ms > timeout_ms;
        if (link_gone || silent_too_long) {
            LogBleLine("heartbeat teardown VS-" + session->device.id +
                       " reason=" + (link_gone ? "connection_status_disconnected" : "no_rx_timeout") +
                       " silent_ms=" + std::to_string(silent_ms));
            HandleDeviceDisconnected(session->device.id, session);
            continue;
        }
        // 向 control_rx 写心跳：对端存活时固件必回 battery_status（刷新
        // last_rx_ms）；链路静默死亡时该写迫使控制器发包，加速协议栈通过
        // supervision timeout / 后续写入失败发现断链。
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

ByteVector BleCentralWin::BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    DataReader reader = DataReader::FromBuffer(buffer);
    ByteVector bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

void BleCentralWin::PublishConnections() {
    if (!on_connection_change) return;
    std::vector<ConnectedDevice> devices;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session->ready) devices.push_back(session->device);
        }
    }
    std::sort(devices.begin(), devices.end(), [](const ConnectedDevice& lhs, const ConnectedDevice& rhs) {
        return lhs.id < rhs.id;
    });
    on_connection_change(devices);
}

void BleCentralWin::LogConnectionSnapshot(std::string_view reason) {
    std::lock_guard lock(mutex_);
    std::string line = "conn_snapshot reason=";
    line += std::string(reason);
    line += " paired=[";
    bool first = true;
    for (const auto& id : paired_device_ids_) {
        if (!first) line += " ";
        first = false;
        line += "VS-" + id;
        auto it = sessions_by_device_id_.find(id);
        if (it != sessions_by_device_id_.end() && it->second) {
            line += it->second->ready ? "(ready)" : "(session,!ready)";
        } else {
            line += "(no_session)";
        }
    }
    line += "]";
    LogBleLine(line);
}

} // namespace voicestick
