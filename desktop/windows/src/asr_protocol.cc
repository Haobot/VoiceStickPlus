#include "asr_protocol.h"

#include <algorithm>
#include <sstream>
#include <utility>

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

std::string HotwordsCorpusJson(const AppConfig& config) {
    if (config.asr_hotwords.empty()) return {};

    std::ostringstream context;
    context << "{\"hotwords\":[";
    for (std::size_t i = 0; i < config.asr_hotwords.size(); ++i) {
        if (i != 0) context << ",";
        context << "{\"word\":\"" << JsonEscape(config.asr_hotwords[i]) << "\"}";
    }
    context << "]}";

    return ",\"corpus\":{\"context\":\"" + JsonEscape(context.str()) + "\"}";
}

} // namespace

ByteVector AsrProtocol::MakeClientRequestFrame(const AppConfig& config) {
    const std::string payload = SessionPayload(config);
    return MakeBinaryFrame(0x01, 0x00, 0x01, 0x00, ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeAudioFrame(std::span<const std::uint8_t> ogg_data, bool is_last) {
    return MakeBinaryFrame(0x02, is_last ? 0x02 : 0x00, 0x00, 0x00, ogg_data);
}

ByteVector AsrProtocol::MakeStartConnectionFrame(const AppConfig& config) {
    const auto payload = ConnectionPayload(config);
    return MakeEventFrame(0x01, AsrEvent::kStartConnection, {}, 0x01,
                          ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeFinishConnectionFrame(const AppConfig& config) {
    const auto payload = ConnectionPayload(config);
    return MakeEventFrame(0x01, AsrEvent::kFinishConnection, {}, 0x01,
                          ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeStartSessionFrame(const AppConfig& config, std::string_view session_id) {
    const auto payload = SessionPayload(config);
    return MakeEventFrame(0x01, AsrEvent::kStartSession, session_id, 0x01,
                          ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeFinishSessionFrame(const AppConfig& config, std::string_view session_id) {
    const auto payload = ConnectionPayload(config);
    return MakeEventFrame(0x01, AsrEvent::kFinishSession, session_id, 0x01,
                          ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeCancelSessionFrame(const AppConfig& config, std::string_view session_id) {
    const auto payload = ConnectionPayload(config);
    return MakeEventFrame(0x01, AsrEvent::kCancelSession, session_id, 0x01,
                          ByteVector(payload.begin(), payload.end()));
}

ByteVector AsrProtocol::MakeTaskRequestFrame(std::span<const std::uint8_t> ogg_data,
                                             std::string_view session_id) {
    return MakeEventFrame(0x02, AsrEvent::kTaskRequest, session_id, 0x00, ogg_data);
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

std::optional<AsrEventResponse> AsrProtocol::ParseEventResponse(std::span<const std::uint8_t> data) {
    if (data.size() < 4) return std::nullopt;
    const std::uint8_t message_type = data[1] >> 4;
    const std::uint8_t flags = data[1] & 0x0f;
    const std::uint8_t compression = data[2] & 0x0f;
    if (message_type != 0x09 && message_type != 0x0b) return std::nullopt;
    if (flags != 0x04 || compression != 0x00) return std::nullopt;

    std::size_t offset = static_cast<std::size_t>(data[0] & 0x0f) * 4;
    if (data.size() < offset + 4) return std::nullopt;
    const auto event_id = ReadBe32(data.subspan(offset, 4));
    offset += 4;

    std::string session_id;
    if (data.size() >= offset + 4) {
        const auto session_id_size = ReadBe32(data.subspan(offset, 4));
        offset += 4;
        if (data.size() < offset + session_id_size) return std::nullopt;
        session_id = Utf8FromBytes(data.subspan(offset, session_id_size));
        offset += session_id_size;
    }

    std::string payload_text;
    if (data.size() >= offset + 4) {
        const auto payload_size = ReadBe32(data.subspan(offset, 4));
        offset += 4;
        if (data.size() < offset + payload_size) return std::nullopt;
        payload_text = Utf8FromBytes(data.subspan(offset, payload_size));
    }

    AsrEventResponse response;
    response.event_id = event_id;
    switch (event_id) {
    case 1: response.event = AsrEvent::kStartConnection; break;
    case 2: response.event = AsrEvent::kFinishConnection; break;
    case 50: response.event = AsrEvent::kConnectionStarted; break;
    case 51: response.event = AsrEvent::kConnectionFailed; break;
    case 52: response.event = AsrEvent::kConnectionFinished; break;
    case 100: response.event = AsrEvent::kStartSession; break;
    case 101: response.event = AsrEvent::kCancelSession; break;
    case 102: response.event = AsrEvent::kFinishSession; break;
    case 150: response.event = AsrEvent::kSessionStarted; break;
    case 151: response.event = AsrEvent::kSessionCanceled; break;
    case 152: response.event = AsrEvent::kSessionFinished; break;
    case 154: response.event = AsrEvent::kUsageResponse; break;
    case 200: response.event = AsrEvent::kTaskRequest; break;
    case 450: response.event = AsrEvent::kAsrInfo; break;
    case 451: response.event = AsrEvent::kAsrResponse; break;
    case 459: response.event = AsrEvent::kAsrEnd; break;
    default: break;
    }
    response.session_id = std::move(session_id);
    response.payload_text = std::move(payload_text);
    return response;
}

std::string AsrProtocol::ExtractTranscript(std::string_view json_or_text) {
    auto result_text = JsonStringValue(json_or_text, "text");
    if (!result_text.empty()) return result_text;
    return json_or_text.find('{') == std::string_view::npos ? std::string(json_or_text) : std::string();
}

std::string AsrProtocol::SessionPayload(const AppConfig& config) {
    return "{\"user\":{\"uid\":\"voice-stick-local\"},"
           "\"audio\":{\"format\":\"ogg\",\"codec\":\"opus\",\"rate\":16000,\"bits\":16,\"channel\":1},"
           "\"request\":{\"model_name\":\"bigmodel\",\"enable_nonstream\":true,"
           "\"show_utterances\":false,\"result_type\":\"full\",\"enable_ddc\":true,"
           "\"resource_id\":\"" +
           JsonEscape(config.resource_id) + "\"" + HotwordsCorpusJson(config) + "}}";
}

std::string AsrProtocol::ConnectionPayload(const AppConfig& config) {
    return "{\"namespace\":\"BidirectionalASR\",\"event\":0,\"req_params\":" +
           SessionPayload(config) + "}";
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

ByteVector AsrProtocol::MakeEventFrame(std::uint8_t message_type,
                                        AsrEvent event,
                                        std::string_view session_id,
                                        std::uint8_t serialization,
                                        std::span<const std::uint8_t> payload) {
    ByteVector frame;
    frame.push_back(0x11);
    frame.push_back(static_cast<std::uint8_t>((message_type << 4) | 0x04));
    frame.push_back(static_cast<std::uint8_t>(serialization << 4));
    frame.push_back(0x00);
    AppendBe32(frame, static_cast<std::uint32_t>(event));
    if (!session_id.empty()) {
        AppendBe32(frame, static_cast<std::uint32_t>(session_id.size()));
        frame.insert(frame.end(), session_id.begin(), session_id.end());
    }
    AppendBe32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace voicestick
