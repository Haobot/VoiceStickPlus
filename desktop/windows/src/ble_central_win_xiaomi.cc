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
// ---- 小米 ATVV 会话辅助实现 ----

void BleCentralWin::DriveXiaomiSession(
    const std::shared_ptr<DeviceSession>& session,
    const std::function<std::vector<XiaomiAtvvAction>(XiaomiAtvvSession&)>& entry) {
    if (!session) return;
    std::vector<XiaomiAtvvAction> actions;
    {
        std::lock_guard lock(session->xiaomi_mutex);
        if (!session->xiaomi_atvv_session) return;
        actions = entry(*session->xiaomi_atvv_session);
    }
    if (!actions.empty()) DispatchXiaomiActions(session, std::move(actions));
}

void BleCentralWin::DispatchXiaomiActions(const std::shared_ptr<DeviceSession>& session,
                                          std::vector<XiaomiAtvvAction> actions) {
    if (!session) return;
    const std::string device_id = session->device.id;
    for (auto& action : actions) {
        std::visit(
            [&](auto&& a) {
                using T = std::decay_t<decltype(a)>;
                if constexpr (std::is_same_v<T, XiaomiAtvvWriteTx>) {
                    WriteXiaomiTxAsync(session, std::move(a.bytes));
                } else if constexpr (std::is_same_v<T, XiaomiAtvvStateEvent>) {
                    LogBleLine("atvv event RC-" + device_id + " type=" + a.event.event);
                    if (on_state_event) on_state_event(device_id, a.event);
                } else if constexpr (std::is_same_v<T, XiaomiAtvvAudioFrame>) {
                    if (on_audio_frame) on_audio_frame(device_id, a.frame);
                } else if constexpr (std::is_same_v<T, XiaomiAtvvError>) {
                    LogBleLine("xiaomi session error RC-" + device_id + " code=" + a.code);
                    if (a.code == "caps_timeout") {
                        // CAPS 应答超时多为半开链路（ATT 通但对端会话栈已乱）：
                        // 直接拆链走扫描快速重连，而不是只 SetPairingError 后挂着
                        // 等 90s 心跳超时。注意 DriveXiaomiSession 分发在锁外，
                        // 此处拆链不会递归持 xiaomi_mutex。
                        HandleDeviceDisconnected(device_id, session);
                    } else if (on_connection_error) {
                        on_connection_error(device_id, "Xiaomi remote error: " + a.code);
                    }
                }
            },
            std::move(action));
    }
}

winrt::fire_and_forget BleCentralWin::WriteXiaomiTxAsync(std::shared_ptr<DeviceSession> session,
                                                         ByteVector payload) {
    // 特征句柄先拷局部再 co_await：CloseSession 在别的线程会把成员置空，
    // 持局部拷贝消除「判空后、挂起点恢复前」的读写竞争窗口。
    const auto tx = session ? session->xiaomi_tx_characteristic : nullptr;
    if (!tx) co_return;
    const std::string device_id = session->device.id;
    LogBleLine("atvv tx write RC-" + device_id + " len=" + std::to_string(payload.size()) +
               " hex=" + HexDump(payload));
    GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
    try {
        DataWriter writer;
        writer.WriteBytes(payload);
        status = co_await tx.WriteValueAsync(
            writer.DetachBuffer(), GattWriteOption::WriteWithoutResponse);
    } catch (const winrt::hresult_error& error) {
        // 与 control 写同一语义：抛异常说明 GATT 对象已不可用，按链路已死拆除。
        LogBleLine("atvv tx write threw RC-" + device_id + " hr=" + FormatHresult(error.code()) +
                   "; tearing down session");
        HandleDeviceDisconnected(device_id, session);
        co_return;
    } catch (...) {
        LogBleLine("atvv tx write threw RC-" + device_id + " unknown exception");
        co_return;
    }
    if (status == GattCommunicationStatus::Success) co_return;
    LogBleLine("atvv tx write failed RC-" + device_id + " status=" + GattStatusName(status));
    if (status == GattCommunicationStatus::Unreachable) {
        HandleDeviceDisconnected(device_id, session);
    }
}

