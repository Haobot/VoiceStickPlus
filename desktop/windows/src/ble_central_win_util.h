#pragma once
// N7 cut1：ble_central_win.cc 拆分共享层——尾段（小米/OTA/心跳僵尸）方法所用的
// 文件级助手/常量迁入本头（匿名 ns=每 TU 内部链接，无 ODR 风险）；原 cc 与新 cc
// 均包含本头，定义各得一份。preamble（includes+winrt using）逐字沿用原文件顶部。
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
constexpr int kOsBondRepairMaxAttempts = 1;

constexpr int kOsBondPairAttempts = 3;

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

ByteVector BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    DataReader reader = DataReader::FromBuffer(buffer);
    ByteVector bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) reader.ReadBytes(bytes);
    return bytes;
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

winrt::Windows::Foundation::IAsyncAction WaitMs(std::chrono::milliseconds delay) {
    using winrt::Windows::Foundation::TimeSpan;
    co_await winrt::resume_after(TimeSpan{delay});
}

const char* ZombieHealLevelName(ZombieHealLevel level) {
    switch (level) {
    case ZombieHealLevel::kLightReconnect: return "light-reconnect(A)";
    case ZombieHealLevel::kFullRepair: return "full-repair(B)";
    case ZombieHealLevel::kUserAction: return "user-action";
    }
    return "unknown";
}

