#include "ble_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace voicestick {

namespace {

std::string TrimCopy(std::string_view text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::string Uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool IsHex4(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

// 遍历广播 AD 结构，在 128-bit service UUID 列表（type 0x06 incomplete / 0x07
// complete）中匹配目标 UUID（uuid 为线上小端字节序，16 字节）。
bool ContainsServiceUuid128(std::span<const std::uint8_t> data,
                            std::span<const std::uint8_t, 16> uuid) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto length = data[offset];
        if (length == 0) break;
        if (offset + 1u + length > data.size()) return false;
        const auto type = data[offset + 1];
        if ((type == 0x06 || type == 0x07) && (length - 1) % uuid.size() == 0) {
            for (std::size_t uuid_offset = offset + 2; uuid_offset + uuid.size() <= offset + 1u + length;
                 uuid_offset += uuid.size()) {
                if (std::equal(uuid.begin(), uuid.end(), data.begin() + static_cast<std::ptrdiff_t>(uuid_offset))) {
                    return true;
                }
            }
        }
        offset += 1u + length;
    }
    return false;
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
            switch (ch) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(ch); break;
            }
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

std::optional<std::uint32_t> JsonU32Value(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    auto end = begin;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (begin == end) return std::nullopt;
    std::uint32_t value = 0;
    auto result = std::from_chars(json.data() + begin, json.data() + end, value);
    if (result.ec != std::errc()) return std::nullopt;
    return value;
}

std::optional<int> JsonIntValue(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    auto end = begin;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) ++end;
    if (begin == end) return std::nullopt;
    int value = 0;
    auto result = std::from_chars(json.data() + begin, json.data() + end, value);
    if (result.ec != std::errc()) return std::nullopt;
    return value;
}

std::optional<bool> JsonBoolValue(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    if (json.substr(begin, 4) == "true") return true;
    if (json.substr(begin, 5) == "false") return false;
    return std::nullopt;
}

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

} // namespace

std::optional<AudioFrame> BleProtocol::ParseAudioFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 16 || data[0] != 1 || data[1] != 0x01) return std::nullopt;
    const auto header_len = ReadLe16(data.subspan(2, 2));
    if (header_len != 16 || data.size() < header_len) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(14, 2));
    if (data.size() < 16u + payload_len) return std::nullopt;
    AudioFrame frame;
    frame.session_id = ReadLe32(data.subspan(4, 4));
    frame.seq = ReadLe32(data.subspan(8, 4));
    frame.flags = data[12];
    frame.payload.assign(data.begin() + 16, data.begin() + 16 + payload_len);
    return frame;
}

std::optional<StateEvent> BleProtocol::ParseStateEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != state_type_json) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    StateEvent event;
    event.event = JsonStringValue(json, "event");
    // power_mgmt（供电态自动关机开关）由 ParsePowerMgmtEvent 消费，此处跳过，
    // 否则会以通用 StateEvent 返回并截走分发链（ble_central 的 fallback 顺序）。
    if (event.event.empty() || event.event == "power_mgmt") return std::nullopt;
    event.button = JsonStringValue(json, "button");
    event.session_id = JsonU32Value(json, "session_id");
    event.duration_ms = JsonU32Value(json, "duration_ms");
    event.hardware = JsonStringValue(json, "hardware");
    event.firmware_version = JsonStringValue(json, "firmware_version");
    // encoder_status 事件：{"event":"encoder_status","present":true/false}。
    // 独立小帧上报（device_info 已接近 BLE 通知 MTU 上限，不宜再扩字段）；
    // 老固件不发送此事件，消费端按「在线」处理保持编码器设置可见（向后兼容）。
    if (event.event == "encoder_status") {
        event.encoder_present = JsonBoolValue(json, "present");
    }
    event.battery_level = JsonIntValue(json, "level");
    event.battery_charging = JsonBoolValue(json, "charging");
    event.battery_usb_powered = JsonBoolValue(json, "usb_powered");
    event.direction = JsonStringValue(json, "direction");
    event.steps = JsonU32Value(json, "steps");
    event.source = JsonStringValue(json, "source");

    return event;
}

std::optional<MotionEvent> BleProtocol::ParseMotionFrame(std::span<const std::uint8_t> data) {
    // 固定 6 字节：version(1) + type(0x11) + int16 dx + int16 dy，小端。
    if (data.size() < 6 || data[0] != 1 || data[1] != state_type_motion) return std::nullopt;
    MotionEvent event;
    event.dx = static_cast<std::int16_t>(ReadLe16(data.subspan(2, 2)));
    event.dy = static_cast<std::int16_t>(ReadLe16(data.subspan(4, 2)));
    return event;
}

int Base64DigitValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