winrt::Windows::Foundation::IAsyncAction BleCentralWin::SetupXiaomiBatteryAsync(
    std::shared_ptr<DeviceSession> session,
    std::string device_id) {
    if (!session || !session->ble_device) co_return;
    try {
        auto services_result = co_await session->ble_device.GetGattServicesForUuidAsync(
            winrt::guid{kBatteryServiceUuid}, BluetoothCacheMode::Cached);
        if (services_result.Status() == GattCommunicationStatus::Success &&
            services_result.Services().Size() > 0) {
            session->xiaomi_battery_service = services_result.Services().GetAt(0);
            auto chars_result = co_await session->xiaomi_battery_service
                .GetCharacteristicsForUuidAsync(winrt::guid{kBatteryLevelUuid},
                                                BluetoothCacheMode::Cached);
            if (chars_result.Status() == GattCommunicationStatus::Success &&
                chars_result.Characteristics().Size() > 0) {
                session->battery_characteristic = chars_result.Characteristics().GetAt(0);
                session->probe_characteristic = session->battery_characteristic;
                if (HasNotify(session->battery_characteristic)) {
                    session->battery_value_changed_token =
                        session->battery_characteristic.ValueChanged(
                            [this, device_id,
                             weak_session = std::weak_ptr<DeviceSession>(session)](
                                const GattCharacteristic&, const auto& args) {
                                if (auto s = weak_session.lock()) {
                                    s->last_rx_ms.store(NowSteadyMs(),
                                                        std::memory_order_relaxed);
                                }
                                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                                if (bytes.empty()) return;
                                StateEvent event;
                                event.event = "battery_status";
                                event.battery_level = static_cast<int>(bytes[0]);
                                DispatchToUiThread([this, device_id, e = std::move(event)]() {
                                    if (on_state_event) on_state_event(device_id, e);
                                });
                            });
                    // 此处有意省略 ATVV 订阅用的 kValueChangedHandlerSettleDelay：
                    // 电量是低频辅助信息，紧随其后的初始 Uncached 读已兜底拿到一次
                    // 电量，极端情况下最多丢首条 notify，不值得为它对就绪路径再付
                    // 一次 settle 延迟。
                    const auto cccd = co_await session->battery_characteristic
                        .WriteClientCharacteristicConfigurationDescriptorAsync(
                            GattClientCharacteristicConfigurationDescriptorValue::Notify);
                    if (cccd != GattCommunicationStatus::Success) {
                        LogBleLine("battery notify subscribe RC-" + device_id +
                                   " status=" + GattStatusName(cccd));
                    }
                }
                // 初始读：立即拿到一次电量（同时刷新 last_rx 作为心跳基线）。
                const auto read = co_await session->battery_characteristic
                    .ReadValueAsync(BluetoothCacheMode::Uncached);
                if (read.Status() == GattCommunicationStatus::Success) {
                    auto bytes = BytesFromBuffer(read.Value());
                    if (!bytes.empty()) {
                        session->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
                        StateEvent event;
                        event.event = "battery_status";
                        event.battery_level = static_cast<int>(bytes[0]);
                        DispatchToUiThread([this, device_id, e = std::move(event)]() {
                            if (on_state_event) on_state_event(device_id, e);
                        });
                    }
                }
                co_return;
            }
        }
        LogBleLine("battery service unavailable RC-" + device_id +
                   "; falling back to GAP device name as keepalive probe");
    } catch (const winrt::hresult_error& error) {
        LogBleLine("battery service setup threw RC-" + device_id +
                   " hr=" + FormatHresult(error.code()));
    } catch (...) {
        LogBleLine("battery service setup threw RC-" + device_id + " unknown exception");
    }
    // 无电量特征：GAP 设备名读作心跳保活特征（句柄存入 xiaomi_battery_service
    // 保持服务存活并随 CloseSession 关闭）。
    if (session->probe_characteristic) co_return;
    try {
        auto gap_services = co_await session->ble_device.GetGattServicesForUuidAsync(
            winrt::guid{kGapServiceUuid}, BluetoothCacheMode::Cached);
        if (gap_services.Status() == GattCommunicationStatus::Success &&
            gap_services.Services().Size() > 0) {
            session->xiaomi_battery_service = gap_services.Services().GetAt(0);
            auto name_chars = co_await session->xiaomi_battery_service
                .GetCharacteristicsForUuidAsync(winrt::guid{kGapDeviceNameUuid},
                                                BluetoothCacheMode::Cached);
            if (name_chars.Status() == GattCommunicationStatus::Success &&
                name_chars.Characteristics().Size() > 0) {
                session->probe_characteristic = name_chars.Characteristics().GetAt(0);
                LogBleLine("keepalive probe via GAP device name RC-" + device_id);
            }
        }
    } catch (...) {
        LogBleLine("GAP keepalive probe setup threw RC-" + device_id);
    }
}