// TryUnpairAsync 删掉的是整条 Windows 配对记录，其中**包含 HOGP HID 节点**。
// VS 设备的按键直通完全依赖那条系统级 bond（设备经 HOGP 向本机发键盘/Consumer
// 报告；没有 Enum\BTHLE\Dev_<地址> 节点就没有 HID 链路），所以失效恢复用完
// 「删除」之后必须把 bond 补回来，否则恢复完只剩语音可用、所有按键静默失效。
// 2026-09-18 真机定案：13:31 一次烧录触发的 stale bond 恢复顺手删掉了系统配对，
// 现象即「语音键正常、其他按键全死」，排查耗掉半天。
// 小米遥控器不走这里——它的系统级配对由 AttemptXiaomiOsPairing 负责。
winrt::Windows::Foundation::IAsyncOperation<bool> TryRestoreOsBondAsync(
    BluetoothLEDevice device, std::string device_id) {
    try {
        if (!device) {
            LogBleLine("os bond restore VS-" + device_id + ": device handle gone");
            co_return false;
        }
        auto pairing = device.DeviceInformation().Pairing();
        if (!pairing) {
            LogBleLine("os bond restore VS-" + device_id + ": no pairing interface");
            co_return false;
        }
        if (pairing.IsPaired()) {
            LogBleLine("os bond restore VS-" + device_id + ": already bonded");
            co_return true;
        }
        const auto result = co_await pairing.PairAsync();
        const auto status = result.Status();
        const bool bonded = status == winrt::Windows::Devices::Enumeration::DevicePairingResultStatus::Paired ||
                            status == winrt::Windows::Devices::Enumeration::DevicePairingResultStatus::AlreadyPaired;
        if (bonded) {
            LogBleLine("os bond restore VS-" + device_id + ": bonded (HID passthrough link restored)");
        } else {
            LogBleLine("os bond restore VS-" + device_id + ": failed status=" +
                       std::to_string(static_cast<int>(status)) +
                       " (HID passthrough stays dead until user re-pairs in Windows settings)");
        }
        co_return bonded;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("os bond restore VS-" + device_id + ": exception hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("os bond restore VS-" + device_id + ": unknown exception");
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

}

constexpr std::chrono::milliseconds kCharacteristicDiscoveryTimeout{5000};

constexpr std::chrono::milliseconds kValueChangedHandlerSettleDelay{100};

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

// 主动重连单地址重试间隔：一次直连失败后等下一轮心跳再试，避免热循环。
constexpr std::chrono::seconds kProactiveReconnectRetry{60};

// 链上首个 ATT 操作（state 订阅）的应用层超时。正常几十 ms 完成；撞上未死
// 僵尸链路时 OS 要 ~3.5-4s 才宣告断连，这里 2.5s 提前取消并走失败路径，
// 配合 zombie_suspect 免退避把最坏回连压在 ~5.5s 而不是 ~10s。
constexpr std::chrono::milliseconds kSubscribeTimeout{2500};

// OTA 数据通道的两道保险（2026-09-20 真机：传输中途停住且**没有任何日志**——app 卡在
// WriteValueAsync 的裸 co_await 上，用户侧只看到进度条不动）。
//  - kOtaWriteTimeout：单次分块写的上限。写而无响应（设备在 flash 擦写里卡住、控制器
//    缓冲被占满）时不能永久挂着，超时后明确失败并报出 offset。
//  - kOtaConfirmStallTimeout：设备进度通知停更的上限。app 的流控靠设备回传的
//    device_confirmed_written，若设备不再回传（例如 OTA state 订阅其实没生效），
//    循环会以 20ms 空转永远等下去——必须转成可见失败。
constexpr std::chrono::milliseconds kOtaWriteTimeout{5000};

constexpr std::chrono::milliseconds kOtaConfirmStallTimeout{15000};
// 在途窗口等待期间的降速续发节拍（2026-09-26）：v2.3.8 及更早固件的进度回传间隔
// 是 32KB（现行固件 8KB），大于 24KB 常规窗口——若窗口等待时完全停发，设备永远
// 攒不到下一条回传阈值，双方互等死锁（真机：首条确认 32944 后再次 stalled）。
// 低节拍续发（每 200ms 一块 ≈1.2KB/s）让 8KB 缺口约 7s 内补齐并触发回传，且单位
// 时间在途包极少，不触当年 48KB 高在途把对端控制器灌满断链的条件（2026-09-20）。
constexpr std::chrono::milliseconds kOtaWindowedWriteInterval{200};

// 僵尸自愈故障期：同一地址在窗内的多次僵尸判定累计计入同一故障期（据此从轻量重连
// 升级为全量修复）；会话恢复（活性证明通过）或窗过期即清零，重新从轻量重连开始。
constexpr std::chrono::seconds kZombieEpisodeWindow{600};
// 僵尸拆除后提前唤醒主动重连队列的余量：队列项到期点是 teardown+delay，唤醒线程
// 睡满 delay 后可能早于该时刻几个调度周期，多等一点确保 RunDueProactiveReconnects
// 判定为「已到期」。
constexpr std::chrono::milliseconds kZombieReconnectWakeSlack{200};
// 自愈梯度用满（末级 kUserAction）后的重连间隔：已经提示过用户，不能再每几秒
// 重建一次链路空转（每次尝试都要 1-2s 的 BLE 活动），退到分钟级重试。
constexpr std::chrono::seconds kZombieGiveUpRetry{60};
// 末级提示用户的延迟：故障期开始后多久才弹「请重新配对」。2026-09-19 真机：设备重启后
// 被系统 HID 宿主持有的那段时间里，app 的连接全部「假成功」，梯度很快跑到末级就弹了气泡——
// 而设备 3.5 分钟后就自己重新广播、app 秒级恢复，气泡纯属误报。故延后到 2 分钟：
// 期间只要恢复就不打扰用户，真有硬故障也仍在 2 分钟内拿到指引。
constexpr std::chrono::seconds kZombieUserPromptDelay{120};

// 系统配对看门狗（P0 安全网）：app 会话健康但 Windows 没有系统级 bond 时，HOGP
// 按键直通是死的（设备发的 HID 报告无处可去）——2026-09-18 与 2026-09-19 两次踩到。
// 自愈梯度已收敛为「不删 bond」（B 只重置无线电），所以这里只做「缺失即补」，不破坏
// 任何现有状态。检查间隔 10min、每次运行最多补 3 次，避免反复拆会话空转。
// 每次运行最多补 1 次：重建必须拆掉健康会话（设备需重新广播才能 PairAsync），
// 失败就白付一次语音空窗，所以宁可只试一次、由用户重配兜底。
constexpr std::chrono::minutes kOsBondCheckInterval{10};
// 重建 bond 的两个硬前提（2026-09-19 真机实测）：
// ① 设备必须在广播 —— PairAsync 在设备已被 app 连上（NimBLE 连上即 stop_advertising）
//    时必失败（status=19 Failed，连续 12 次复现）；配对对话框流程（扫描看到广播后再
//    PairAsync）则成功（同日 01:54 真机）。
// ② 需要重试 —— 刚断开/刚重置无线电后第一发常失败。
constexpr std::chrono::seconds kOsBondRepairAdvWait{6};
constexpr std::chrono::seconds kOsBondPairRetryDelay{2};

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

// 标准 GATT Battery Service（小米遥控器电量，可选）与 GAP 设备名（心跳保活兜底读）。
constexpr const wchar_t* kBatteryServiceUuid = L"0000180f-0000-1000-8000-00805f9b34fb";
constexpr const wchar_t* kBatteryLevelUuid = L"00002a19-0000-1000-8000-00805f9b34fb";
constexpr const wchar_t* kGapServiceUuid = L"00001800-0000-1000-8000-00805f9b34fb";
constexpr const wchar_t* kGapDeviceNameUuid = L"00002a00-0000-1000-8000-00805f9b34fb";

// 在途连接 claim 的滞留上限：ConnectDeviceAsync 若在任一无超时的 WinRT
// co_await 上永久挂起（既不 fail 也不 ready），claim 永不释放，该地址的
// 后续广播全被 try_claim_connect 否决，形成重连自我封锁。最坏正常连接
// 耗时实测 ~65s（6 次服务发现重试 + radio reset），120s 留出充足余量。
constexpr std::chrono::seconds kConnectClaimTimeout{120};


} // namespace voicestick