bool Base64Decode(std::string_view text, ByteVector* out) {
    out->clear();
    if (text.size() % 4 != 0 || text.empty()) return false;
    out->reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int d0 = Base64DigitValue(text[i]);
        const int d1 = Base64DigitValue(text[i + 1]);
        if (d0 < 0 || d1 < 0) return false;
        const bool pad2 = text[i + 2] == '=';
        const bool pad3 = text[i + 3] == '=';
        const int d2 = pad2 ? 0 : Base64DigitValue(text[i + 2]);
        const int d3 = pad3 ? 0 : Base64DigitValue(text[i + 3]);
        if (d2 < 0 || d3 < 0) return false;
        if (pad3 && !pad2) return false;  // 非法 padding
        const std::uint32_t n = (static_cast<std::uint32_t>(d0) << 18) |
                                (static_cast<std::uint32_t>(d1) << 12) |
                                (static_cast<std::uint32_t>(d2) << 6) |
                                static_cast<std::uint32_t>(d3);
        out->push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        if (!pad2) out->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        if (!pad3) out->push_back(static_cast<std::uint8_t>(n & 0xFF));
    }
    return true;
}

std::optional<PowerLogFragment> BleProtocol::ParsePowerLogFragment(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != state_type_json) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    // 分片帧形如 {"power_log":{"seq":0,"offset":16,"total":1234,"eof":0,"data":"<base64>"}}。
    if (json.find("\"power_log\"") == std::string::npos) return std::nullopt;
    PowerLogFragment fragment;
    fragment.seq = JsonU32Value(json, "seq").value_or(0);
    fragment.offset = JsonU32Value(json, "offset").value_or(0);
    fragment.total = JsonU32Value(json, "total").value_or(0);
    fragment.eof = JsonIntValue(json, "eof").value_or(0) != 0;
    const auto b64 = JsonStringValue(json, "data");
    if (!b64.empty() && !Base64Decode(b64, &fragment.data)) return std::nullopt;
    return fragment;
}

std::optional<bool> BleProtocol::ParsePowerMgmtEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != state_type_json) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    if (json.find("\"power_mgmt\"") == std::string::npos) return std::nullopt;
    return JsonBoolValue(json, "usb_auto_off");
}

std::optional<FirmwareOtaStateEvent> BleProtocol::ParseFirmwareOtaStateEvent(std::span<const std::uint8_t> data) {
    if (data.size() < 4 || data[0] != 1 || data[1] != ota_type_state) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    FirmwareOtaStateEvent event;
    event.event = JsonStringValue(json, "event");
    if (event.event.empty()) return std::nullopt;
    event.transfer_id = JsonU32Value(json, "transfer_id");
    event.written = JsonU32Value(json, "written");
    event.size = JsonU32Value(json, "size");
    event.code = JsonStringValue(json, "code");
    event.reboot_ms = JsonU32Value(json, "reboot_ms");
    return event;
}