winrt::fire_and_forget BleCentralWin::ProbeXiaomiSessionAsync(
    std::shared_ptr<DeviceSession> session) {
    // 与 WriteXiaomiTxAsync 同理：特征句柄先拷局部，消除与 CloseSession 置空的竞争窗口。
    const auto probe = session ? session->probe_characteristic : nullptr;
    if (!probe) co_return;
    const std::string device_id = session->device.id;
    GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
    try {
        const auto read = co_await probe.ReadValueAsync(
            BluetoothCacheMode::Uncached);
        status = read.Status();
    } catch (const winrt::hresult_error& error) {
        LogBleLine("keepalive probe threw RC-" + device_id +
                   " hr=" + FormatHresult(error.code()) + "; tearing down session");
        HandleDeviceDisconnected(device_id, session);
        co_return;
    } catch (...) {
        LogBleLine("keepalive probe threw RC-" + device_id +
                   " unknown exception; tearing down session");
        HandleDeviceDisconnected(device_id, session);
        co_return;
    }
    if (status == GattCommunicationStatus::Success) {
        session->last_rx_ms.store(NowSteadyMs(), std::memory_order_relaxed);
        co_return;
    }
    LogBleLine("keepalive probe failed RC-" + device_id + " status=" + GattStatusName(status));
    if (status == GattCommunicationStatus::Unreachable) {
        // 与心跳写同一语义：只对 Unreachable 拆链，ProtocolError/AccessDenied 仅记日志。
        HandleDeviceDisconnected(device_id, session);
    }
}

void BleCentralWin::StopXiaomiSessionBestEffort(const std::shared_ptr<DeviceSession>& session) {
    if (!session) return;
    DriveXiaomiSession(session, [now = NowSteadyMs()](XiaomiAtvvSession& sess) {
        return sess.Stop(now);
    });
}

