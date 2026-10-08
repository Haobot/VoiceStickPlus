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
} // namespace voicestick
