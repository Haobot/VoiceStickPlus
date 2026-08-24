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

// 体感鼠标运动帧：固件已完成零偏校准+死区+缩放的整型光标位移。
struct MotionEvent {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
};

struct StateEvent {
    std::string event;
    std::string button;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> duration_ms;
    std::string hardware;
    std::string firmware_version;
    // MiniEncoderC 编码器在线标志：仅 encoder_status 事件携带；老固件不发送该事件，
    // 消费端按「在线」处理以保持编码器设置可见（向后兼容）。
    std::optional<bool> encoder_present;
    std::optional<int> battery_level;
    std::optional<bool> battery_charging;
    std::optional<bool> battery_usb_powered;
    // 编码器旋转事件字段：direction 为 "cw"/"ccw"（固件上报的原始物理方向，
    // 语义映射在桌面端完成）；steps 为该帧内同向累计格数（>=1）。非旋转事件为空。
    std::string direction;
    std::optional<std::uint32_t> steps;
    // 事件来源标签：编码器按钮事件为 "encoder"；物理键/远程键省略该字段（空串）。
    std::string source;
};

struct FirmwareOtaStateEvent {
    std::string event;
    std::optional<std::uint32_t> transfer_id;
    std::optional<std::uint32_t> written;
    std::optional<std::uint32_t> size;
    std::string code;
    std::optional<std::uint32_t> reboot_ms;
};

// power_log 导出分片（state_tx JSON 帧 {"power_log":{...}}，协议见
// Doc/Ref/protocol.md「Power Log Export」）。data 为 base64 解码后的原始字节。
struct PowerLogFragment {
    std::uint32_t seq = 0;
    std::uint32_t offset = 0;
    std::uint32_t total = 0;
    bool eof = false;
    ByteVector data;
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

    static constexpr std::uint8_t state_type_json = 0x10;
    static constexpr std::uint8_t state_type_motion = 0x11;

    static std::optional<AudioFrame> ParseAudioFrame(std::span<const std::uint8_t> data);
    static std::optional<StateEvent> ParseStateEvent(std::span<const std::uint8_t> data);
    static std::optional<MotionEvent> ParseMotionFrame(std::span<const std::uint8_t> data);
    // power_log 分片帧（type==0x10，payload 含 "power_log" 键；ParseStateEvent 对
    // 其返回 nullopt，因无 "event" 字段）。
    static std::optional<PowerLogFragment> ParsePowerLogFragment(std::span<const std::uint8_t> data);
    static std::optional<FirmwareOtaStateEvent> ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data);
    static ByteVector UiStatePayload(std::string_view state, std::string_view text);
    static ByteVector InteractionModePayload(std::string_view mode);
    static ByteVector ShowImuDebugPayload(bool enabled);
    static ByteVector ImuWakeSensitivityPayload(int threshold_lsb);
    static ByteVector TapEnabledPayload(bool enabled);
    static ByteVector EncoderLedColorPayload(std::string_view color);
    static ByteVector EncoderRecordingGatePayload(bool enabled);
    static ByteVector TapSensitivityPayload(int level);
    static ByteVector AirMouseEnabledPayload(bool enabled);
    static ByteVector BatteryStatusRequestPayload();
    // power_log 命令族（电池电压监测，见 Doc/Ref/protocol.md）：
    // dump 从 offset 起启动一次性流式导出（max 为单片原始字节上限，≤160）；
    // time_anchor 写入墙钟锚点条目。
    static ByteVector PowerLogDumpPayload(std::uint32_t offset, std::uint32_t max);
    static ByteVector PowerLogTimeAnchorPayload(std::uint32_t epoch);
    static ByteVector RemoteButtonPayload(std::string_view action,
                                          std::string_view button,
                                          std::string_view source,
                                          std::uint32_t request_id);
    static ByteVector OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id);
    static ByteVector OtaDataPayload(std::uint32_t transfer_id, std::uint32_t offset, std::span<const std::uint8_t> chunk);
    static ByteVector OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size);
    static ByteVector OtaAbortPayload(std::uint32_t transfer_id);
    static std::optional<std::string> DeviceIdFromName(std::string_view name);
    static std::optional<std::string> LocalNameFromAdvertisementData(std::span<const std::uint8_t> data);
    static bool HasVoiceStickServiceUuid(std::span<const std::uint8_t> data);
    static std::string DeviceIdFromBluetoothAddress(std::uint64_t bluetooth_address);
    static std::string NormalizeDeviceId(std::string_view text);
};

} // namespace voicestick