void BleCentralWin::TickXiaomiSessions() {
    std::vector<std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session->ready && session->device_class == DeviceClass::kXiaomiRemote2Pro) {
                sessions.push_back(session);
            }
        }
    }
    for (auto& session : sessions) {
        DriveXiaomiSession(session, [now = NowSteadyMs()](XiaomiAtvvSession& sess) {
            return sess.Tick(now);
        });
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
        // 与 state/audio 订阅同款两步走：先写 None 击穿 Windows 的 CCCD 缓存，
        // 再写 Notify 并**与超时竞速**。2026-09-20 真机：这里原先直接裸 co_await，
        // 设备侧日志证明写已到达并登记（subscribe attr=37 ota_state notify=1），
        // 但 Windows 的 await 永不完成 ⇒ 整个 OTA 卡在 0%（正是 P4 记账的
        // 「网关模式 OTA 失败」样本之一）。注意本路径失败**不**返回 false：
        // 设备既然登记了订阅，OTA 数据通道仍可用，错过状态通知只影响进度显示。
        co_await WriteCccdBestEffortAsync(session->ota_state_characteristic,
                                          GattClientCharacteristicConfigurationDescriptorValue::None,
                                          device_id, "ota_state");
        auto ota_op = session->ota_state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        auto ota_wait = [](decltype(ota_op) op)
            -> winrt::Windows::Foundation::IAsyncAction {
            try { co_await op; } catch (...) {}
        }(ota_op);
        co_await winrt::when_any(ota_wait, WaitMs(kSubscribeTimeout));
        if (ota_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            try { ota_op.Cancel(); } catch (...) {}
            LogBleLine("ota state subscribe did not complete VS-" + device_id +
                       " within " + std::to_string(kSubscribeTimeout.count()) +
                       "ms; continuing (device-side subscription still effective)");
        } else {
            LogBleLine("ota state subscribe VS-" + device_id +
                       " status=" + GattStatusName(ota_op.GetResults()));
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
        // D6：分块走协议 helper（无下限兜底）——原 max(20,…) 在 MTU 退化（pdu≤20）时
        // 构造出超过可写上限的包，固件必 bad_offset；放不下则报错终止。
        const std::size_t chunk_size = BleProtocol::OtaChunkSizeForPdu(max_pdu);
        if (chunk_size == 0) {
            FinishFirmwareUpdate(update_session, false,
                                 "BLE OTA unsupported ATT MTU: pdu=" +
                                     std::to_string(max_pdu));
            co_return;
        }
        // 在途窗口（app 领先设备已确认字节的上限）按已确认字节数自适应，取值与
        // 历史约束见 BleProtocol::OtaMaxInFlightBytes：首条确认前放宽 40KB（覆盖
        // v2.3.8 及更早固件的 32KB 进度回传间隔，否则互等死锁 15s stalled），
        // 确认流动后收紧 24KB（持续在途过大曾把对端控制器灌满断链，2026-09-20）。
        LogBleLine("OTA data VS-" + update_session->device_id +
                   " chunk_size=" + std::to_string(chunk_size) +
                   " max_pdu=" + std::to_string(max_pdu) +
                   " write_without_response=" +
                   (ota_supports_write_without_response ? "true" : "false"));
        std::size_t offset = 0;
        std::size_t last_progress = 0;
        auto last_confirm_ms = NowSteadyMs();
        std::uint32_t last_confirmed_seen = 0;
        auto last_window_write_ms = NowSteadyMs();
        while (offset < update_session->image.size()) {
            if (update_session->cancel_requested) co_return;
            const std::uint32_t confirmed =
                update_session->device_confirmed_written.load();
            if (confirmed != last_confirmed_seen) {
                last_confirmed_seen = confirmed;
                last_confirm_ms = NowSteadyMs();
            }
            if (ota_supports_write_without_response &&
                offset > confirmed + BleProtocol::OtaMaxInFlightBytes(confirmed)) {
                if (NowSteadyMs() - last_confirm_ms > kOtaConfirmStallTimeout.count()) {
                    LogBleLine("OTA device progress stalled VS-" + update_session->device_id +
                               " sent=" + std::to_string(offset) +
                               " confirmed=" + std::to_string(confirmed));
                    FinishFirmwareUpdate(
                        update_session, false,
                        "Device stopped confirming OTA progress at " +
                            std::to_string(confirmed) + "/" +
                            std::to_string(update_session->image.size()) + " bytes.");
                    co_return;
                }
                // 超窗不完全停发：按低节拍续发（见 kOtaWindowedWriteInterval），
                // 让设备能攒到下一条进度回传阈值；回传一到窗口即恢复正常全速。
                if (NowSteadyMs() - last_window_write_ms <
                    kOtaWindowedWriteInterval.count()) {
                    co_await winrt::resume_after(std::chrono::milliseconds(20));
                    continue;
                }
                last_window_write_ms = NowSteadyMs();
            }
            const auto end = std::min(offset + chunk_size, update_session->image.size());
            auto payload = BleProtocol::OtaDataPayload(
                update_session->transfer_id,
                static_cast<std::uint32_t>(offset),
                std::span<const std::uint8_t>(update_session->image.data() + offset, end - offset));
            // 分块写同样要有上限：真机上这里曾永久挂住（无日志、无失败），
            // 用户只能看到进度条停住。超时即失败并报出 offset，让问题可诊断。
            auto write_op = write_payload(
                payload,
                ota_supports_write_without_response
                    ? GattWriteOption::WriteWithoutResponse
                    : GattWriteOption::WriteWithResponse);
            auto write_wait = [](decltype(write_op) op)
                -> winrt::Windows::Foundation::IAsyncAction {
                try { co_await op; } catch (...) {}
            }(write_op);
            co_await winrt::when_any(write_wait, WaitMs(kOtaWriteTimeout));
            if (write_op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
                try { write_op.Cancel(); } catch (...) {}
                LogBleLine("OTA write timed out VS-" + update_session->device_id +
                           " offset=" + std::to_string(offset) +
                           " confirmed=" +
                           std::to_string(update_session->device_confirmed_written.load()));
                FinishFirmwareUpdate(update_session, false,
                                     "BLE OTA write timed out at offset " +
                                         std::to_string(offset) + " bytes.");
                co_return;
            }
            status = write_op.GetResults();
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
                   " code=" + (event.code.empty() ? "unknown" : event.code) +
                   (event.esp_err.has_value() ? " esp_err=" + std::to_string(*event.esp_err)
                                              : std::string()));
        FinishFirmwareUpdate(update_session, false,
                             "Device rejected OTA: " + (event.code.empty() ? "unknown" : event.code) +
                             (event.esp_err.has_value()
                                  ? " esp_err=" + std::to_string(*event.esp_err)
                                  : std::string()));
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
    // 小米会话拆除前尽力发 MIC_CLOSE（遥控器侧 mic 可能仍开着），再复位状态机。
    // 前缀类感知：removed 缺失（重复拆链/会话已被先摘走的二次调用）时回落到
    // 调用方持有的 session 引用，仍无法判定才用通用 VS- 前缀。
    const auto* class_source = removed ? removed.get() : session.get();
    const char* id_prefix =
        class_source && class_source->device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-"
                                                                                     : "VS-";
    if (removed && removed->device_class == DeviceClass::kXiaomiRemote2Pro) {
        StopXiaomiSessionBestEffort(removed);
    }
    // 断连后一律登记按地址主动直连（2026-09-19 P0 扩展，真机事故取证）：
    // 扫描对「设备在场但不广播」天然失明 —— 设备有 OS 配对时，重启后 ~1s 就会被
    // 系统 HID 宿主连上并 stop_advertising()，此后广告永远等不到（实测 08:16:16
    // 断连 → 08:18:26 设备已作为 HID 设备被宿主连上，app 三分钟零恢复）。
    // 登记是无条件安全的：RunDueProactiveReconnects 在设备未配对/已有会话/已有在途
    // 连接时自动清项，失败后按 kProactiveReconnectRetry{60s} 节流。
    auto reconnect_session = removed ? removed : session;
    if (removed) CloseSession(std::move(removed));
    LogBleLine("device disconnected " + std::string(id_prefix) + device_id +
               "; restarting scan + scheduling address reconnect");
    LogConnectionSnapshot("disconnected");
    ScheduleZombieReconnect(reconnect_session, kReconnectSettleDelay);
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
    {
        std::lock_guard lock(session->xiaomi_mutex);
        session->xiaomi_atvv_session.reset();
    }
    if (session->xiaomi_audio_characteristic &&
        session->xiaomi_audio_value_changed_token.value != 0) {
        try { session->xiaomi_audio_characteristic.ValueChanged(session->xiaomi_audio_value_changed_token); } catch (...) {}
        session->xiaomi_audio_value_changed_token = {};
    }
    if (session->xiaomi_control_characteristic &&
        session->xiaomi_control_value_changed_token.value != 0) {
        try { session->xiaomi_control_characteristic.ValueChanged(session->xiaomi_control_value_changed_token); } catch (...) {}
        session->xiaomi_control_value_changed_token = {};
    }
    if (session->battery_characteristic && session->battery_value_changed_token.value != 0) {
        try { session->battery_characteristic.ValueChanged(session->battery_value_changed_token); } catch (...) {}
        session->battery_value_changed_token = {};
    }
    if (session->xiaomi_battery_service) {
        try { session->xiaomi_battery_service.Close(); } catch (...) {}
        session->xiaomi_battery_service = nullptr;
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
    session->xiaomi_tx_characteristic = nullptr;
    session->xiaomi_audio_characteristic = nullptr;
    session->xiaomi_control_characteristic = nullptr;
    session->battery_characteristic = nullptr;
    session->probe_characteristic = nullptr;
    session->ready = false;
}

void BleCentralWin::CloseSessions() {
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        sessions.swap(sessions_by_device_id_);
    }
    for (auto& [_, session] : sessions) {
        if (session->device_class == DeviceClass::kXiaomiRemote2Pro) {
            StopXiaomiSessionBestEffort(session);
        }
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
        RunDueProactiveReconnects();
        lock.lock();
    }
}

void BleCentralWin::RunDueProactiveReconnects() {
    std::vector<std::pair<std::uint64_t, ProactiveReconnect>> due;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_proactive_reconnects_.begin();
             it != pending_proactive_reconnects_.end();) {
            if (now < it->second.not_before) {
                ++it;
                continue;
            }
            // 已不配对（忘记设备/配置变更）或已连上/连接中：使命完成，清项。
            if (!paired_device_ids_.contains(it->second.device_id) ||
                sessions_by_device_id_.contains(it->second.device_id) ||
                connecting_addresses_.contains(it->first)) {
                it = pending_proactive_reconnects_.erase(it);
                continue;
            }
            due.emplace_back(it->first, it->second);
            // 发起后推进到期点：失败（如遥控器仍半开拒绝 ATT）时下一轮心跳重试，
            // 成功则上面的 sessions 检查在下一跳清项。
            it->second.not_before = now + kProactiveReconnectRetry;
            ++it;
        }
    }
    for (const auto& [address, info] : due) {
        const char* id_prefix =
            info.device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-";
        LogBleLine("proactive reconnect " + std::string(id_prefix) + info.device_id +
                   " address=" + FormatBluetoothAddress(address) +
                   " (heartbeat fallback: stale-session teardown or connect-failure retry)");
        DispatchToUiThread([this, address, info] {
            ConnectPairedDevice(info.device_id, address, info.address_kind,
                                std::string(), info.device_class);
        });
    }
}

