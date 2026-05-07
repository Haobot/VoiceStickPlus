#pragma once

#include "app_config.h"
#include "byte_utils.h"

#include <optional>
#include <string>

namespace voicestick {

struct AsrResponse {
    bool is_error = false;
    bool is_final = false;
    std::string text;
    std::optional<std::string> upgrade_url;
};

class AsrProtocol {
public:
    static ByteVector MakeClientRequestFrame(const AppConfig& config);
    static ByteVector MakeAudioFrame(std::span<const std::uint8_t> ogg_data, bool is_last);
    static std::optional<AsrResponse> ParseResponse(std::span<const std::uint8_t> data);
    static std::string ExtractTranscript(std::string_view json_or_text);

private:
    static ByteVector MakeBinaryFrame(std::uint8_t message_type,
                                        std::uint8_t flags,
                                        std::uint8_t serialization,
                                        std::uint8_t compression,
                                        std::span<const std::uint8_t> payload);
};

} // namespace voicestick
