#include "app_config.h"

#include "ble_protocol.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace voicestick {

namespace {

constexpr const char* kVolcengineUrl = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";

std::filesystem::path KnownFolder(REFKNOWNFOLDERID folder_id, const wchar_t* fallback_env) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder_id, 0, nullptr, &path)) && path != nullptr) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(fallback_env, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer);
    }
    return std::filesystem::current_path();
}

std::string Trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string Unquote(std::string value) {
    value = Trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (char ch : value) {
        if (escaped) {
            out.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

bool BoolValue(const std::string& value, bool fallback) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "true" || lower == "yes" || lower == "1" || lower == "on") return true;
    if (lower == "false" || lower == "no" || lower == "0" || lower == "off") return false;
    return fallback;
}

std::string TomlEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

} // namespace

const std::vector<std::string>& AppConfig::SupportedResourceIds() {
    // Mirrors macOS AppConfig.supportedResourceIDs so both platforms expose the
    // same Volcengine model options.
    static const std::vector<std::string> ids = {
        "volc.seedasr.sauc.duration",
        "volc.seedasr.sauc.concurrent",
        "volc.bigasr.sauc.duration",
        "volc.bigasr.sauc.concurrent",
    };
    return ids;
}

std::filesystem::path AppConfig::ConfigDirectory() {
    return KnownFolder(FOLDERID_RoamingAppData, L"APPDATA") / L"VoiceStick";
}

std::filesystem::path AppConfig::ConfigPath() {
    return ConfigDirectory() / L"config.toml";
}

std::filesystem::path AppConfig::DefaultDebugAudioDirectory() {
    return KnownFolder(FOLDERID_LocalAppData, L"LOCALAPPDATA") / L"VoiceStick" / L"DebugAudio";
}

AppConfig AppConfig::Defaults() {
    AppConfig config;
    config.debug_audio_directory = DefaultDebugAudioDirectory();
    return config;
}

namespace {

PairedDeviceEntry ParsePairedDeviceEntry(std::string_view line) {
    PairedDeviceEntry entry;
    // Format: device_id,bluetooth_address_hex,address_kind,name
    std::size_t start = 0;
    auto next_field = [&]() -> std::string {
        if (start > line.size()) return {};
        auto comma = line.find(',', start);
        auto part = (comma == std::string_view::npos)
                        ? line.substr(start)
                        : line.substr(start, comma - start);
        start = (comma == std::string_view::npos) ? line.size() + 1 : comma + 1;
        return std::string(part);
    };
    entry.device_id = next_field();
    auto addr_str = next_field();
    if (!addr_str.empty()) {
        entry.bluetooth_address = std::strtoull(addr_str.c_str(), nullptr, 16);
    }
    auto kind_str = next_field();
    if (kind_str == "1") entry.address_kind = BluetoothAddressKind::kPublic;
    else if (kind_str == "2") entry.address_kind = BluetoothAddressKind::kRandom;
    entry.name = next_field();
    return entry;
}

std::string FormatPairedDeviceEntry(const PairedDeviceEntry& entry) {
    char addr_buf[17]{};
    snprintf(addr_buf, sizeof(addr_buf), "%012llX", static_cast<unsigned long long>(entry.bluetooth_address));
    return entry.device_id + "," + addr_buf + "," +
           std::to_string(static_cast<int>(entry.address_kind)) + "," + entry.name;
}

} // namespace

AppConfig AppConfig::Load() {
    AppConfig config = Defaults();
    std::ifstream input(ConfigPath());
    if (!input) return config;

    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line.starts_with("#")) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = Trim(line.substr(0, equals));
        const auto value = Unquote(line.substr(equals + 1));

        if (key == "asr_provider") config.asr_provider = AsrProviderFromName(value);
        if (key == "voicestick_api_key") config.voicestick_api_key = value;
        if (key == "voicestick_cloud_url") config.voicestick_cloud_url = value;
        if (key == "volcengine_api_key" || key == "api_key") config.volcengine_api_key = value;
        if (key == "resource_id") config.resource_id = value;
        if (key == "paired_device_ids") config.paired_device_ids = ParseDeviceIdList(value);
        if (key == "auto_enter") config.auto_enter = BoolValue(value, config.auto_enter);
        if (key == "debug_audio_cache") config.debug_audio_cache = BoolValue(value, config.debug_audio_cache);
        if (key == "debug_audio_dir" && !value.empty()) config.debug_audio_directory = std::filesystem::path(value);
        if (key == "paired_device") {
            auto entry = ParsePairedDeviceEntry(value);
            if (!entry.device_id.empty()) config.paired_devices.push_back(entry);
        }
    }
    return config;
}