ZombieHealLevel BleCentralWin::NoteZombieEpisode(std::uint64_t bluetooth_address,
                                                 bool is_voice_stick) {
    ZombieHealLevel level = ZombieHealLevel::kLightReconnect;
    bool repair_pending = false;
    {
        std::lock_guard lock(mutex_);
        auto& episode = zombie_episodes_[bluetooth_address];
        const auto now = std::chrono::steady_clock::now();
        if (episode.last_at != std::chrono::steady_clock::time_point{} &&
            now - episode.last_at > kZombieEpisodeWindow) {
            // 故障期过期：上一轮自愈已过去很久，重新从零副作用的轻量重连开始。
            episode = ZombieEpisode{};
        }
        if (episode.last_at == std::chrono::steady_clock::time_point{}) {
            episode.started_at = now;
        }
        episode.last_at = now;
        level = BleProtocol::PlanZombieHeal(episode.light_attempts, episode.full_repairs,
                                            is_voice_stick);
        switch (level) {
        case ZombieHealLevel::kLightReconnect:
            ++episode.light_attempts;
            break;
        case ZombieHealLevel::kFullRepair:
            // 记账与实际执行分离：标记留到下一次连接、打开设备之后消费（那时才有
            // 句柄可 unpair），次数在此记入以防标记被提前清掉时无限升级。
            ++episode.full_repairs;
            zombie_repair_pending_.insert(bluetooth_address);
            repair_pending = true;
            break;
        case ZombieHealLevel::kUserAction:
            break;
        }
    }
    if (repair_pending) {
        LogBleLine("zombie heal B scheduled address=" + FormatBluetoothAddress(bluetooth_address) +
                   " (light reconnects exhausted; next connect runs unpair + radio reset + PairAsync)");
    }
    return level;
}

