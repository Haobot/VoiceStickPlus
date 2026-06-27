#pragma once

#include "byte_utils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

struct AudioFrame {
    std::uint32_t session_id = 0;
    std::uint32_t seq = 0;
    std::uint8_t flags = 0;
    ByteVector payload;

    bool IsStart() const { return (flags & 0x01) != 0; }
    bool IsEnd() const { return (flags & 0x02) != 0; }
};

struct WifiStatusSnapshot {
    // 与 Doc/Ref/protocol.md §wifi 协议对齐：固件下发的完整快照，不做差分。
    std::string state;                                 // disabled|configured|connecting|connected|disconnected|error
    std::string ssid;
    std::string ip;
    std::optional<int> rssi;
    std::string last_error;                            // 错误码枚举见协议表；空字符串代表无错
    std::string ota_pull_state;                        // idle|downloading|finishing|success|failed
    std::optional<int> ota_pull_progress_pct;          // 0..100
    std::string ota_pull_url;
    std::string ota_pull_last_error;
    bool ota_pending_verify = false;                   // 新固件首次启动等待业务侧 mark_valid
    std::string running_partition;                     // ota_0|ota_1，用于 OTA 后确认当前槽位
    bool park_locked = true;                           // 录音空闲且未 OTA 时为 true
};

// 固件 wifi_scan_result 中单个 AP 信息
struct WifiApInfo {
    std::string ssid;
    int rssi = 0;
    int auth = 0;  // 0=OPEN, 1=WEP, 2=WPA_PSK, 3=WPA2_PSK, 4=WPA_WPA2_PSK, ...
};

// 固件 wifi_scan_result 事件载荷
struct WifiScanResult {
    std::vector<WifiApInfo> aps;
};

struct StateEvent {
    std::string event;
    std::string button;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> duration_ms;
    std::string hardware;
    std::string firmware_version;
    std::optional<int> battery_level;
    std::optional<bool> battery_charging;
    std::optional<bool> battery_usb_powered;
    std::optional<WifiStatusSnapshot> wifi;            // 仅 event=="wifi_status" 时有值
    std::optional<WifiScanResult> wifi_scan;           // 仅 event=="wifi_scan_result" 时有值
};

struct FirmwareOtaStateEvent {
    std::string event;
    std::optional<std::uint32_t> transfer_id;
    std::optional<std::uint32_t> written;
    std::optional<std::uint32_t> size;
    std::string code;
    std::optional<std::uint32_t> reboot_ms;
};

class BleProtocol {
public:
    static constexpr const wchar_t* service_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100";
    static constexpr const wchar_t* audio_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101";
    static constexpr const wchar_t* state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102";
    static constexpr const wchar_t* control_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103";
    static constexpr const wchar_t* ota_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104";
    static constexpr const wchar_t* ota_state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105";
    static constexpr std::uint8_t ota_type_begin = 0x20;
    static constexpr std::uint8_t ota_type_data = 0x21;
    static constexpr std::uint8_t ota_type_end = 0x22;
    static constexpr std::uint8_t ota_type_abort = 0x23;
    static constexpr std::uint8_t ota_type_state = 0x30;

    static std::optional<AudioFrame> ParseAudioFrame(std::span<const std::uint8_t> data);
    static std::optional<StateEvent> ParseStateEvent(std::span<const std::uint8_t> data);
    static std::optional<FirmwareOtaStateEvent> ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data);
    static ByteVector UiStatePayload(std::string_view state, std::string_view text);
    static ByteVector InteractionModePayload(std::string_view mode);
    static ByteVector PromptTonePayload(bool enabled);
    static ByteVector ShowImuDebugPayload(bool enabled);
    static ByteVector ShowWifiInfoPayload(bool enabled);
    static ByteVector ImuWakeSensitivityPayload(int threshold_lsb);
    static ByteVector BatteryStatusRequestPayload();
    static ByteVector RemoteButtonPayload(std::string_view action,
                                          std::string_view button,
                                          std::string_view source,
                                          std::uint32_t request_id);
    static ByteVector OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id);
    static ByteVector OtaDataPayload(std::uint32_t transfer_id, std::uint32_t offset, std::span<const std::uint8_t> chunk);
    static ByteVector OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size);
    static ByteVector OtaAbortPayload(std::uint32_t transfer_id);
    // Wi-Fi STA 配置 + HTTPS pull OTA（详见 Doc/Plan/wifi-sta-ble-provisioning.md §3.1）
    static ByteVector WifiSetPayload(std::string_view ssid, std::string_view password);
    static ByteVector WifiClearPayload();
    static ByteVector WifiStatusRequestPayload();
    static ByteVector WifiScanPayload();
    static ByteVector OtaPullPayload(std::string_view url, std::string_view sha256_hex);
    static ByteVector OtaCommitPayload();
    static std::optional<std::string> DeviceIdFromName(std::string_view name);
    static std::optional<std::string> LocalNameFromAdvertisementData(std::span<const std::uint8_t> data);
    static bool HasVoiceStickServiceUuid(std::span<const std::uint8_t> data);
    static std::string DeviceIdFromBluetoothAddress(std::uint64_t bluetooth_address);
    static std::string NormalizeDeviceId(std::string_view text);
};

} // namespace voicestick
