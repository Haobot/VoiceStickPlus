#pragma once

#include "byte_utils.h"

#include <optional>
#include <string>
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

struct StateEvent {
    std::string event;
    std::string button;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> duration_ms;
    std::string hardware;
    std::string firmware_version;
};

class BleProtocol {
public:
    static constexpr const wchar_t* service_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100";
    static constexpr const wchar_t* audio_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101";
    static constexpr const wchar_t* state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102";
    static constexpr const wchar_t* control_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103";
    static constexpr const wchar_t* ota_rx_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104";
    static constexpr const wchar_t* ota_state_uuid = L"8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105";

    static std::optional<AudioFrame> ParseAudioFrame(std::span<const std::uint8_t> data);
    static std::optional<StateEvent> ParseStateEvent(std::span<const std::uint8_t> data);
    static ByteVector UiStatePayload(std::string_view state, std::string_view text);
    static std::optional<std::string> DeviceIdFromName(std::string_view name);
    static std::string NormalizeDeviceId(std::string_view text);
};

} // namespace voicestick