bool BleCentralWin::ShouldPromptZombieUser(std::uint64_t bluetooth_address) const {
    std::lock_guard lock(mutex_);
    auto it = zombie_episodes_.find(bluetooth_address);
    if (it == zombie_episodes_.end()) return false;
    const auto& episode = it->second;
    if (episode.started_at == std::chrono::steady_clock::time_point{}) return false;
    return std::chrono::steady_clock::now() - episode.started_at >= kZombieUserPromptDelay;
}

void BleCentralWin::ClearZombieEpisode(std::uint64_t bluetooth_address) {
    std::lock_guard lock(mutex_);
    zombie_episodes_.erase(bluetooth_address);
}

bool BleCentralWin::ConsumeZombieRepair(std::uint64_t bluetooth_address) {
    std::lock_guard lock(mutex_);
    return zombie_repair_pending_.erase(bluetooth_address) > 0;
}

void BleCentralWin::ScheduleZombieReconnect(const std::shared_ptr<DeviceSession>& session,
                                            std::chrono::milliseconds delay) {
    if (!session) return;
    {
        std::lock_guard lock(mutex_);
        if (!paired_device_ids_.contains(session->device.id)) return;
        ProactiveReconnect pending;
        pending.device_id = session->device.id;
        pending.address_kind = session->address_kind;
        pending.device_class = session->device_class;
        pending.not_before = std::chrono::steady_clock::now() + delay;
        pending_proactive_reconnects_[session->bluetooth_address] = std::move(pending);
    }
    const char* id_prefix =
        session->device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-";
    LogBleLine("zombie reconnect queued " + std::string(id_prefix) + session->device.id +
               " address=" + FormatBluetoothAddress(session->bluetooth_address) + " in " +
               std::to_string(delay.count()) + "ms (heartbeat fallback + early wake)");
    WakeProactiveReconnectsAfter(delay);
}

void BleCentralWin::WakeProactiveReconnectsAfter(std::chrono::milliseconds delay) {
    // 心跳 30s 一拍只作兜底：这里按 delay 提前唤醒队列，把自愈从「最坏一个心跳周期」
    // 压到秒级。代数守卫同 scan_epoch_ 手法，Shutdown 后线程自然失效。
    const auto epoch = reconnect_wake_epoch_.load(std::memory_order_relaxed);
    std::thread([this, epoch, delay] {
        std::this_thread::sleep_for(delay + kZombieReconnectWakeSlack);
        if (reconnect_wake_epoch_.load(std::memory_order_acquire) != epoch) return;
        RunDueProactiveReconnects();
    }).detach();
}

winrt::Windows::Foundation::IAsyncAction BleCentralWin::WriteCccdBestEffortAsync(
    GattCharacteristic characteristic,
    GattClientCharacteristicConfigurationDescriptorValue value,
    std::string device_id,
    std::string label) {
    try {
        auto op = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(value);
        // 同 kSubscribeTimeout 的手法：cppwinrt 的 when_any 不支持 IAsyncOperation 与
        // IAsyncAction 混搭，把操作包一层 IAsyncAction 再与定时器竞速，异常在包装内吞掉
        //（否则超时取消后 when_any 内部的 fire_and_forget 分支会 terminate）。
        auto wait = [](decltype(op) o) -> winrt::Windows::Foundation::IAsyncAction {
            try { co_await o; } catch (...) {}
        }(op);
        co_await winrt::when_any(wait, WaitMs(kSubscribeTimeout));
        if (op.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
            try { op.Cancel(); } catch (...) {}
            LogBleLine(label + " CCCD write did not complete VS-" + device_id + " (ignored)");
        } else {
            // 留痕：这条日志能证明「缓存击穿」这一步真的执行了（排查同类问题时看它）。
            LogBleLine(label + " CCCD cache-bust write(None) VS-" + device_id + " status=" +
                       GattStatusName(op.GetResults()));
        }
    } catch (const winrt::hresult_error& error) {
        LogBleLine(label + " CCCD write threw VS-" + device_id +
                   " hr=" + FormatHresult(error.code()));
    } catch (...) {
        LogBleLine(label + " CCCD write threw VS-" + device_id + " unknown exception");
    }
}

bool BleCentralWin::BeginOsBondCheck(std::uint64_t bluetooth_address) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto it = os_bond_check_at_.find(bluetooth_address);
    if (it != os_bond_check_at_.end() && now - it->second < kOsBondCheckInterval) return false;
    if (os_bond_repair_attempts_[bluetooth_address] >= kOsBondRepairMaxAttempts) return false;
    os_bond_check_at_[bluetooth_address] = now;
    return true;
}

