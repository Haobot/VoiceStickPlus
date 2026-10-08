#include "ble_central_win.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"
#include "pair_device_helper.h"
#include "xiaomi_atvv_protocol.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

#include "ble_central_win_util.h"

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
constexpr std::chrono::milliseconds kDeviceReopenDelay{800};
constexpr std::chrono::milliseconds kConnectionSettleDelay{100};
constexpr std::chrono::milliseconds kDeviceInfoSettleDelay{100};


constexpr std::int64_t kZombieFreshThresholdMs{45000};
// 连接失败后的退避期：扫描→立即重试→再失败的 tight-loop 防护。5 秒足以让
// Windows BLE 栈从异常状态中恢复；同时也是失败入队主动重连的首次重试延迟。
constexpr std::chrono::seconds kConnectFailureCooldown{5};


// 广播报告的"会话仍活着"否决窗（见 HandleAdvertisement 的 stale_session 分支）：
// 1s 内还有入站数据的会话不因一条广播被拆；真重启的设备 1s 内发不出任何数据。
constexpr std::int64_t kAliveAdvVetoMs{1000};

// zombie_suspect 免退避重试的窗口与上限：连按重启会产生多重僵尸，
// 单次免退避不够；但无限免退避会让持续失败的设备 tight-loop，
// 故限 15s 窗内最多 3 次，超出回落正常 5s 退避。
constexpr std::chrono::milliseconds kZombieSuspectWindow{15000};
constexpr int kZombieSuspectMaxFreeRetries = 3;

// 连接期活性证明（P0 僵尸自愈）：CCCD 写返回 Success 只说明本机 ATT 层认为写完了。
// 加密上下文陈旧（设备重启致 LTK 轮换、WinRT 缓存未失效）时该写「假成功」而设备侧
// 从未登记（固件 voice_ble 的 state_sub 仍为 0）⇒ voice_ble_is_ready() 恒假、录音必被
// 拒，UI 却显示已连接。判据取「订阅后是否收到任何入站 notify」——固件的
// send_state_json 受 s_state_subscribed 门控，收到任何 state 通知即证明设备侧确实登记
// 了订阅（僵尸链路上一个字节都收不到）。
// 超时 2.5s 的依据：健康链路上主动索要的回包（battery_status，见下方探针）实测
// <1s 到达；僵尸链路上不会有任何回包，2.5s 即判死，远快于旧路径的 90s 心跳超时。
constexpr std::chrono::milliseconds kSessionLivenessTimeout{2500};
constexpr std::chrono::milliseconds kSessionLivenessPollInterval{100};

// 网关模式下「跳过直连遥控器」日志的节流间隔（每个遥控器地址首次必记一条）。
constexpr std::chrono::milliseconds kGatewaySkipLogIntervalMs{300000};


// watcher 异常停止的退避重启：3s 内再次异常停止视为连续失败，指数退避
// 1s→2s→4s→…→30s 封顶。radio 坏状态下无退避会形成每秒数百次扫描热循环。
constexpr std::int64_t kScanRestartStreakWindowMs{3000};
constexpr int kScanRestartMaxBackoffMs{30000};



// 小米电池/保活 setup 的总超时：内部为裸 WinRT co_await 串（服务发现/特征发现/
// CCCD/初始读），任一步被楔死都会挂住 ready 前的连接序列；超时即放弃电池特征
// 继续主连接（心跳保活随之缺失，90s 静默拆除兜底）。
constexpr std::chrono::milliseconds kXiaomiBatterySetupTimeout{5000};


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


struct AdvertisementIdentity {
    std::string local_name;
    bool has_voice_stick_service = false;
    bool has_xiaomi_atvv_service = false;
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
    identity.has_xiaomi_atvv_service = BleProtocol::HasXiaomiAtvvServiceUuid(ad_data);
    return identity;
}





std::uint32_t RandomTransferId() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
    return dist(rng);
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
    // 同理使在途的僵尸自愈提前唤醒线程失效。
    reconnect_wake_epoch_.fetch_add(1, std::memory_order_release);
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
                                        const std::string& name,
                                        DeviceClass device_class) {
    const char* id_prefix = device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-";
    LogBleLine("direct connect enter " + std::string(id_prefix) + device_id + " (pre-lock)");
    {
        std::lock_guard lock(mutex_);
        paired_device_ids_.insert(device_id);
        if (sessions_by_device_id_.contains(device_id)) {
            LogBleLine("direct connect skipped: " + std::string(id_prefix) + device_id +
                       " is already connected");
            return;
        }
        if (connecting_addresses_.contains(bluetooth_address)) {
            LogBleLine("direct connect skipped: " + FormatBluetoothAddress(bluetooth_address) +
                       " is already connecting");
            return;
        }
        connecting_addresses_.emplace(bluetooth_address, std::chrono::steady_clock::now());
    }
    LogBleLine("direct connect requested " + std::string(id_prefix) + device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind,
                       name.empty() ? std::string(id_prefix) + device_id : name, device_id,
                       device_class);
}

namespace {

// 前置声明：定义于本文件后部，与服务发现 stale bond 恢复路径共用。
winrt::Windows::Foundation::IAsyncOperation<bool> TryUnpairAsync(winrt::hstring device_id);

} // namespace

