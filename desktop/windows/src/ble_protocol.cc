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

// 提取 JSON 顶层 key 对应的对象字面量子串（含两侧 {}），不做嵌套校验外的容错。
// 用于把 wifi_status 的 "ota_pull": {...} 子对象切出来再独立查字段，避免与外层同名字段
// （state、last_error）冲突。
std::string_view JsonObjectSlice(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return {};
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return {};
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    if (begin >= json.size() || json[begin] != '{') return {};
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (auto i = begin; i < json.size(); ++i) {
        char ch = json[i];
        if (in_string) {
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') { in_string = false; }
            continue;
        }
        if (ch == '"') { in_string = true; continue; }
        if (ch == '{') ++depth;
        else if (ch == '}') {
            --depth;
            if (depth == 0) return json.substr(begin, i - begin + 1);
        }
    }
    return {};
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

// 解析 JSON 顶层 key 对应的数组，返回每个数组元素的子串（{...} 对象或原始值）。
// 用于 wifi_scan_result 的 "aps":[{...},{...}] 等字段。
std::vector<std::string_view> JsonArrayItems(std::string_view json, std::string_view key) {
    std::vector<std::string_view> items;
    const std::string needle = "\"" + std::string(key) + "\"";
    auto key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) return items;
    auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return items;
    auto begin = colon + 1;
    while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    if (begin >= json.size() || json[begin] != '[') return items;

    // 遍历数组，切分每个顶层元素
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    auto elem_start = begin + 1;
    for (auto i = begin + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (in_string) {
            if (escaped) { escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') { in_string = false; }
            continue;
        }
        if (ch == '"') { in_string = true; continue; }
        if (ch == '{' || ch == '[') { ++depth; continue; }
        if (ch == '}' || ch == ']') {
            if (ch == ']' && depth == 0) {
                // 数组结束，处理最后一个元素（如果有）
                if (elem_start < i) {
                    items.push_back(json.substr(elem_start, i - elem_start));
                }
                return items;
            }
            --depth;
            continue;
        }
        if (ch == ',' && depth == 0) {
            items.push_back(json.substr(elem_start, i - elem_start));
            elem_start = i + 1;
        }
    }
    return items;
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
    if (data.size() < 4 || data[0] != 1 || data[1] != 0x10) return std::nullopt;
    const auto payload_len = ReadLe16(data.subspan(2, 2));
    if (data.size() < 4u + payload_len) return std::nullopt;
    const auto json = Utf8FromBytes(data.subspan(4, payload_len));
    StateEvent event;
    event.event = JsonStringValue(json, "event");
    if (event.event.empty()) return std::nullopt;
    event.button = JsonStringValue(json, "button");
    event.session_id = JsonU32Value(json, "session_id");
    event.duration_ms = JsonU32Value(json, "duration_ms");
    event.hardware = JsonStringValue(json, "hardware");
    event.firmware_version = JsonStringValue(json, "firmware_version");
    event.battery_level = JsonIntValue(json, "level");
    event.battery_charging = JsonBoolValue(json, "charging");
    event.battery_usb_powered = JsonBoolValue(json, "usb_powered");

    if (event.event == "wifi_status") {
        WifiStatusSnapshot wifi;
        // 顶层字段直接从 JSON 提取，但 ota_pull 子对象与外层有同名字段（state/last_error），
        // 必须先把子对象切出来再独立查，避免误命中外层。
        const auto ota_slice = JsonObjectSlice(json, "ota_pull");
        if (!ota_slice.empty()) {
            wifi.ota_pull_state = JsonStringValue(ota_slice, "state");
            wifi.ota_pull_progress_pct = JsonIntValue(ota_slice, "progress_pct");
            wifi.ota_pull_url = JsonStringValue(ota_slice, "url");
            wifi.ota_pull_last_error = JsonStringValue(ota_slice, "last_error");
        }
        // 切掉 ota_pull 子对象后查询外层字段，确保 state / last_error 取的是顶层值。
        std::string outer(json);
        if (!ota_slice.empty()) {
            // 把 ota_pull 整段（连同 key 和 :）从外层字符串里抹掉。
            const auto key_pos = outer.find("\"ota_pull\"");
            if (key_pos != std::string::npos) {
                const auto end_pos = static_cast<std::size_t>(ota_slice.data() - json.data()) + ota_slice.size();
                outer.erase(key_pos, end_pos - key_pos);
            }
        }
        wifi.state = JsonStringValue(outer, "state");
        wifi.ssid = JsonStringValue(outer, "ssid");
        wifi.ip = JsonStringValue(outer, "ip");
        wifi.rssi = JsonIntValue(outer, "rssi");
        wifi.last_error = JsonStringValue(outer, "last_error");
        auto pending = JsonBoolValue(outer, "pending");
        if (pending.has_value()) wifi.ota_pending_verify = *pending;
        wifi.running_partition = JsonStringValue(outer, "partition");
        auto parked = JsonBoolValue(outer, "park");
        if (parked.has_value()) wifi.park_locked = *parked;
        event.wifi = std::move(wifi);
    }

    if (event.event == "wifi_scan_result") {
        WifiScanResult scan;
        const auto aps = JsonArrayItems(json, "aps");
        for (const auto& ap : aps) {
            WifiApInfo info;
            info.ssid = JsonStringValue(ap, "ssid");
            info.rssi = JsonIntValue(ap, "rssi").value_or(0);
            info.auth = JsonIntValue(ap, "auth").value_or(0);
            if (!info.ssid.empty()) scan.aps.push_back(std::move(info));
        }
        event.wifi_scan = std::move(scan);
    }

    return event;
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

ByteVector BleProtocol::PromptTonePayload(bool enabled) {
    const auto json = std::string("{\"event\":\"prompt_tone\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::ShowImuDebugPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"show_imu_debug\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::ShowWifiInfoPayload(bool enabled) {
    const auto json = std::string("{\"event\":\"show_wifi_info\",\"enabled\":") +
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

ByteVector BleProtocol::WifiSetPayload(std::string_view ssid, std::string_view password) {
    // 与 Doc/Plan §3.1 对齐：密码为空表示开放网络，仍带 password 字段保持解析端不分支。
    const auto json = std::string("{\"event\":\"wifi_set\",\"ssid\":\"") +
                      JsonEscape(ssid) + "\",\"password\":\"" + JsonEscape(password) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::WifiClearPayload() {
    const std::string json = "{\"event\":\"wifi_clear\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::WifiStatusRequestPayload() {
    const std::string json = "{\"event\":\"wifi_status_request\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::WifiScanPayload() {
    const std::string json = "{\"event\":\"wifi_scan\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::OtaPullPayload(std::string_view url, std::string_view sha256_hex) {
    // sha256_hex 可选：留空时不带字段，固件侧据此走"无校验"分支；URL 必须由调用方校验为 HTTPS。
    std::string json = std::string("{\"event\":\"ota_pull\",\"url\":\"") +
                       JsonEscape(url) + "\"";
    if (!sha256_hex.empty()) {
        json += std::string(",\"sha256_hex\":\"") + JsonEscape(sha256_hex) + "\"";
    }
    json += "}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::OtaCommitPayload() {
    const std::string json = "{\"event\":\"ota_commit\"}";
    return ByteVector(json.begin(), json.end());
}

std::optional<std::string> BleProtocol::DeviceIdFromName(std::string_view name) {
    auto value = Uppercase(TrimCopy(name));
    if (!value.starts_with("VS-")) return std::nullopt;
    value = value.substr(3, 4);
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
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto length = data[offset];
        if (length == 0) break;
        if (offset + 1u + length > data.size()) return false;
        const auto type = data[offset + 1];
        if ((type == 0x06 || type == 0x07) && (length - 1) % sizeof(uuid) == 0) {
            for (std::size_t uuid_offset = offset + 2; uuid_offset + sizeof(uuid) <= offset + 1u + length;
                 uuid_offset += sizeof(uuid)) {
                if (std::equal(std::begin(uuid), std::end(uuid), data.begin() + static_cast<std::ptrdiff_t>(uuid_offset))) {
                    return true;
                }
            }
        }
        offset += 1u + length;
    }
    return false;
}

std::string BleProtocol::DeviceIdFromBluetoothAddress(std::uint64_t bluetooth_address) {
    char buffer[5]{};
    snprintf(buffer, sizeof(buffer), "%04X", static_cast<unsigned int>(bluetooth_address & 0xffff));
    return buffer;
}

std::string BleProtocol::NormalizeDeviceId(std::string_view text) {
    auto value = Uppercase(TrimCopy(text));
    if (value.starts_with("VS-")) value = value.substr(3);
    value = value.substr(0, std::min<std::size_t>(4, value.size()));
    return IsHex4(value) ? value : std::string();
}

} // namespace voicestick
