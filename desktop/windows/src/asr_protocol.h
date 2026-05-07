#pragma once

#include "app_config.h"
#include "byte_utils.h"

#include <optional>
#include <string>

namespace voicestick {

enum class AsrEvent : std::uint32_t {
    kStartConnection = 1,
    kFinishConnection = 2,
    kConnectionStarted = 50,
    kConnectionFailed = 51,
    kConnectionFinished = 52,
    kStartSession = 100,
    kCancelSession = 101,
    kFinishSession = 102,
    kSessionStarted = 150,
    kSessionCanceled = 151,
    kSessionFinished = 152,
    kUsageResponse = 154,
    kTaskRequest = 200,
    kAsrInfo = 450,
    kAsrResponse = 451,
    kAsrEnd = 459,
};

struct AsrResponse {
    bool is_error = false;
    bool is_final = false;
    std::string text;
    std::optional<std::string> upgrade_url;
};

struct AsrEventResponse {
    std::uint32_t event_id = 0;
    std::optional<AsrEvent> event;
    std::string session_id;
    std::string payload_text;
};

class AsrProtocol {
public:
    static ByteVector MakeClientRequestFrame(const AppConfig& config);
    static ByteVector MakeAudioFrame(std::span<const std::uint8_t> ogg_data, bool is_last);
    static ByteVector MakeStartConnectionFrame(const AppConfig& config);
    static ByteVector MakeFinishConnectionFrame(const AppConfig& config);
    static ByteVector MakeStartSessionFrame(const AppConfig& config, std::string_view session_id);
    static ByteVector MakeFinishSessionFrame(const AppConfig& config, std::string_view session_id);
    static ByteVector MakeCancelSessionFrame(const AppConfig& config, std::string_view session_id);
    static ByteVector MakeTaskRequestFrame(std::span<const std::uint8_t> ogg_data,
                                           std::string_view session_id);
    static std::optional<AsrResponse> ParseResponse(std::span<const std::uint8_t> data);
    static std::optional<AsrEventResponse> ParseEventResponse(std::span<const std::uint8_t> data);
    static std::string ExtractTranscript(std::string_view json_or_text);

private:
    static std::string SessionPayload(const AppConfig& config);
    static std::string ConnectionPayload(const AppConfig& config);
    static ByteVector MakeBinaryFrame(std::uint8_t message_type,
                                        std::uint8_t flags,
                                        std::uint8_t serialization,
                                        std::uint8_t compression,
                                        std::span<const std::uint8_t> payload);
    static ByteVector MakeEventFrame(std::uint8_t message_type,
                                      AsrEvent event,
                                      std::string_view session_id,
                                      std::uint8_t serialization,
                                      std::span<const std::uint8_t> payload);
};

} // namespace voicestick