winrt::fire_and_forget BleCentralWin::UnpairOsBondAsync(
    std::string device_id,
    std::uint64_t bluetooth_address,
    std::function<void(bool)> completion) {
    // 「忘记设备」的 OS 侧清理（见 Doc/Plan/device-forget-os-unpair.md）：不依赖
    // 在连会话（调用前会话已拆），统一按地址枚举系统配对记录，VS/RC 两类通用。
    if (bluetooth_address == 0 || !completion) co_return;
    try {
        // 地址属性两个都请求、任一可解析即匹配（真机 probe：DeviceInterface 属性
        // 为无分隔符 12 位十六进制，Aep 属性为冒号分隔，解析函数两者兼容）。
        constexpr wchar_t kIfDeviceAddressProp[] = L"System.DeviceInterface.Bluetooth.DeviceAddress";
        constexpr wchar_t kAepDeviceAddressProp[] = L"System.Devices.Aep.DeviceAddress";
        const auto selector = BluetoothLEDevice::GetDeviceSelectorFromPairingState(true);
        // FindAllAsync 附加属性参数只收 initializer_list/右值 vector（param::
        // async_iterable 约束），const 左值 vector 无法匹配重载。
        const auto infos = co_await DeviceInformation::FindAllAsync(
            selector, {winrt::hstring{kIfDeviceAddressProp}, winrt::hstring{kAepDeviceAddressProp}});
        for (const auto& info : infos) {
            std::optional<std::uint64_t> info_address;
            for (const wchar_t* prop : {kIfDeviceAddressProp, kAepDeviceAddressProp}) {
                const auto value = info.Properties().TryLookup(prop);
                if (!value) continue;
                try {
                    info_address = ParseBluetoothAddressString(
                        winrt::to_string(winrt::unbox_value<winrt::hstring>(value)));
                } catch (...) {
                    // 属性非字符串类型（异常装箱），跳过该属性。
                }
                if (info_address.has_value()) break;
            }
            if (!info_address.has_value() || *info_address != bluetooth_address) continue;
            // 同一物理地址可能枚举出多条接口（GATT/HID/电池各一条），UnpairAsync
            // 作用于设备容器，仅对首条命中执行。
            LogBleLine("os unpair: removing Windows pairing of " +
                       FormatBluetoothAddress(bluetooth_address) + " (device " + device_id + ")");
            const bool ok = co_await TryUnpairAsync(info.Id());
            DispatchToUiThread([completion, ok]() { completion(ok); });
            co_return;
        }
        LogBleLine("os unpair: no Windows pairing record of " +
                   FormatBluetoothAddress(bluetooth_address) + " (device " + device_id +
                   "); treating as done");
        DispatchToUiThread([completion]() { completion(true); });
    } catch (const winrt::hresult_error& error) {
        LogBleLine("os unpair: enumeration failed hr=" + FormatHresult(error.code()) +
                   " (device " + device_id + ")");
        DispatchToUiThread([completion]() { completion(false); });
    } catch (...) {
        LogBleLine("os unpair: unknown exception (device " + device_id + ")");
        DispatchToUiThread([completion]() { completion(false); });
    }
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            } else if (it != sessions_by_device_id_.end() &&
                       it->second->device_class != DeviceClass::kStickS3) {
                // 类别门控跳过（RC 遥控器无屏幕）：与未连接/未就绪区分，避免排障误读。
                LogBleLine("send ui_state skipped: device class not applicable state=" +
                           state + " dev=RC-" + *device_id +
                           " text_len=" + std::to_string(text.size()));
            } else {
                LogBleLine("send ui_state skipped state=" + state +
                           " dev=VS-" + *device_id +
                           " text_len=" + std::to_string(text.size()));
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendGatewayKeymapGet(const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::GatewayKeymapGetPayload();
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }
    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendProtoNegotiate(const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::ProtoNegotiatePayload();
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendGatewayKeymapSet(const std::string& key, bool software,
                                         const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::GatewayKeymapSetPayload(key, software);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendGatewayTargetInfo(const std::string& name,
                                          const std::optional<std::string>& device_id) {
    if (name.empty()) return;
    auto payload = BleProtocol::GatewayTargetInfoPayload(name);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }
    if (targets.empty()) {
        LogBleLine("gateway target info dropped: no ready StickS3 session (device_id=" +
                   device_id.value_or("<any>") + ")");
        return;
    }
    for (auto& session : targets) {
        // 固件把名字绑定到「当前连接对端」的 identity address，故只对目标设备自身下发。
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendGatewaySelectTarget(bool self,
                                             const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::GatewaySelectTargetPayload(self);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
            }
        }
    }
    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendRawControl(const std::string& json,
                                    const std::optional<std::string>& device_id) {
    if (json.empty()) return;
    auto payload = ByteVector(json.begin(), json.end());
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
    if (targets.empty()) {
        LogBleLine("raw control dropped: no ready session (device_id=" +
                   device_id.value_or("<any>") + ")");
        return;
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
    bool class_not_applicable = false;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it != sessions_by_device_id_.end() && it->second->ready &&
            it->second->device_class == DeviceClass::kStickS3) {
            targets.push_back(it->second);
        } else if (it != sessions_by_device_id_.end() &&
                   it->second->device_class != DeviceClass::kStickS3) {
            // RC 遥控器无功耗记账：类别门控跳过，与未连接区分，避免排障误读。
            class_not_applicable = true;
        }
    }
    if (targets.empty()) {
        LogBleLine(class_not_applicable
                       ? "power_log command skipped (device class not applicable) dev=RC-" +
                             device_id
                       : "power_log command skipped (not connected) dev=VS-" + device_id);
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
            if (it != sessions_by_device_id_.end() && it->second->ready &&
                it->second->device_class == DeviceClass::kStickS3) {
                targets.push_back(it->second);
            } else if (it != sessions_by_device_id_.end() &&
                       it->second->device_class != DeviceClass::kStickS3) {
                // 类别门控跳过（RC 遥控器无按键回注）：与未连接/未就绪区分。
                LogBleLine("send remote_button_" + std::string(action_name) +
                           " skipped: device class not applicable dev=RC-" + *device_id);
            } else {
                LogBleLine("send remote_button_" + std::string(action_name) +
                           " skipped dev=VS-" + *device_id);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready && session->device_class == DeviceClass::kStickS3)
                    targets.push_back(session);
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
        if (it->second->device_class != DeviceClass::kStickS3) {
            completion(false, "This device does not support VoiceStick firmware update.");
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

// 网关是否活跃：存在「已就绪且固件上报 gateway 模式」的 StickS3 会话。
// 调用者必须持有 mutex_。老固件不上报 gateway_status ⇒ gateway_mode 恒 false ⇒ 不抑制。
bool BleCentralWin::GatewayModeActiveLocked() const {
    for (const auto& [device_id, session] : sessions_by_device_id_) {
        if (!session) continue;
        if (session->device_class == DeviceClass::kXiaomiRemote2Pro) continue;
        if (session->gateway_mode && session->ready) return true;
    }
    return false;
}

void BleCentralWin::HandleAdvertisement(const BluetoothLEAdvertisementWatcher&,
                                        const BluetoothLEAdvertisementReceivedEventArgs& args) {
    // 任意广告包（不限配对设备）都是 watcher 存活证明，供 CheckScanHealth()
    // 检测 watcher 静默失效。
    last_adv_received_ms_.store(NowSteadyMs(), std::memory_order_relaxed);
    const auto identity = AdvertisementIdentityFrom(args.Advertisement());
    const auto bluetooth_address = args.BluetoothAddress();
    // 双通道发现：StickS3（VS- 名称/service UUID）与小米遥控器（名称白名单/ATVV UUID）。
    // 设备 ID 均归一化为去前缀 4 位大写 hex（paired_device_ids_ 同形存储；VS/RC 撞车
    // 为已知取舍，见 ble_protocol.h NormalizeDeviceId 注释）。
    auto device_id = BleProtocol::DeviceIdFromName(identity.local_name);
    DeviceClass device_class = DeviceClass::kStickS3;
    if (device_id.has_value()) {
        device_class =
            BleProtocol::DeviceClassFromName(identity.local_name).value_or(DeviceClass::kStickS3);
    } else if (identity.has_voice_stick_service) {
        device_id = BleProtocol::DeviceIdFromBluetoothAddress(bluetooth_address);
    } else if (BleProtocol::IsXiaomiRemoteName(identity.local_name) ||
               identity.has_xiaomi_atvv_service) {
        device_id = BleProtocol::DeviceIdFromBluetoothAddress(bluetooth_address);
        device_class = DeviceClass::kXiaomiRemote2Pro;
    }
    if (!device_id.has_value()) return;
    const char* id_prefix = device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-";
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
    // 见下方 stale_session 分支：广播报告不足以判定"活着的会话已死"。
    bool stale_drop_vetoed = false;
    const char* stale_veto_reason = "";
    {
        std::lock_guard lock(mutex_);
        if (!paired_device_ids_.contains(*device_id)) return;
        // 网关模式下抑制「直连遥控器 ATVV」：遥控器 bond 在 StickS3 上，桌面端直连必然
        // 失败（atvv control subscribe timeout），只会刷连接与日志。判据取固件上报的
        // gateway_status（device_info 已到长度预算，故走独立小帧）；老固件不上报 ⇒
        // gateway_mode 缺省 false ⇒ 行为与今天一致，不误抑制。
        // 只挡新连接：已在运行的直连会话不主动拆（P0 教训：不破坏当前可用状态）。
        if (device_class == DeviceClass::kXiaomiRemote2Pro && GatewayModeActiveLocked()) {
            // 每个地址首次必记一条，其后按全局节流（避免广告反复触发刷屏）。
            const auto now = NowSteadyMs();
            const bool first_for_device = gateway_suppressed_rc_.insert(*device_id).second;
            if (first_for_device ||
                now - gateway_skip_log_at_ms_ >= kGatewaySkipLogIntervalMs.count()) {
                gateway_skip_log_at_ms_ = now;
                LogBleLine("gateway mode active: skipping direct ATVV for " +
                           std::string(id_prefix) + *device_id +
                           " (remote is relayed by the StickS3)");
            }
            return;
        }
        // 已配对设备最近一次广播时间：bond 重建要等设备重新广播（见 RepairOsBondAsync）。
        adv_seen_ms_by_address_[bluetooth_address] = NowSteadyMs();
        auto session_it = sessions_by_device_id_.find(*device_id);
        stale_session = session_it != sessions_by_device_id_.end();
        if (stale_session && session_it->second) {
            const auto last_rx = session_it->second->last_rx_ms.load(std::memory_order_relaxed);
            stale_recently_alive =
                last_rx > 0 && (NowSteadyMs() - last_rx) < kZombieFreshThresholdMs;
            // 「广播 ⇒ 旧链路已死」这条铁证的**前提**是"固件只在未连接时广播"。但一条广播报告
            // 并不总是设备此刻真的在广播（Windows 广告 watcher 会送来延迟/复用的报告），
            // 而拆掉一个**活着**的会话代价极高。2026-09-20 真机：网关模式 OTA 传到 ~270KB
            // （开始后 ~30s）时来了这样一条报告，守卫据此拆会话 ⇒ OTA 必死（两次复现都在
            // +30s / 16-17%；设备侧看门狗证明设备一直以为连接还在、数据也一直在写）。
            // 判据：①有固件升级正在跑；②会话 1s 内还有入站数据（真重启的设备发不出数据）。
            const bool ota_in_progress = firmware_update_session_ != nullptr &&
                                         firmware_update_session_->device_id == *device_id;
            const auto last_rx_now = session_it->second->last_rx_ms.load(std::memory_order_relaxed);
            const bool alive_now =
                last_rx_now > 0 && (NowSteadyMs() - last_rx_now) < kAliveAdvVetoMs;
            if (ota_in_progress || alive_now) {
                stale_drop_vetoed = true;
                stale_veto_reason = ota_in_progress ? "ota_in_progress" : "alive_now";
            }
        }
        if (!stale_session) claimed = try_claim_connect();
    }
    if (stale_session && stale_drop_vetoed) {
        LogBleLine("ignoring advertisement for " + std::string(id_prefix) + *device_id +
                   " (session still active: " + stale_veto_reason + ")");
        return;
    }
    if (stale_session) {
        // 固件只在未连接时广播（连接成功即停广播，断连后才恢复广播），
        // 因此"已配对设备带着本地已就绪会话重新广播"本身就是旧链路已死的
        // 铁证。典型场景：设备 deep sleep 唤醒重启，而 WinRT 没有对静默
        // 消失的对端投递 ConnectionStatusChanged 断连事件。若不在这里拆
        // 掉陈旧会话，旧会话会一直否决后续广告，重连永远不发生（设备停在
        // pairing 屏，主机却显示已连接）。
        LogBleLine("advertisement from paired " + std::string(id_prefix) + *device_id +
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
            // 纯等广播有盲区：遥控器 HID 通道仍被 OS 维持时它不再广播，上面的
            // settle 窗等不到下一条广告即永久失联（2026-09-11 事故）。安定窗后
            // 由心跳主动按地址直连兜底。
            BleCentralWin::ProactiveReconnect pending;
            pending.device_id = *device_id;
            pending.address_kind = address_kind;
            pending.device_class = device_class;
            pending.not_before =
                std::chrono::steady_clock::now() + kReconnectSettleDelay;
            pending_proactive_reconnects_[bluetooth_address] = std::move(pending);
            LogBleLine("reconnect settle " + std::string(id_prefix) + *device_id + ": delaying " +
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
            LogBleLine("connect claim denied " + std::string(id_prefix) + *device_id + " address=" +
                       FormatBluetoothAddress(bluetooth_address) +
                       " (already connecting, settle window, or cooldown)");
        }
        return;
    }

    LogBleLine("advertisement matched " + std::string(id_prefix) + *device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind) +
               " scan_to_adv_ms=" + std::to_string(ElapsedMs(scan_started_at_)));
    ConnectDeviceAsync(bluetooth_address, address_kind, identity.local_name, *device_id,
                       device_class);
}

namespace {



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
                                                         std::string device_id,
                                                         DeviceClass device_class) {
    auto session = std::make_shared<DeviceSession>();
    session->bluetooth_address = bluetooth_address;
    // 记下地址类型：僵尸自愈要按地址重连（见 ScheduleZombieReconnect）。
    session->address_kind = address_kind;
    session->device_class = device_class;
    const bool is_xiaomi = device_class == DeviceClass::kXiaomiRemote2Pro;
    const char* id_prefix = is_xiaomi ? "RC-" : "VS-";
    session->device = ConnectedDevice{
        device_id,
        local_name.empty() ? std::string(id_prefix) + device_id : local_name,
        is_xiaomi ? std::string(kHardwareXiaomiRemote2Pro) : std::string()};

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

    auto fail = [this, bluetooth_address, address_kind, device_id, session,
                 detach_device_handlers,
                 detach_session_status_handler](const std::string& message) {
        int zombie_free_retry = 0;
        bool reconnect_queued = false;
        std::chrono::milliseconds reconnect_delay{};
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
                // 连接失败后设置退避期，防止扫描→立即重试→再失败的
                // tight-loop。退避期内 Windows BLE 栈得以从异常状态中恢复。
                connect_cooldown_until_[bluetooth_address] =
                    std::chrono::steady_clock::now() + kConnectFailureCooldown;
            }
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
            // 失败即入队主动重连（2026-09-14 定案，详见
            // PlanReconnectAfterConnectFailure 注释）：广播触发的重连对「设备
            // 在场但不广播」天然失明（小米被系统 HID 连上即停广播），心跳按
            // 地址直连是唯一可靠兜底。取消/已忘记由纯函数守卫拦下。
            const auto plan = BleProtocol::PlanReconnectAfterConnectFailure(
                message, paired_device_ids_.contains(device_id),
                zombie_free_retry > 0, kConnectFailureCooldown);
            if (plan.schedule) {
                BleCentralWin::ProactiveReconnect pending;
                pending.device_id = device_id;
                pending.address_kind = address_kind;
                pending.device_class = session->device_class;
                pending.not_before =
                    std::chrono::steady_clock::now() + plan.delay;
                pending_proactive_reconnects_[bluetooth_address] = std::move(pending);
                reconnect_queued = true;
                reconnect_delay = plan.delay;
            }
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
        LogBleLine(std::string("connect failed ") +
                   (session->device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-") +
                   device_id + " address=" +
                   FormatBluetoothAddress(bluetooth_address) + " reason=" + message +
                   (zombie_free_retry > 0
                        ? " [zombie-suspect: no cooldown, immediate retry #" +
                              std::to_string(zombie_free_retry) + "]"
                        : "") +
                   (reconnect_queued
                        ? " [proactive reconnect queued in " +
                              std::to_string(reconnect_delay.count()) + "ms]"
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
                    auto session = weak_session.lock();
                    // 真实断连（设备重启/关机）= 会话结束：故障期记账清零，下一次连接
                    // 从零副作用的 A 级重新开始。
                    if (session) ClearZombieEpisode(session->bluetooth_address);
                    HandleDeviceDisconnected(device_id, std::move(session));
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
                        auto session = weak_session.lock();
                        if (session) ClearZombieEpisode(session->bluetooth_address);
                        HandleDeviceDisconnected(device_id, std::move(session));
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

        // 僵尸自愈 B 级（2026-09-19 P0）：只重置 Bluetooth radio，**不动系统配对**。
        // 判定依据：《轻量重连已用满仍未恢复 ⇒ 失效的加密上下文不在应用可控层级》
        // 成立（BTHLE 在 controller 级另有 key cache，Close 设备对象清不掉），
        // 但清掉它只需要 radio reset —— 设备侧 bond 仍在 NVS 里，Windows 侧 bond 也
        // 没变，重置后 HID 宿主会用同一把 LTK 自行恢复按键直通。
        // **红线（真机实测得出）**：绝不在自愈里 unpair。整条 Windows 配对记录含
        // HOGP HID 节点，删掉后重建依赖 PairAsync —— 而实测 PairAsync 在「设备已被
        // app 连上（停广播）」时必失败（status=19 Failed，12 次复现）；2026-09-19 03:00
        // 一次全量修复就是这样把按键直通弄死的（BTHPORT 有密钥、Enum\BTHLE 无节点）。
        // 需要重建 bond 的场景改由独立的 bond 看门狗处理（见 RepairOsBondAsync），
        // 它的执行条件是「无会话 + 设备在广播」，与配对对话框流程一致。
        if (ConsumeZombieRepair(bluetooth_address)) {
            LogBleLine("zombie heal B: radio reset VS-" + device_id +
                       " (clears controller key cache; OS pairing left intact)");
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
                LogBleLine("zombie heal B: radio reset succeeded, reopening device");
            } else {
                LogBleLine("zombie heal B: radio reset skipped/failed, reopening after delay");
                co_await WaitMs(kDeviceReopenDelay);
            }
            co_await WaitMs(std::chrono::milliseconds(500));
            self_radio_reset_.store(false, std::memory_order_relaxed);
            // 无线电关开会杀死广告 watcher（静默失效，见 RestartForResume 注释）：
            // 无论本次重连成败都必须重建扫描。
            DispatchToUiThread([this] { StartScan(); });

            session->ble_device = co_await open_device(address_type);
            if (!session->ble_device) {
                fail("Windows could not reopen the BLE device after zombie radio reset.");
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
        }

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
                    fail(std::string(kConnectFailureReasonCancelled));
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
                        fail(std::string(kConnectFailureReasonCancelled));
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
                // B16 红线闸（10-08）：自愈路径绝不 unpair RC——删 Enum\BTHLE 即毁
                // HOGP 按键直通，且 PairAsync 于已连设备必失败；RC 陈旧键走本 fail 路径
                //（用户手动重配对）。VS 无 HOGP 键盘角色，保留陈旧键恢复。
                if (!unpair_attempted && !is_xiaomi && IsLikelyStaleBondError(throw_hresult)) {
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
                    // 恢复动作里的 unpair 顺带删掉了 HOGP HID 节点，必须补回 bond
                    co_await TryRestoreOsBondAsync(session->ble_device, device_id);
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
                const winrt::guid wanted{is_xiaomi ? XiaomiAtvvProtocol::service_uuid
                                                   : BleProtocol::service_uuid};
                for (uint32_t i = 0; i < count; ++i) {
                    auto candidate = services.GetAt(i);
                    if (candidate.Uuid() == wanted) {
                        session->service = candidate;
                        break;
                    }
                }
                if (session->service) break;
                LogBleLine(std::string(is_xiaomi ? "Xiaomi ATVV" : "VoiceStick") +
                           " service UUID not present in result for " + id_prefix + device_id);
            }

            if (attempt == kServiceDiscoveryAttempts) {
                fail(std::string(is_xiaomi ? "Xiaomi ATVV" : "VoiceStick") +
                     " service discovery failed after retries (status=" +
                     GattStatusName(status) + ", services=" + std::to_string(count) +
                     "). Toggle Bluetooth off and back on, then try pairing again.");
                co_return;
            }

            // Unreachable usually means the peripheral rejected the encrypted
            // link (stale bond / LTK mismatch). Unpair, reset the Bluetooth
            // radio to flush the controller-level key cache, then recycle.
            // B16 红线闸（10-08）：同上——RC 自愈绝不 unpair（见 1792 处闸注释）。
            if (status == GattCommunicationStatus::Unreachable && !unpair_attempted
                && !is_xiaomi) {
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
                // 恢复动作里的 unpair 顺带删掉了 HOGP HID 节点，必须补回 bond
                co_await TryRestoreOsBondAsync(session->ble_device, device_id);
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

        if (is_xiaomi) {
            // ---- 小米遥控器 2 Pro（ATVV）连接序列 ----
            // 与 StickS3 的差异：无 ui_state/OTA 通道，主机只写 TX 命令、订阅
            // Audio/Control 两个 notify；ATVV 会话状态机在 UI 线程驱动
            //（XiaomiAtvvSession 线程契约），GATT 回调只负责 marshal 字节。
            const auto atvv_char_ok = [](const GattCharsResult& r) {
                return r && r.Status() == GattCommunicationStatus::Success &&
                       r.Characteristics().Size() > 0;
            };
            auto tx_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::tx_uuid},
                                                     "atvv_tx", BluetoothCacheMode::Cached);
            auto atvv_audio_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::audio_uuid},
                                                             "atvv_audio", BluetoothCacheMode::Cached);
            auto atvv_control_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::control_uuid},
                                                               "atvv_control", BluetoothCacheMode::Cached);
            if (!atvv_char_ok(tx_result) || !atvv_char_ok(atvv_audio_result) ||
                !atvv_char_ok(atvv_control_result)) {
                LogBleLine("cached characteristic discovery incomplete RC-" + device_id +
                           "; retrying uncached");
                tx_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::tx_uuid},
                                                    "atvv_tx", BluetoothCacheMode::Uncached);
                atvv_audio_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::audio_uuid},
                                                            "atvv_audio", BluetoothCacheMode::Uncached);
                atvv_control_result = discover_characteristic(winrt::guid{XiaomiAtvvProtocol::control_uuid},
                                                              "atvv_control", BluetoothCacheMode::Uncached);
            }
            if (!atvv_char_ok(tx_result)) {
                fail("atvv_tx discovery failed: " +
                     (tx_result ? GattStatusName(tx_result.Status()) : std::string("timeout")));
                co_return;
            }
            if (!atvv_char_ok(atvv_audio_result)) {
                fail("atvv_audio discovery failed: " +
                     (atvv_audio_result ? GattStatusName(atvv_audio_result.Status())
                                        : std::string("timeout")));
                co_return;
            }
            if (!atvv_char_ok(atvv_control_result)) {
                fail("atvv_control discovery failed: " +
                     (atvv_control_result ? GattStatusName(atvv_control_result.Status())
                                          : std::string("timeout")));
                co_return;
            }

            session->xiaomi_tx_characteristic = tx_result.Characteristics().GetAt(0);
            session->xiaomi_audio_characteristic = atvv_audio_result.Characteristics().GetAt(0);
            session->xiaomi_control_characteristic = atvv_control_result.Characteristics().GetAt(0);
            if (!HasWriteWithoutResponse(session->xiaomi_tx_characteristic) ||
                !HasNotify(session->xiaomi_audio_characteristic) ||
                !HasNotify(session->xiaomi_control_characteristic)) {
                fail("required GATT characteristic properties are missing");
                co_return;
            }

            session->xiaomi_control_value_changed_token =
                session->xiaomi_control_characteristic.ValueChanged(
                    [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                        const GattCharacteristic&, const auto& args) {
                        auto s = weak_session.lock();
                        if (!s) return;
                        s->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                        auto bytes = BytesFromBuffer(args.CharacteristicValue());
                        LogBleLine("atvv control rx RC-" + device_id + " len=" +
                                   std::to_string(bytes.size()) +
                                   (bytes.empty() ? std::string() : " hex=" + HexDump(bytes)));
                        // 语音键按下：遥控器固件会同步向 OS 多发一个 F5（且按住期间
                        // ~30ms 自动重复），记录时刻供 VoiceF5Suppressor 在 80ms 窗内
                        // 吞掉该按键。2 Pro 的按下帧是 STREAM_START(0x04) 而非
                        // MIC_OPEN(0x08)，两者都要更新锚点。
                        if (!bytes.empty() &&
                            (bytes[0] == XiaomiAtvvProtocol::control_mic_open ||
                             bytes[0] == XiaomiAtvvProtocol::control_stream_start) &&
                            xiaomi_mic_open_sink_) {
                            xiaomi_mic_open_sink_->store(NowSteadyMs(),
                                                         std::memory_order_relaxed);
                        }
                        DispatchToUiThread([this, weak_session, bytes = std::move(bytes)]() {
                            auto s2 = weak_session.lock();
                            if (!s2) return;
                            DriveXiaomiSession(s2, [bytes = std::move(bytes),
                                                    now = NowSteadyMs()](XiaomiAtvvSession& sess) {
                                return sess.HandleControlCommand(bytes, now);
                            });
                        });
                    });
            session->xiaomi_audio_value_changed_token =
                session->xiaomi_audio_characteristic.ValueChanged(
                    [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                        const GattCharacteristic&, const auto& args) {
                        auto s = weak_session.lock();
                        if (!s) return;
                        s->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                        // 音频在流即麦克风开着：每帧刷新 F5 抑制锚点，吞掉按住期间
                        // 持续的 F5 自动重复风暴；松开后音频停，80ms 窗自然到期。
                        if (xiaomi_mic_open_sink_) {
                            xiaomi_mic_open_sink_->store(NowSteadyMs(),
                                                         std::memory_order_relaxed);
                        }
                        auto bytes = BytesFromBuffer(args.CharacteristicValue());
                        const auto audio_rx_n =
                            s->xiaomi_audio_rx_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (audio_rx_n <= 3 || audio_rx_n % 50 == 0) {
                            LogBleLine("atvv audio rx RC-" + device_id + " n=" +
                                       std::to_string(audio_rx_n) + " len=" +
                                       std::to_string(bytes.size()));
                        }
                        DispatchToUiThread([this, weak_session, bytes = std::move(bytes)]() {
                            auto s2 = weak_session.lock();
                            if (!s2) return;
                            DriveXiaomiSession(s2, [bytes = std::move(bytes),
                                                    now = NowSteadyMs()](XiaomiAtvvSession& sess) {
                                return sess.HandleAudioData(bytes, now);
                            });
                        });
                    });
            // 与 StickS3 相同：先让 BTHLE 驱动把 ValueChanged 接线就绪再写 CCCD，
            // 否则首条 notify 可能竞态丢失。
            co_await WaitMs(kValueChangedHandlerSettleDelay);

            // 先 Control 后 Audio：MIC_OPEN/STOP 控制帧优先级高于音频流。
            LogBleLine("subscribing atvv control notifications RC-" + device_id);
            // 同 VS 路径的 CCCD 缓存击穿：先写 None 再写 Notify。
            co_await WriteCccdBestEffortAsync(
                session->xiaomi_control_characteristic,
                GattClientCharacteristicConfigurationDescriptorValue::None, device_id, "atvv_control");
            auto atvv_control_op = session->xiaomi_control_characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify);
            auto atvv_control_wait = [](decltype(atvv_control_op) op)
                -> winrt::Windows::Foundation::IAsyncAction {
                try { co_await op; } catch (...) {}
            }(atvv_control_op);
            co_await winrt::when_any(atvv_control_wait, WaitMs(kSubscribeTimeout));
            if (atvv_control_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
                try { atvv_control_op.Cancel(); } catch (...) {}
                fail("atvv control subscribe timeout after " +
                     std::to_string(kSubscribeTimeout.count()) + "ms");
                co_return;
            }
            const auto atvv_control_subscribe = atvv_control_op.GetResults();
            LogBleLine("atvv control subscribe RC-" + device_id +
                       " status=" + GattStatusName(atvv_control_subscribe));

            LogBleLine("subscribing atvv audio notifications RC-" + device_id);
            co_await WriteCccdBestEffortAsync(
                session->xiaomi_audio_characteristic,
                GattClientCharacteristicConfigurationDescriptorValue::None, device_id, "atvv_audio");
            auto atvv_audio_op = session->xiaomi_audio_characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify);
            auto atvv_audio_wait = [](decltype(atvv_audio_op) op)
                -> winrt::Windows::Foundation::IAsyncAction {
                try { co_await op; } catch (...) {}
            }(atvv_audio_op);
            co_await winrt::when_any(atvv_audio_wait, WaitMs(kSubscribeTimeout));
            if (atvv_audio_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
                try { atvv_audio_op.Cancel(); } catch (...) {}
                fail("atvv audio subscribe timeout after " +
                     std::to_string(kSubscribeTimeout.count()) + "ms");
                co_return;
            }
            const auto atvv_audio_subscribe = atvv_audio_op.GetResults();
            LogBleLine("atvv audio subscribe RC-" + device_id +
                       " status=" + GattStatusName(atvv_audio_subscribe));

            if (atvv_control_subscribe != GattCommunicationStatus::Success ||
                atvv_audio_subscribe != GattCommunicationStatus::Success) {
                fail("atvv notification subscription failed: control=" +
                     GattStatusName(atvv_control_subscribe) +
                     " audio=" + GattStatusName(atvv_audio_subscribe));
                co_return;
            }

            // 可选 Battery Service（0x180F/0x2A19）：只记日志不阻断连接。整段包
            // when_any 总超时（同 ATVV 订阅骨架）：内部裸 co_await 串被 WinRT 楔死时
            // 放弃电池特征继续主连接，避免连接永久挂起、claim 滞留。
            auto battery_op = SetupXiaomiBatteryAsync(session, device_id);
            auto battery_wait = [](winrt::Windows::Foundation::IAsyncAction op)
                -> winrt::Windows::Foundation::IAsyncAction {
                try { co_await op; } catch (...) {}
            }(battery_op);
            co_await winrt::when_any(battery_wait, WaitMs(kXiaomiBatterySetupTimeout));
            if (battery_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
                try { battery_op.Cancel(); } catch (...) {}
                LogBleLine("xiaomi battery setup timeout RC-" + device_id + " after " +
                           std::to_string(kXiaomiBatterySetupTimeout.count()) +
                           "ms; continuing without battery/keepalive probe");
            }

            session->ready = true;
            session->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
            {
                std::lock_guard lock(mutex_);
                sessions_by_device_id_[device_id] = session;
                connecting_addresses_.erase(bluetooth_address);
                zombie_suspect_marks_.erase(bluetooth_address);
            }
            PublishConnections();
            LogBleLine("connected RC-" + device_id);
            LogConnectionSnapshot("connected");
            log_stage("ready");

            // 已知竞态：ready 注册（上行 notify 开始流入）先于 UI 线程的会话创建，
            // 窗口内到达的 MIC_OPEN/音频会被 DriveXiaomiSession 的空会话判空丢弃；
            // 遥控器等不到 MIC_OPEN ACK 会超时自闭合，用户下次按键即自愈，故不引入
            // 额外同步等待（那会拖慢 ready 路径）。
            // ATVV 会话状态机在 UI 线程创建并启动（构造可抛：opus encoder 失败）。
            DispatchToUiThread(
                [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)]() {
                    auto s = weak_session.lock();
                    if (!s) return;
                    XiaomiAtvvSession::Options options;
                    if (xiaomi_options_resolver_) {
                        options = xiaomi_options_resolver_(device_id);
                    }
                    try {
                        std::lock_guard lock(s->xiaomi_mutex);
                        s->xiaomi_atvv_session = std::make_unique<XiaomiAtvvSession>(options);
                        LogBleLine("xiaomi session created RC-" + device_id);
                    } catch (const std::exception& error) {
                        LogBleLine("xiaomi session create failed RC-" + device_id + ": " +
                                   error.what());
                        HandleDeviceDisconnected(device_id, s);
                        return;
                    }
                    // 合成 device_info：协调器据此登记 hardware 能力标签（固件版本
                    // 留空——小米遥控器没有 VoiceStick 固件概念）。
                    StateEvent info;
                    info.event = "device_info";
                    info.hardware = std::string(kHardwareXiaomiRemote2Pro);
                    if (on_state_event) on_state_event(device_id, info);
                    DriveXiaomiSession(s, [now = NowSteadyMs()](XiaomiAtvvSession& sess) {
                        return sess.Start(now);
                    });
                });
            co_return;
        }

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
                if (event->gateway_mode.has_value()) {
                    // 网关模式标记：只存会话里供 HandleAdvertisement 判定，不改变
                    // 与协调器之间的既有事件流（gateway_status 继续下发给 UI 层）。
                    if (auto s = weak_session.lock()) {
                        std::lock_guard lock(mutex_);
                        s->gateway_mode = *event->gateway_mode;
                    }
                    LogBleLine("gateway status VS-" + device_id + " mode=" +
                               (*event->gateway_mode ? "gateway" : "normal"));
                }
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
        // CCCD 缓存击穿：先写 None，使紧随其后的 Notify 一定是「值变化」，Windows 才不会
        // 用缓存短路掉这次写（真机取证见 WriteCccdBestEffortAsync 注释与
        // Doc/Expe/xiaomi-gateway-voice-key-stops-after-while-2026-09-19.md）。
        co_await WriteCccdBestEffortAsync(session->state_characteristic,
                                          GattClientCharacteristicConfigurationDescriptorValue::None,
                                          device_id, "state");
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
        co_await WriteCccdBestEffortAsync(session->audio_characteristic,
                                          GattClientCharacteristicConfigurationDescriptorValue::None,
                                          device_id, "audio");
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

        // 连接期活性证明（P0）：CCCD 写返回 Success 只说明本机 ATT 层认为写完了 ——
        // 加密上下文陈旧（设备重启致 LTK 轮换、WinRT 缓存未失效）时该写「假成功」而
        // 设备侧从未登记（state_sub 仍为 0），此后录音必被拒，UI 却显示已连接。
        // 2026-09-18 真机实例：14:31:05 state/audio 订阅双双 Success、14:31:06 报
        // connected，随后零入站，直到 14:32:42（96.5s）才被心跳判僵尸。
        // 证据取「订阅后是否收到任何入站 notify」。固件在 state_subscribed 置位时立即
        // 下发 device_info + encoder_status（见 voice_ble.c 的 BLE_GAP_EVENT_SUBSCRIBE），
        // 但那两帧是本连接的第一个通知，实测常被 Windows BTHLE 驱动在 handler 接线完成
        // 前吞掉（本机日志里多次连接只有约半数收到 device_info，例如 02:51 那次连接
        // 全程无 device_info）⇒ 不能只依赖它。
        // 因此补一发 battery_status_request（与 30s 心跳同一机制）主动索要回包：
        // 固件 send_state_json 受 s_state_subscribed 门控，收到回包即证明设备侧确实登记了
        // 订阅（僵尸链路不会有任何回包）。fire-and-forget，僵尸链路上该写可能挂到 OS
        // 宣告链路死亡，不能阻塞连接协程。
        [](std::shared_ptr<DeviceSession> s) -> winrt::fire_and_forget {
            try {
                auto payload = BleProtocol::BatteryStatusRequestPayload();
                DataWriter writer;
                writer.WriteBytes(payload);
                co_await s->control_characteristic.WriteValueAsync(
                    writer.DetachBuffer(), GattWriteOption::WriteWithoutResponse);
            } catch (...) {
            }
        }(session);
        for (auto waited = std::chrono::milliseconds::zero();
             session->last_rx_ms.load(std::memory_order_relaxed) <= 0 &&
             waited < kSessionLivenessTimeout;
             waited += kSessionLivenessPollInterval) {
            co_await WaitMs(kSessionLivenessPollInterval);
        }
        if (session->last_rx_ms.load(std::memory_order_relaxed) <= 0) {
            // 诊断探针（区分两种僵尸成因）：强制走空口的未缓存读取。
            //   · 读到应答（哪怕 ATT 错误）= 链路真的在收发 ⇒ 问题在 Windows 把 CCCD 写
            //     短路在本地缓存里；
            //   · 超时 = ATT 承载已死（Windows 只是还记着「已连接」）⇒ 必须重建链路。
            try {
                auto read_op =
                    session->state_characteristic.ReadValueAsync(BluetoothCacheMode::Uncached);
                auto read_wait = [](decltype(read_op) op)
                    -> winrt::Windows::Foundation::IAsyncAction {
                    try { co_await op; } catch (...) {}
                }(read_op);
                co_await winrt::when_any(read_wait, WaitMs(kSessionLivenessTimeout));
                if (read_op.Status() == winrt::Windows::Foundation::AsyncStatus::Completed) {
                    const auto read_result = read_op.GetResults();
                    LogBleLine("zombie probe VS-" + device_id + ": uncached read status=" +
                               GattStatusName(read_result.Status()) + " bytes=" +
                               std::to_string(read_result.Value().Length()) +
                               " (ATT bearer alive ⇒ CCCD write was short-circuited locally)");
                } else {
                    try { read_op.Cancel(); } catch (...) {}
                    LogBleLine("zombie probe VS-" + device_id +
                               ": uncached read TIMED OUT (ATT bearer looks dead)");
                }
            } catch (const winrt::hresult_error& error) {
                LogBleLine("zombie probe VS-" + device_id + " uncached read threw hr=" +
                           FormatHresult(error.code()));
            } catch (...) {
                LogBleLine("zombie probe VS-" + device_id + " uncached read threw unknown");
            }
            const auto heal = NoteZombieEpisode(bluetooth_address, !is_xiaomi);
            LogBleLine("zombie session VS-" + device_id +
                       ": subscriptions reported success but device sent nothing within " +
                       std::to_string(kSessionLivenessTimeout.count()) +
                       "ms (device-side state_sub=0); heal level=" +
                       std::string(ZombieHealLevelName(heal)));
            if (heal == ZombieHealLevel::kUserAction && ShouldPromptZombieUser(bluetooth_address)) {
                LogBleLine("zombie session VS-" + device_id +
                           ": self-heal exhausted (A and B both failed); prompting user to "
                           "re-pair in Windows Bluetooth settings");
                NotifyZombieUserAction(device_id);
            }
            fail("notification subscriptions reported success but the device never acknowledged "
                 "them (zombie session: device-side state_sub=0); reconnecting");
            // fail() 已按地址入队主动重连；这里覆盖成安定窗后的更早到期点并提前唤醒，
            // 让自愈在秒级发生而不是等满一个心跳周期。梯度用满（kUserAction）后不再
            // 几秒一次空转，退到分钟级（此时已提示用户）。
            ScheduleZombieReconnect(session,
                                    heal == ZombieHealLevel::kUserAction ? kZombieGiveUpRetry
                                                                        : kReconnectSettleDelay);
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
            // 会话恢复：故障期记账清零，下次故障重新从零副作用的轻量重连开始。
            zombie_episodes_.erase(bluetooth_address);
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

} // namespace voicestick