ByteVector BleProtocol::UiStatePayload(std::string_view state, std::string_view text) {
    const auto json = std::string("{\"event\":\"ui_state\",\"state\":\"") +
                      JsonEscape(state) + "\",\"text\":\"" + JsonEscape(text) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::InteractionModePayload(std::string_view mode) {
    const auto json = std::string("{\"event\":\"interaction_mode\",\"mode\":\"") +
                      JsonEscape(mode) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::ShowImuDebugPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"show_imu_debug\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::TapEnabledPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"tap_enabled\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::EncoderLedColorPayload(std::string_view color) {
    const auto json = std::string("{\"event\":\"encoder_led_color\",\"color\":\"") +
                      JsonEscape(color) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::EncoderRecordingGatePayload(bool enabled) {
    const auto json = std::string("{\"event\":\"encoder_recording_gate\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::TapSensitivityPayload(int level) {
    const auto json = std::string("{\"event\":\"tap_sensitivity\",\"level\":") +
                      std::to_string(level) + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::AirMouseEnabledPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"air_mouse_enabled\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::ImuWakeSensitivityPayload(int threshold_lsb) {
    const auto json = std::string("{\"event\":\"imu_wake_sensitivity\",\"threshold\":") +
                      std::to_string(threshold_lsb) + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::BatteryStatusRequestPayload() {
    const std::string json = "{\"event\":\"battery_status_request\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::PowerLogDumpPayload(std::uint32_t offset, std::uint32_t max) {
    const std::string json = "{\"power_log\":{\"cmd\":\"dump\",\"offset\":" +
                              std::to_string(offset) + ",\"max\":" +
                              std::to_string(max) + "}}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::PowerLogTimeAnchorPayload(std::uint32_t epoch) {
    const std::string json = "{\"power_log\":{\"cmd\":\"time_anchor\",\"epoch\":" +
                              std::to_string(epoch) + "}}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::PowerLogClearPayload() {
    const std::string json = "{\"power_log\":{\"cmd\":\"clear\"}}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::UsbAutoOffPayload(bool enabled) {
    const std::string json = std::string("{\"event\":\"usb_auto_off\",\"enabled\":") +
                              (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::UsbAutoOffGetPayload() {
    const std::string json = "{\"event\":\"usb_auto_off_get\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::RemoteButtonPayload(std::string_view action,
                                            std::string_view button,
                                            std::string_view source,
                                            std::uint32_t request_id) {
    const auto json = std::string("{\"event\":\"remote_button_") +
                      std::string(action) +
                      "\",\"button\":\"" + JsonEscape(button) +
                      "\",\"source\":\"" + JsonEscape(source) +
                      "\",\"request_id\":" + std::to_string(request_id) + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::OtaBeginPayload(std::uint32_t image_size, std::uint32_t transfer_id) {
    ByteVector data = {1, ota_type_begin, 12, 0};
    AppendLe32(data, image_size);
    AppendLe32(data, transfer_id);
    return data;
}

ByteVector BleProtocol::OtaDataPayload(std::uint32_t transfer_id,
                                       std::uint32_t offset,
                                       std::span<const std::uint8_t> chunk) {
    ByteVector data = {1, ota_type_data, 12, 0};
    AppendLe32(data, transfer_id);
    AppendLe32(data, offset);
    data.insert(data.end(), chunk.begin(), chunk.end());
    return data;
}

ByteVector BleProtocol::OtaEndPayload(std::uint32_t transfer_id, std::uint32_t image_size) {
    ByteVector data = {1, ota_type_end, 12, 0};
    AppendLe32(data, transfer_id);
    AppendLe32(data, image_size);
    return data;
}

ByteVector BleProtocol::OtaAbortPayload(std::uint32_t transfer_id) {
    ByteVector data = {1, ota_type_abort, 8, 0};
    AppendLe32(data, transfer_id);
    return data;
}

std::optional<std::string> BleProtocol::DeviceIdFromName(std::string_view name) {
    auto value = Uppercase(TrimCopy(name));
    std::string_view prefix;
    if (value.starts_with("VS-")) {
        prefix = "VS-";
    } else if (value.starts_with("RC-")) {
        prefix = "RC-";
    } else {
        return std::nullopt;
    }
    value = value.substr(prefix.size(), 4);
    if (!IsHex4(value)) return std::nullopt;
    return value;
}

std::optional<std::string> BleProtocol::LocalNameFromAdvertisementData(std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto length = data[offset];
        if (length == 0) break;
        if (offset + 1u + length > data.size()) return std::nullopt;
        const auto type = data[offset + 1];
        if (type == 0x08 || type == 0x09) {
            const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset + 2);
            return std::string(begin, begin + static_cast<std::ptrdiff_t>(length - 1));
        }
        offset += 1u + length;
    }
    return std::nullopt;
}

bool BleProtocol::HasVoiceStickServiceUuid(std::span<const std::uint8_t> data) {
    constexpr std::uint8_t uuid[] = {
        0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
        0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f,
    };
    return ContainsServiceUuid128(data, uuid);
}

bool BleProtocol::HasXiaomiAtvvServiceUuid(std::span<const std::uint8_t> data) {
    // AB5E0001-5A21-4F05-BC7D-AF01F617B664 的线上小端字节序（与广播 AD 结构一致）。
    constexpr std::uint8_t uuid[] = {
        0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
        0x05, 0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab,
    };
    return ContainsServiceUuid128(data, uuid);
}

std::string BleProtocol::DeviceIdFromBluetoothAddress(std::uint64_t bluetooth_address) {
    char buffer[5]{};
    snprintf(buffer, sizeof(buffer), "%04X", static_cast<unsigned int>(bluetooth_address & 0xffff));
    return buffer;
}

std::string BleProtocol::NormalizeDeviceId(std::string_view text) {
    auto value = Uppercase(TrimCopy(text));
    if (value.starts_with("VS-") || value.starts_with("RC-")) value = value.substr(3);
    value = value.substr(0, std::min<std::size_t>(4, value.size()));
    return IsHex4(value) ? value : std::string();
}

bool BleProtocol::IsXiaomiRemoteName(std::string_view name) {
    auto value = TrimCopy(name);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // 中文名无大小写，tolower 只影响 ASCII，UTF-8 字节序列保持原样比较。
    return value == "mi rc" || value == "xiaomi bluetooth remote 2 pro" ||
           value == "小米蓝牙语音遥控器" || value == "rc001" || value == "rc003";
}

std::optional<DeviceClass> BleProtocol::DeviceClassFromName(std::string_view name) {
    if (IsXiaomiRemoteName(name)) return DeviceClass::kXiaomiRemote2Pro;
    const auto value = Uppercase(TrimCopy(name));
    if (value.starts_with("RC-") && IsHex4(value.substr(3, 4))) {
        return DeviceClass::kXiaomiRemote2Pro;
    }
    if (value.starts_with("VS-") && IsHex4(value.substr(3, 4))) return DeviceClass::kStickS3;
    return std::nullopt;
}

} // namespace voicestick
