#include "asr_protocol.h"

#include <algorithm>

namespace voicestick {

namespace {

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string JsonStringValue(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return {};
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return {};
    auto first_quote = json.find('"', colon + 1);
    if (first_quote == std::string_view::npos) return {};
    std::string out;
    bool escaped = false;
    for (auto i = first_quote + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (escaped) {
            out.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    return {};
}

} // namespace

ByteVector AsrProtocol::MakeClientRequestFrame(const AppConfig& config) {
    const std::string payload =
        "{\"user\":{\"uid\":\"voice-stick-local\"},"
        "\"audio\":{\"format\":\"ogg\",\"codec\":\"opus\",\"rate\":16000,\"bits\":16,\"channel\":1},"
        "\"request\":{\"model_name\":\"bigmodel\",\"enable_nonstream\":true,"
        "\"show_utterances\":false,\"enable_ddc\":true,\"resource_id\":\"" +
        JsonEscape(config.resource_id) + "\"}}";
    return MakeBinaryFrame(0x01, 0x00, 0x01, 0x00, ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeAudioFrame(std::span<const std::uint8_t> ogg_data, bool is_last) {
    return MakeBinaryFrame(0x02, is_last ? 0x02 : 0x00, 0x00, 0x00, ogg_data);
}

std::optional<AsrResponse> AsrProtocol::ParseResponse(std::span<const std::uint8_t> data) {
    if (data.size() < 4) return std::nullopt;
    const std::uint8_t message_type = data[1] >> 4;
    const std::uint8_t flags = data[1] & 0x0f;
    std::size_t offset = static_cast<std::size_t>(data[0] & 0x0f) * 4;
    if (offset > data.size()) return std::nullopt;

    if (message_type == 0x09) {
        if (flags == 0x01 || flags == 0x03) {
            if (data.size() < offset + 4) return std::nullopt;
            offset += 4;
        }
        if (data.size() < offset + 4) return std::nullopt;
        const auto payload_size = ReadBe32(data.subspan(offset, 4));
        offset += 4;
        if (data.size() < offset + payload_size) return std::nullopt;
        const auto body = Utf8FromBytes(data.subspan(offset, payload_size));
        return AsrResponse{false, flags == 0x03, ExtractTranscript(body), std::nullopt};
    }

    if (message_type == 0x0f) {
        if (data.size() < offset + 8) return std::nullopt;
        const auto code = ReadBe32(data.subspan(offset, 4));
        offset += 4;
        const auto message_size = ReadBe32(data.subspan(offset, 4));
        offset += 4;
        if (data.size() < offset + message_size) return std::nullopt;
        const auto message = Utf8FromBytes(data.subspan(offset, message_size));
        const auto detail = JsonStringValue(message, "message");
        const auto error = JsonStringValue(message, "error");
        const auto upgrade_url = JsonStringValue(message, "upgrade_url");
        AsrResponse response;
        response.is_error = true;
        response.text = "ASR " + std::to_string(code) + ": " + (!detail.empty() ? detail : (!error.empty() ? error : message));
        if (!upgrade_url.empty()) response.upgrade_url = upgrade_url;
        return response;
    }

    return std::nullopt;
}

std::string AsrProtocol::ExtractTranscript(std::string_view json_or_text) {
    auto result_text = JsonStringValue(json_or_text, "text");
    if (!result_text.empty()) return result_text;
    return json_or_text.find('{') == std::string_view::npos ? std::string(json_or_text) : std::string();
}

ByteVector AsrProtocol::MakeBinaryFrame(std::uint8_t message_type,
                                          std::uint8_t flags,
                                          std::uint8_t serialization,
                                          std::uint8_t compression,
                                          std::span<const std::uint8_t> payload) {
    ByteVector frame;
    frame.push_back(0x11);
    frame.push_back(static_cast<std::uint8_t>((message_type << 4) | flags));
    frame.push_back(static_cast<std::uint8_t>((serialization << 4) | compression));
    frame.push_back(0x00);
    AppendBe32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace voicestick