void AppConfig::Save() const {
    std::filesystem::create_directories(ConfigDirectory());
    std::ofstream output(ConfigPath(), std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open config for writing");
    }
    std::ostringstream paired;
    for (std::size_t i = 0; i < paired_device_ids.size(); ++i) {
        if (i != 0) paired << ",";
        paired << paired_device_ids[i];
    }
    output << "asr_provider = \"" << AsrProviderName(asr_provider) << "\"\n";
    output << "voicestick_api_key = \"" << TomlEscape(voicestick_api_key) << "\"\n";
    output << "voicestick_cloud_url = \"" << TomlEscape(voicestick_cloud_url) << "\"\n";
    output << "volcengine_api_key = \"" << TomlEscape(volcengine_api_key) << "\"\n";
    output << "resource_id = \"" << TomlEscape(resource_id) << "\"\n";
    output << "paired_device_ids = \"" << paired.str() << "\"\n";
    output << "auto_enter = " << (auto_enter ? "true" : "false") << "\n";
    output << "debug_audio_cache = " << (debug_audio_cache ? "true" : "false") << "\n";
    output << "debug_audio_dir = \"" << TomlEscape(debug_audio_directory.string()) << "\"\n";
    for (const auto& entry : paired_devices) {
        output << "paired_device = \"" << TomlEscape(FormatPairedDeviceEntry(entry)) << "\"\n";
    }
}

std::string AppConfig::ActiveApiKey() const {
    return asr_provider == AsrProvider::kVoiceStickCloud ? voicestick_api_key : volcengine_api_key;
}

std::string AppConfig::ActiveWebsocketUrl() const {
    if (asr_provider == AsrProvider::kVolcengine) return kVolcengineUrl;
    auto url = Trim(voicestick_cloud_url);
    return url.empty() ? AppConfig{}.voicestick_cloud_url : url;
}

void AppConfig::SavePairedDevice(const PairedDeviceEntry& entry) {
    bool found = false;
    for (auto& existing : paired_devices) {
        if (existing.device_id == entry.device_id) {
            existing = entry;
            found = true;
            break;
        }
    }
    if (!found) {
        paired_devices.push_back(entry);
    }
    if (std::find(paired_device_ids.begin(), paired_device_ids.end(), entry.device_id) == paired_device_ids.end()) {
        paired_device_ids.push_back(entry.device_id);
    }
    Save();
}

void AppConfig::RemovePairedDevice(const std::string& device_id) {
    paired_devices.erase(
        std::remove_if(paired_devices.begin(), paired_devices.end(),
                       [&](const PairedDeviceEntry& e) { return e.device_id == device_id; }),
        paired_devices.end());
    paired_device_ids.erase(
        std::remove(paired_device_ids.begin(), paired_device_ids.end(), device_id),
        paired_device_ids.end());
    Save();
}

std::string AsrProviderName(AsrProvider provider) {
    return provider == AsrProvider::kVoiceStickCloud ? "voicestick_cloud" : "volcengine";
}

AsrProvider AsrProviderFromName(std::string_view name) {
    return name == "voicestick_cloud" ? AsrProvider::kVoiceStickCloud : AsrProvider::kVolcengine;
}

std::vector<std::string> ParseDeviceIdList(std::string_view text) {
    std::vector<std::string> ids;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto part = text.substr(start, comma == std::string_view::npos ? text.size() - start : comma - start);
        auto id = BleProtocol::NormalizeDeviceId(part);
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return ids;
}

} // namespace voicestick