winrt::fire_and_forget BleCentralWin::RepairOsBondAsync(std::shared_ptr<DeviceSession> session,
                                                        std::string device_id) {
    const auto bluetooth_address = session->bluetooth_address;
    const auto address_kind = session->address_kind;
    // 先只读查询，确认真的缺 bond 才动会话：查不到句柄/查询异常一律按「已配对」处理
    //（什么都不做），绝不为一次探测拆掉健康会话。
    bool already_paired = true;
    try {
        if (session->ble_device) {
            auto pairing = session->ble_device.DeviceInformation().Pairing();
            already_paired = pairing && pairing.IsPaired();
        }
    } catch (...) {
        already_paired = true;
    }
    if (already_paired) co_return;

    // 缺 bond 时的三步（每步都有真机依据，见常量注释）：
    // ① 拆会话 —— 设备在 app 连接期间停广播，而 PairAsync 需要 Windows 看到广播；
    // ② 重置无线电 —— Windows 侧此时往往还挂着「僵尸链路」：对端早已消失，
    //    FromBluetoothAddressAsync 仍返回 ConnectionStatus=Connected（2026-09-19 实测，
    //    app 停掉 9s 后依旧如此）。PairAsync 在 Windows 认为设备已连接时必失败
    //    status=19，radio reset 是唯一能清掉该状态的应用层动作；
    // ③ 等设备重新广播后再 PairAsync（配对对话框那条已验证可行的路径）。
    HandleDeviceDisconnected(device_id, session);
    DispatchToUiThread([this] { StartScan(); });
    LogBleLine("os bond repair VS-" + device_id + ": session dropped, resetting radio");
    self_radio_reset_.store(true, std::memory_order_relaxed);
    const bool radio_reset = co_await TryResetBluetoothRadioAsync();
    co_await WaitMs(std::chrono::milliseconds(500));
    self_radio_reset_.store(false, std::memory_order_relaxed);
    // 无线电关开会杀死广告 watcher（静默失效），必须重建扫描并重新取广播基线。
    DispatchToUiThread([this] { StartScan(); });
    LogBleLine(std::string("os bond repair VS-") + device_id +
               (radio_reset ? ": radio reset ok (clears Windows zombie link state); waiting for advertisement"
                            : ": radio reset skipped/failed; waiting for advertisement anyway"));
    const auto adv_baseline = NowSteadyMs();
    bool advertised = false;
    const auto adv_deadline = std::chrono::steady_clock::now() + kOsBondRepairAdvWait;
    while (std::chrono::steady_clock::now() < adv_deadline) {
        co_await WaitMs(std::chrono::milliseconds(250));
        {
            std::lock_guard lock(mutex_);
            auto it = adv_seen_ms_by_address_.find(bluetooth_address);
            if (it != adv_seen_ms_by_address_.end() && it->second >= adv_baseline) {
                advertised = true;
                break;
            }
        }
    }
    if (!advertised) {
        LogBleLine("os bond repair VS-" + device_id +
                   ": no fresh advertisement seen; deferring (HOGP stays dead until re-paired)");
        ScheduleZombieReconnect(session, kReconnectSettleDelay);
        co_return;
    }

    bool bonded = false;
    for (int attempt = 1; attempt <= kOsBondPairAttempts && !bonded; ++attempt) {
        try {
            const auto address_type = ToBluetoothAddressType(address_kind);
            auto device = address_type == BluetoothAddressType::Unspecified
                              ? co_await BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address)
                              : co_await BluetoothLEDevice::FromBluetoothAddressAsync(
                                    bluetooth_address, address_type);
            if (device) {
                bonded = co_await TryRestoreOsBondAsync(device, device_id);
                LogBleLine("os bond repair VS-" + device_id + ": attempt " +
                           std::to_string(attempt) + "/" + std::to_string(kOsBondPairAttempts) +
                           (bonded ? " bonded" : " failed"));
                try { device.Close(); } catch (...) {}
            } else {
                LogBleLine("os bond repair VS-" + device_id + ": attempt " +
                           std::to_string(attempt) + " device handle unavailable");
            }
        } catch (const winrt::hresult_error& error) {
            LogBleLine("os bond repair VS-" + device_id + ": attempt " + std::to_string(attempt) +
                       " threw hr=" + FormatHresult(error.code()));
        } catch (...) {
            LogBleLine("os bond repair VS-" + device_id + ": attempt " + std::to_string(attempt) +
                       " threw unknown exception");
        }
        if (!bonded) co_await WaitMs(kOsBondPairRetryDelay);
    }
    {
        std::lock_guard lock(mutex_);
        ++os_bond_repair_attempts_[bluetooth_address];
    }
    LogBleLine(std::string("os bond repair VS-") + device_id +
               (bonded ? ": bonded (HOGP key passthrough restored)" : ": FAILED after retries (HOGP key passthrough stays dead until user re-pairs in Windows settings)"));
    // 无论成败都重新连上：语音链路不依赖系统配对，别把它一起丢了。
    ScheduleZombieReconnect(session, kReconnectSettleDelay);
}

void BleCentralWin::NotifyZombieUserAction(const std::string& device_id) {
    if (!on_session_zombie) return;
    auto notify = on_session_zombie;
    DispatchToUiThread([notify = std::move(notify), device_id] { notify(device_id); });
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
        const auto action = BleProtocol::PlanZombieRecovery(
            link_gone, silent_ms, timeout_ms,
            session->device_class != DeviceClass::kXiaomiRemote2Pro);
        // 僵尸拆除后的主动直连延迟：自愈梯度还有余量时用安定窗（秒级自愈），
        // 用满（末级 kUserAction，已提示用户）后退到分钟级，避免空转重建链路。
        auto zombie_reconnect_delay = kReconnectSettleDelay;
        if (action != ZombieRecoveryAction::kNone) {
            LogBleLine(std::string("heartbeat teardown ") +
                       (session->device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-") +
                       session->device.id +
                       " reason=" + (link_gone ? "connection_status_disconnected" : "no_rx_timeout") +
                       " silent_ms=" + std::to_string(silent_ms));
            // 顺序要紧：先拆会话（HandleDeviceDisconnected 内已排一次按地址直连），
            // 僵尸路径随后再登记本次故障——真断连（link_gone）会清掉故障期记账，
            // 而僵尸判定必须重新计数，否则梯度被自己在同一轮里清空。
            const bool zombie = action == ZombieRecoveryAction::kRepairBond;
            const auto zombie_device_id = session->device.id;
            const auto zombie_address = session->bluetooth_address;
            HandleDeviceDisconnected(zombie_device_id, session);
            if (!zombie) {
                // 链路真的断了：本次会话已结束，故障期记账清零，下次（可能是设备重启
                // 后的全新链路）重新从零副作用的 A 级开始。
                ClearZombieEpisode(zombie_address);
                continue;
            }
            // 僵尸会话：订阅/写入全部假成功而设备侧从未登记（固件日志 send_state_json
            // gated: state_sub=0），录音因此被拒。此时设备多半已被系统 HID 宿主连上并
            // 停止广播，重扫永远等不到（实测 14:55 拆除后连续 8 小时零恢复）。
            LogBleLine("zombie session VS-" + zombie_device_id +
                       ": subscriptions reported success but device never registered them");
            const auto heal = NoteZombieEpisode(zombie_address, true);
            LogBleLine("zombie heal level=" + std::string(ZombieHealLevelName(heal)) + " VS-" +
                       zombie_device_id);
            if (heal == ZombieHealLevel::kUserAction) {
                // 轻量重连与全量修复都用满仍失败：系统配对保持完好，转用户处理。
                // 但要等故障期真的持续够久才提示——设备被系统 HID 宿主短暂持有时
                // 梯度会很快跑到末级，而它往往几秒~几分钟后自己重新广播、app 即恢复
                //（2026-09-19 真机：末级气泡弹出后 3.5 分钟就自行恢复，纯误报）。
                if (ShouldPromptZombieUser(zombie_address)) {
                    LogBleLine("zombie session VS-" + zombie_device_id +
                               ": self-heal exhausted (A and B both failed); prompting user to "
                               "re-pair in Windows Bluetooth settings");
                    NotifyZombieUserAction(zombie_device_id);
                } else {
                    LogBleLine("zombie session VS-" + zombie_device_id +
                               ": self-heal exhausted but episode younger than " +
                               std::to_string(kZombieUserPromptDelay.count()) +
                               "s; holding the user prompt (device may free itself)");
                }
            }
            zombie_reconnect_delay = heal == ZombieHealLevel::kUserAction ? kZombieGiveUpRetry
                                                                         : kReconnectSettleDelay;
            ScheduleZombieReconnect(session, zombie_reconnect_delay);
            continue;
        }
        if (session->device_class == DeviceClass::kXiaomiRemote2Pro) {
            // 小米无 control_rx 心跳写通道：读电量/GAP 特征强制链路层收发，
            // 成功刷新 last_rx_ms；Unreachable/异常按链路已死拆除。
            ProbeXiaomiSessionAsync(std::move(session));
            continue;
        }
        // 系统配对看门狗（只补不删）：会话健康但 Windows 无系统级 bond 时 HOGP 按键
        // 直通是死的。节流 10min/地址、每次运行最多 3 次；真的缺 bond 时该协程会先
        // 拆掉本会话（设备需重新广播才能 PairAsync），所以这里直接进下一轮。
        if (BeginOsBondCheck(session->bluetooth_address)) {
            RepairOsBondAsync(session, session->device.id);
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
        auto it = sessions_by_device_id_.find(id);
        // 前缀类感知：有会话按 device_class，无会话（未连接）无法判定用通用 VS-。
        line += (it != sessions_by_device_id_.end() && it->second &&
                 it->second->device_class == DeviceClass::kXiaomiRemote2Pro)
                    ? "RC-" + id
                    : "VS-" + id;
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
