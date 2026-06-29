#include "app_config.h"

#include "ble_protocol.h"
#include "toml.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

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

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
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
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\t': out += "\\t";  break;
            case '\n': out += "\\n";  break;
            case '\f': out += "\\f";  break;
            case '\r': out += "\\r";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    // 其他控制字符用 \uXXXX 转义
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::optional<std::string> TomlString(const toml::table& table, std::string_view key) {
    return table[key].value<std::string>();
}

std::optional<bool> TomlBool(const toml::table& table, std::string_view key) {
    return table[key].value<bool>();
}

std::vector<std::string> TomlStringArray(const toml::table& table, std::string_view key) {
    std::vector<std::string> values;
    const auto* array = table[key].as_array();
    if (!array) return values;
    for (const auto& node : *array) {
        if (auto value = node.value<std::string>()) {
            values.push_back(*value);
        }
    }
    return values;
}

template <typename Value>
std::map<std::string, Value> ParseDeviceValueMap(
    std::string_view text,
    std::function<Value(std::string_view)> parse_value) {
    std::map<std::string, Value> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto part = comma == std::string_view::npos
            ? text.substr(start)
            : text.substr(start, comma - start);
        const auto colon = part.find(':');
        if (colon != std::string_view::npos) {
            auto device_id = BleProtocol::NormalizeDeviceId(part.substr(0, colon));
            auto value = Trim(std::string(part.substr(colon + 1)));
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (device_id.size() == 4 && !value.empty()) {
                result[device_id] = parse_value(value);
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

template <typename Value>
std::string FormatDeviceValueMap(const std::map<std::string, Value>& values,
                                 const std::vector<std::string>& paired_device_ids,
                                 Value default_value,
                                 std::function<std::string(Value)> format_value) {
    std::ostringstream out;
    bool first = true;
    for (const auto& [device_id, value] : values) {
        if (value == default_value) continue;
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) ==
            paired_device_ids.end()) {
            continue;
        }
        if (!first) out << ",";
        first = false;
        out << device_id << ":" << format_value(value);
    }
    return out.str();
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

std::filesystem::path AppConfig::PortableBaseDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool AppConfig::IsPortableMode() {
    std::error_code ec;
    return std::filesystem::exists(PortableBaseDirectory() / L"config.toml", ec);
}

std::filesystem::path AppConfig::ConfigDirectory() {
    if (IsPortableMode()) {
        return PortableBaseDirectory();
    }
    return KnownFolder(FOLDERID_RoamingAppData, L"APPDATA") / L"VoiceStick";
}

std::filesystem::path AppConfig::ConfigPath() {
    return ConfigDirectory() / L"config.toml";
}

std::filesystem::path AppConfig::DefaultDebugAudioDirectory() {
    if (IsPortableMode()) {
        return PortableBaseDirectory() / L"DebugAudio";
    }
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
    // Format: device_id,bluetooth_address_hex,address_kind,name[,hardware,firmware_version]
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
    entry.hardware = next_field();
    entry.firmware_version = next_field();
    return entry;
}

std::string FormatPairedDeviceEntry(const PairedDeviceEntry& entry) {
    char addr_buf[17]{};
    snprintf(addr_buf, sizeof(addr_buf), "%012llX", static_cast<unsigned long long>(entry.bluetooth_address));
    return entry.device_id + "," + addr_buf + "," +
           std::to_string(static_cast<int>(entry.address_kind)) + "," + entry.name + "," +
           entry.hardware + "," + entry.firmware_version;
}

} // namespace

namespace {

void ApplyConfigValue(AppConfig& config, const std::string& key, const std::string& value) {
    if (key == "asr_provider") config.asr_provider = AsrProviderFromName(value);
    if (key == "voicestick_api_key") config.voicestick_api_key = value;
    if (key == "voicestick_cloud_url") config.voicestick_cloud_url = value;
    if (key == "volcengine_api_key" || key == "api_key") config.volcengine_api_key = value;
    if (key == "tencent_secret_id") config.tencent_secret_id = value;
    if (key == "tencent_secret_key") config.tencent_secret_key = value;
    if (key == "tencent_appid") config.tencent_appid = value;
    if (key == "tencent_engine_model_type") config.tencent_engine_model_type = value;
    if (key == "tencent_hotword_id") config.tencent_hotword_id = value;
    if (key == "llm_base_url") config.llm_base_url = value;
    if (key == "llm_api_key") config.llm_api_key = value;
    if (key == "llm_model") config.llm_model = value;
    if (key == "refine_enabled") config.refine_enabled = BoolValue(value, config.refine_enabled);
    if (key == "refine_prompt") config.refine_prompt = value;
    if (key == "interaction_mode") config.interaction_mode = InteractionModeFromName(value);
    if (key == "ui_language") config.ui_language = UiLanguageFromName(value);
    if (key == "resource_id") config.resource_id = value;
    if (key == "asr_hotwords") config.asr_hotwords = ParseHotwordList(value);
    if (key == "paired_device_ids") config.paired_device_ids = ParseDeviceIdList(value);
    if (key == "device_theme_colors") {
        config.device_theme_colors = ParseDeviceValueMap<OverlayThemeColor>(value, OverlayThemeColorFromName);
    }
    if (key == "device_theme_sizes") {
        config.device_theme_sizes = ParseDeviceValueMap<OverlayThemeSize>(value, OverlayThemeSizeFromName);
    }
    if (key == "device_overlay_positions") {
        config.device_overlay_positions = ParseDeviceValueMap<OverlayPosition>(value, OverlayPositionFromName);
    }
    if (key == "output_target") config.default_output_profile.target = OutputTargetFromName(value);
    if (key == "text_transform") config.default_output_profile.transform = TextTransformFromName(value);
    if (key == "translation_target" && !value.empty()) config.default_output_profile.translation_target = value;
    if (key == "auto_enter") config.auto_enter = BoolValue(value, config.auto_enter);
    if (key == "global_hotkey_enabled") config.global_hotkey_enabled = BoolValue(value, config.global_hotkey_enabled);
    if (key == "global_hotkey") config.global_hotkey = value;
    if (key == "prompt_tone_enabled") config.prompt_tone_enabled = BoolValue(value, config.prompt_tone_enabled);
    if (key == "show_imu_debug") config.show_imu_debug = BoolValue(value, config.show_imu_debug);
    if (key == "imu_wake_sensitivity") config.imu_wake_sensitivity = ImuWakeSensitivityFromName(value);
    if (key == "show_device_wifi_info") config.show_device_wifi_info = BoolValue(value, config.show_device_wifi_info);
    if (key == "launch_at_login") config.launch_at_login = BoolValue(value, config.launch_at_login);
    if (key == "debug_audio_cache") config.debug_audio_cache = BoolValue(value, config.debug_audio_cache);
    if (key == "debug_audio_dir" && !value.empty()) config.debug_audio_directory = std::filesystem::path(value);
    if (key == "paired_device") {
        auto entry = ParsePairedDeviceEntry(value);
        if (!entry.device_id.empty()) config.paired_devices.push_back(entry);
    }
}

AppConfig LoadLegacyConfig(std::istream& input) {
    AppConfig config = AppConfig::Defaults();

    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line.starts_with("#")) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = Trim(line.substr(0, equals));
        const auto value = Unquote(line.substr(equals + 1));
        ApplyConfigValue(config, key, value);
    }
    return config;
}

OutputProfile ParseOutputProfile(const toml::table& table, const OutputProfile& fallback, bool include_target) {
    OutputProfile profile = fallback;
    if (include_target) {
        if (auto value = TomlString(table, "target")) profile.target = OutputTargetFromName(*value);
    }
    if (auto value = TomlString(table, "transform")) profile.transform = TextTransformFromName(*value);
    if (auto value = TomlString(table, "translation_target"); value && !value->empty()) {
        profile.translation_target = Trim(*value);
        if (profile.translation_target.empty()) profile.translation_target = fallback.translation_target;
    }
    return profile;
}

} // namespace

AppConfig AppConfig::Load() {
    AppConfig config = Load(ConfigPath());
    config.portable_mode = IsPortableMode();
    return config;
}

AppConfig AppConfig::Load(const std::filesystem::path& path) {
    AppConfig config = Defaults();
    std::ifstream input(path);
    if (!input) return config;

    try {
        auto table = toml::parse(input, path.native());

        if (auto value = TomlString(table, "asr_provider")) config.asr_provider = AsrProviderFromName(*value);
        if (auto value = TomlString(table, "voicestick_api_key")) config.voicestick_api_key = *value;
        if (auto value = TomlString(table, "voicestick_cloud_url")) config.voicestick_cloud_url = *value;
        if (auto value = TomlString(table, "volcengine_api_key")) config.volcengine_api_key = *value;
        if (auto value = TomlString(table, "api_key")) config.volcengine_api_key = *value;
        if (auto value = TomlString(table, "tencent_secret_id")) config.tencent_secret_id = *value;
        if (auto value = TomlString(table, "tencent_secret_key")) config.tencent_secret_key = *value;
        if (auto value = TomlString(table, "tencent_appid")) config.tencent_appid = *value;
        if (auto value = TomlString(table, "tencent_engine_model_type")) config.tencent_engine_model_type = *value;
        if (auto value = TomlString(table, "tencent_hotword_id")) config.tencent_hotword_id = *value;
        if (auto value = TomlString(table, "llm_base_url")) config.llm_base_url = *value;
        if (auto value = TomlString(table, "llm_api_key")) config.llm_api_key = *value;
        if (auto value = TomlString(table, "llm_model")) config.llm_model = *value;
        if (auto value = TomlBool(table, "refine_enabled")) config.refine_enabled = *value;
        if (auto value = TomlString(table, "refine_prompt")) config.refine_prompt = *value;
        if (auto value = TomlString(table, "interaction_mode")) config.interaction_mode = InteractionModeFromName(*value);
        if (auto value = TomlString(table, "ui_language")) config.ui_language = UiLanguageFromName(*value);
        if (auto value = TomlString(table, "resource_id")) config.resource_id = *value;
        if (auto value = TomlString(table, "asr_hotwords")) config.asr_hotwords = ParseHotwordList(*value);
        if (auto value = TomlString(table, "paired_device_ids")) config.paired_device_ids = ParseDeviceIdList(*value);
        if (auto value = TomlString(table, "device_theme_colors")) {
            config.device_theme_colors = ParseDeviceValueMap<OverlayThemeColor>(*value, OverlayThemeColorFromName);
        }
        if (auto value = TomlString(table, "device_theme_sizes")) {
            config.device_theme_sizes = ParseDeviceValueMap<OverlayThemeSize>(*value, OverlayThemeSizeFromName);
        }
        if (auto value = TomlString(table, "device_overlay_positions")) {
            config.device_overlay_positions = ParseDeviceValueMap<OverlayPosition>(*value, OverlayPositionFromName);
        }
        if (auto value = TomlString(table, "output_target")) {
            config.default_output_profile.target = OutputTargetFromName(*value);
        }
        if (auto value = TomlString(table, "text_transform")) {
            config.default_output_profile.transform = TextTransformFromName(*value);
        }
        if (auto value = TomlString(table, "translation_target"); value && !value->empty()) {
            config.default_output_profile.translation_target = Trim(*value);
        }
        if (const auto* output = table["output"].as_table()) {
            config.default_output_profile = ParseOutputProfile(
                *output, config.default_output_profile, true);
        }
        if (const auto* devices = table["device"].as_table()) {
            for (const auto& [key, node] : *devices) {
                const auto device_id = BleProtocol::NormalizeDeviceId(std::string_view(key.str()));
                if (device_id.size() != 4) continue;
                const auto* device_table = node.as_table();
                if (!device_table) continue;
                if (const auto* output = (*device_table)["output"].as_table()) {
                    config.device_output_profiles[device_id] = ParseOutputProfile(
                        *output, config.default_output_profile, false);
                    config.device_output_profiles[device_id].target = config.default_output_profile.target;
                }
                if (const auto* wifi = (*device_table)["wifi"].as_table()) {
                    WifiDeviceProfile profile;
                    if (auto value = TomlString(*wifi, "ssid")) profile.ssid = Trim(*value);
                    if (auto value = TomlString(*wifi, "ota_url")) profile.ota_url = Trim(*value);
                    if (auto value = TomlString(*wifi, "ota_sha256_hex")) profile.ota_sha256_hex = Trim(*value);
                    if (!profile.IsEmpty()) config.device_wifi_profiles[device_id] = std::move(profile);
                }
                if (const auto* wifi_info = (*device_table)["wifi_info"].as_table()) {
                    DeviceWifiInfo info;
                    if (auto value = TomlString(*wifi_info, "ssid")) info.ssid = Trim(*value);
                    if (auto value = TomlString(*wifi_info, "ip")) info.ip = Trim(*value);
                    config.device_wifi_infos[device_id] = std::move(info);
                }
            }
        }
        if (auto value = TomlBool(table, "auto_enter")) config.auto_enter = *value;
        if (auto value = TomlBool(table, "global_hotkey_enabled")) config.global_hotkey_enabled = *value;
        if (auto value = TomlString(table, "global_hotkey")) config.global_hotkey = *value;
        if (auto value = TomlBool(table, "prompt_tone_enabled")) config.prompt_tone_enabled = *value;
        if (auto value = TomlBool(table, "show_imu_debug")) config.show_imu_debug = *value;
        if (auto value = TomlString(table, "imu_wake_sensitivity")) config.imu_wake_sensitivity = ImuWakeSensitivityFromName(*value);
        if (auto value = TomlBool(table, "show_device_wifi_info")) config.show_device_wifi_info = *value;
        if (auto value = TomlBool(table, "launch_at_login")) config.launch_at_login = *value;
        if (auto value = TomlBool(table, "debug_audio_cache")) config.debug_audio_cache = *value;
        if (auto value = TomlString(table, "debug_audio_dir"); value && !value->empty()) {
            config.debug_audio_directory = std::filesystem::path(*value);
        }
        for (const auto& value : TomlStringArray(table, "paired_device")) {
            auto entry = ParsePairedDeviceEntry(value);
            if (!entry.device_id.empty()) config.paired_devices.push_back(entry);
        }
        return config;
    } catch (const toml::parse_error&) {
        input.clear();
        input.seekg(0);
        return LoadLegacyConfig(input);
    }
    return config;
}

void AppConfig::Save() const {
    Save(ConfigPath());
}

void AppConfig::Save(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
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
    output << "tencent_secret_id = \"" << TomlEscape(tencent_secret_id) << "\"\n";
    output << "tencent_secret_key = \"" << TomlEscape(tencent_secret_key) << "\"\n";
    output << "tencent_appid = \"" << TomlEscape(tencent_appid) << "\"\n";
    output << "tencent_engine_model_type = \"" << TomlEscape(tencent_engine_model_type) << "\"\n";
    output << "tencent_hotword_id = \"" << TomlEscape(tencent_hotword_id) << "\"\n";
    output << "llm_base_url = \"" << TomlEscape(llm_base_url) << "\"\n";
    output << "llm_api_key = \"" << TomlEscape(llm_api_key) << "\"\n";
    output << "llm_model = \"" << TomlEscape(llm_model) << "\"\n";
    output << "refine_enabled = " << (refine_enabled ? "true" : "false") << "\n";
    output << "refine_prompt = \"" << TomlEscape(refine_prompt) << "\"\n";
    output << "interaction_mode = \"" << InteractionModeName(interaction_mode) << "\"\n";
    output << "ui_language = \"" << UiLanguageName(ui_language) << "\"\n";
    output << "resource_id = \"" << TomlEscape(resource_id) << "\"\n";
    std::ostringstream hotwords;
    for (std::size_t i = 0; i < asr_hotwords.size(); ++i) {
        if (i != 0) hotwords << ",";
        hotwords << asr_hotwords[i];
    }
    output << "asr_hotwords = \"" << TomlEscape(hotwords.str()) << "\"\n";
    output << "paired_device_ids = \"" << paired.str() << "\"\n";
    output << "device_theme_colors = \"" << TomlEscape(FormatDeviceValueMap<OverlayThemeColor>(
        device_theme_colors, paired_device_ids, DefaultOverlayThemeColor(), OverlayThemeColorName)) << "\"\n";
    output << "device_theme_sizes = \"" << TomlEscape(FormatDeviceValueMap<OverlayThemeSize>(
        device_theme_sizes, paired_device_ids, OverlayThemeSize::kBig, OverlayThemeSizeName)) << "\"\n";
    output << "device_overlay_positions = \"" << TomlEscape(FormatDeviceValueMap<OverlayPosition>(
        device_overlay_positions, paired_device_ids, DefaultOverlayPosition(), OverlayPositionName)) << "\"\n";
    output << "auto_enter = " << (auto_enter ? "true" : "false") << "\n";
    output << "global_hotkey_enabled = " << (global_hotkey_enabled ? "true" : "false") << "\n";
    output << "global_hotkey = \"" << TomlEscape(global_hotkey) << "\"\n";
    output << "prompt_tone_enabled = " << (prompt_tone_enabled ? "true" : "false") << "\n";
    output << "show_imu_debug = " << (show_imu_debug ? "true" : "false") << "\n";
    output << "imu_wake_sensitivity = \"" << ImuWakeSensitivityName(imu_wake_sensitivity) << "\"\n";
    output << "show_device_wifi_info = " << (show_device_wifi_info ? "true" : "false") << "\n";
    output << "launch_at_login = " << (launch_at_login ? "true" : "false") << "\n";
    output << "debug_audio_cache = " << (debug_audio_cache ? "true" : "false") << "\n";
    output << "debug_audio_dir = \"" << TomlEscape(debug_audio_directory.string()) << "\"\n";
    if (!paired_devices.empty()) {
        output << "paired_device = [\n";
        for (const auto& entry : paired_devices) {
            output << "  \"" << TomlEscape(FormatPairedDeviceEntry(entry)) << "\",\n";
        }
        output << "]\n";
    }
    output << "\n[output]\n";
    output << "target = \"" << OutputTargetName(default_output_profile.target) << "\"\n";
    output << "transform = \"" << TextTransformName(default_output_profile.transform) << "\"\n";
    output << "translation_target = \"" << TomlEscape(default_output_profile.translation_target) << "\"\n";
    for (const auto& [device_id, profile] : device_output_profiles) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        OutputProfile comparable = profile;
        comparable.target = default_output_profile.target;
        OutputProfile default_comparable = default_output_profile;
        if (comparable == default_comparable) continue;
        output << "\n[device." << device_id << ".output]\n";
        output << "transform = \"" << TextTransformName(profile.transform) << "\"\n";
        output << "translation_target = \"" << TomlEscape(profile.translation_target) << "\"\n";
    }
    for (const auto& [device_id, profile] : device_wifi_profiles) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        if (profile.IsEmpty()) continue;
        output << "\n[device." << device_id << ".wifi]\n";
        output << "ssid = \"" << TomlEscape(profile.ssid) << "\"\n";
        output << "ota_url = \"" << TomlEscape(profile.ota_url) << "\"\n";
        output << "ota_sha256_hex = \"" << TomlEscape(profile.ota_sha256_hex) << "\"\n";
    }
    for (const auto& [device_id, info] : device_wifi_infos) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        output << "\n[device." << device_id << ".wifi_info]\n";
        output << "ssid = \"" << TomlEscape(info.ssid) << "\"\n";
        output << "ip = \"" << TomlEscape(info.ip) << "\"\n";
    }
}

std::string AppConfig::ActiveApiKey() const {
    switch (asr_provider) {
        case AsrProvider::kVoiceStickCloud: return voicestick_api_key;
        case AsrProvider::kVolcengine: return volcengine_api_key;
        case AsrProvider::kTencent: return tencent_secret_id;
    }
    return {};
}

std::string AppConfig::ActiveWebsocketUrl() const {
    if (asr_provider == AsrProvider::kVolcengine) return kVolcengineUrl;
    if (asr_provider == AsrProvider::kTencent) {
        // 腾讯云 WebSocket URL 由 AsrClientTencent 动态构建（含签名），此处返回静态前缀仅供诊断
        return "wss://asr.cloud.tencent.com/asr/v2/" + tencent_appid;
    }
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

void AppConfig::SavePairedDeviceInfo(const std::string& device_id,
                                     const std::string& hardware,
                                     const std::string& firmware_version) {
    bool found = false;
    for (auto& existing : paired_devices) {
        if (existing.device_id == device_id) {
            if (!hardware.empty()) existing.hardware = hardware;
            if (!firmware_version.empty()) existing.firmware_version = firmware_version;
            found = true;
            break;
        }
    }
    if (!found) {
        PairedDeviceEntry entry;
        entry.device_id = device_id;
        entry.hardware = hardware;
        entry.firmware_version = firmware_version;
        paired_devices.push_back(entry);
    }
    if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
        paired_device_ids.push_back(device_id);
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
    device_theme_colors.erase(device_id);
    device_theme_sizes.erase(device_id);
    device_overlay_positions.erase(device_id);
    device_output_profiles.erase(device_id);
    device_wifi_profiles.erase(device_id);
    device_wifi_infos.erase(device_id);
    Save();
}

OutputProfile AppConfig::OutputProfileForDevice(const std::optional<std::string>& device_id) const {
    if (!device_id.has_value()) return default_output_profile;
    const auto normalized = BleProtocol::NormalizeDeviceId(*device_id);
    auto it = device_output_profiles.find(normalized);
    if (it == device_output_profiles.end()) return default_output_profile;
    OutputProfile profile = it->second;
    profile.target = default_output_profile.target;
    return profile;
}

std::string AsrProviderName(AsrProvider provider) {
    switch (provider) {
        case AsrProvider::kVoiceStickCloud: return "voicestick_cloud";
        case AsrProvider::kVolcengine: return "volcengine";
        case AsrProvider::kTencent: return "tencent";
    }
    return "voicestick_cloud";
}

AsrProvider AsrProviderFromName(std::string_view name) {
    if (name == "voicestick_cloud") return AsrProvider::kVoiceStickCloud;
    if (name == "tencent") return AsrProvider::kTencent;
    return AsrProvider::kVolcengine;
}

std::string InteractionModeName(InteractionMode mode) {
    return mode == InteractionMode::kClickToTalk ? "click_to_talk" : "hold_to_talk";
}

InteractionMode InteractionModeFromName(std::string_view name) {
    return name == "click_to_talk" ? InteractionMode::kClickToTalk : InteractionMode::kHoldToTalk;
}

std::string UiLanguageName(UiLanguage language) {
    switch (language) {
    case UiLanguage::kEnglish: return "en";
    case UiLanguage::kSimplifiedChinese: return "zh-Hans";
    case UiLanguage::kSystem:
    default:
        return "system";
    }
}

UiLanguage UiLanguageFromName(std::string_view name) {
    if (name == "en") return UiLanguage::kEnglish;
    if (name == "zh-Hans" || name == "zh_CN" || name == "zh-CN" || name == "zh") {
        return UiLanguage::kSimplifiedChinese;
    }
    return UiLanguage::kSystem;
}

UiLanguage UiLanguageFromLocaleName(std::wstring_view locale_name) {
    if (locale_name.empty()) return UiLanguage::kEnglish;
    auto locale = Lowercase(std::wstring(locale_name));
    if (locale == L"zh" || locale.starts_with(L"zh-") || locale.starts_with(L"zh_")) {
        return UiLanguage::kSimplifiedChinese;
    }
    if (locale == L"en" || locale.starts_with(L"en-") || locale.starts_with(L"en_")) {
        return UiLanguage::kEnglish;
    }
    return UiLanguage::kEnglish;
}

UiLanguage EffectiveUiLanguage(UiLanguage configured) {
    if (configured != UiLanguage::kSystem) return configured;

    ULONG count = 0;
    ULONG buffer_length = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &buffer_length) && buffer_length > 0) {
        std::wstring buffer(buffer_length, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &buffer_length)) {
            const wchar_t* first = buffer.c_str();
            if (*first != L'\0') return UiLanguageFromLocaleName(first);
        }
    }

    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        return UiLanguageFromLocaleName(locale_name);
    }
    return UiLanguage::kEnglish;
}

OverlayThemeColor DefaultOverlayThemeColor() {
    return OverlayThemeColor::kAuto;
}

OverlayPosition DefaultOverlayPosition() {
    return OverlayPosition::kBottomCenter;
}

std::string OverlayThemeColorName(OverlayThemeColor color) {
    switch (color) {
    case OverlayThemeColor::kAuto: return "auto";
    case OverlayThemeColor::kBlack: return "black";
    case OverlayThemeColor::kPink: return "pink";
    case OverlayThemeColor::kGreen: return "green";
    case OverlayThemeColor::kYellow: return "yellow";
    case OverlayThemeColor::kBlue: return "blue";
    case OverlayThemeColor::kPurple: return "purple";
    case OverlayThemeColor::kWhite:
    default:
        return "white";
    }
}

OverlayThemeColor OverlayThemeColorFromName(std::string_view name) {
    if (name == "auto") return OverlayThemeColor::kAuto;
    if (name == "black") return OverlayThemeColor::kBlack;
    if (name == "pink") return OverlayThemeColor::kPink;
    if (name == "green") return OverlayThemeColor::kGreen;
    if (name == "yellow") return OverlayThemeColor::kYellow;
    if (name == "blue") return OverlayThemeColor::kBlue;
    if (name == "purple") return OverlayThemeColor::kPurple;
    return OverlayThemeColor::kWhite;
}

std::string OverlayThemeColorDisplayName(OverlayThemeColor color) {
    switch (color) {
    case OverlayThemeColor::kAuto: return "Auto";
    case OverlayThemeColor::kBlack: return "Black";
    case OverlayThemeColor::kPink: return "Pink";
    case OverlayThemeColor::kGreen: return "Green";
    case OverlayThemeColor::kYellow: return "Yellow";
    case OverlayThemeColor::kBlue: return "Blue";
    case OverlayThemeColor::kPurple: return "Purple";
    case OverlayThemeColor::kWhite:
    default:
        return "White";
    }
}

std::string OverlayThemeSizeName(OverlayThemeSize size) {
    switch (size) {
    case OverlayThemeSize::kMedium: return "medium";
    case OverlayThemeSize::kSmall: return "small";
    case OverlayThemeSize::kBig:
    default:
        return "big";
    }
}

OverlayThemeSize OverlayThemeSizeFromName(std::string_view name) {
    if (name == "medium") return OverlayThemeSize::kMedium;
    if (name == "small") return OverlayThemeSize::kSmall;
    return OverlayThemeSize::kBig;
}

std::string OverlayThemeSizeDisplayName(OverlayThemeSize size) {
    switch (size) {
    case OverlayThemeSize::kMedium: return "Medium";
    case OverlayThemeSize::kSmall: return "Small";
    case OverlayThemeSize::kBig:
    default:
        return "Big";
    }
}

std::string OverlayPositionName(OverlayPosition position) {
    switch (position) {
    case OverlayPosition::kBottomCenter: return "bottom_center";
    case OverlayPosition::kTopLeft: return "top_left";
    case OverlayPosition::kTopRight: return "top_right";
    case OverlayPosition::kBottomLeft: return "bottom_left";
    case OverlayPosition::kBottomRight: return "bottom_right";
    case OverlayPosition::kCenter:
    default:
        return "center";
    }
}

OverlayPosition OverlayPositionFromName(std::string_view name) {
    if (name == "bottom_center" || name == "middle_bottom") return OverlayPosition::kBottomCenter;
    if (name == "top_left") return OverlayPosition::kTopLeft;
    if (name == "top_right") return OverlayPosition::kTopRight;
    if (name == "bottom_left") return OverlayPosition::kBottomLeft;
    if (name == "bottom_right") return OverlayPosition::kBottomRight;
    return OverlayPosition::kCenter;
}

std::string OverlayPositionDisplayName(OverlayPosition position) {
    switch (position) {
    case OverlayPosition::kBottomCenter: return "Bottom Center";
    case OverlayPosition::kTopLeft: return "Top Left";
    case OverlayPosition::kTopRight: return "Top Right";
    case OverlayPosition::kBottomLeft: return "Bottom Left";
    case OverlayPosition::kBottomRight: return "Bottom Right";
    case OverlayPosition::kCenter:
    default:
        return "Center";
    }
}

std::string OutputTargetName(OutputTarget target) {
    return target == OutputTarget::kSubtitle ? "subtitle" : "focused_app";
}

OutputTarget OutputTargetFromName(std::string_view name) {
    return name == "subtitle" ? OutputTarget::kSubtitle : OutputTarget::kFocusedApp;
}

std::string OutputTargetDisplayName(OutputTarget target) {
    return target == OutputTarget::kSubtitle ? "Subtitle" : "Focused App";
}

std::string TextTransformName(TextTransform transform) {
    return transform == TextTransform::kTranslate ? "translate" : "original";
}

TextTransform TextTransformFromName(std::string_view name) {
    return name == "translate" ? TextTransform::kTranslate : TextTransform::kOriginal;
}

std::string TextTransformDisplayName(TextTransform transform) {
    return transform == TextTransform::kTranslate ? "Translate" : "Original";
}

std::string ImuWakeSensitivityName(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return "medium";
    case ImuWakeSensitivity::kHigh: return "high";
    case ImuWakeSensitivity::kLow:
    default:
        return "low";
    }
}

ImuWakeSensitivity ImuWakeSensitivityFromName(std::string_view name) {
    if (name == "medium") return ImuWakeSensitivity::kMedium;
    if (name == "high") return ImuWakeSensitivity::kHigh;
    return ImuWakeSensitivity::kLow;
}

std::string ImuWakeSensitivityDisplayName(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return "Medium";
    case ImuWakeSensitivity::kHigh: return "High";
    case ImuWakeSensitivity::kLow:
    default:
        return "Low";
    }
}

int ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity sensitivity) {
    switch (sensitivity) {
    case ImuWakeSensitivity::kMedium: return 500;
    case ImuWakeSensitivity::kHigh: return 250;
    case ImuWakeSensitivity::kLow:
    default:
        return 800;
    }
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

std::vector<std::string> ParseHotwordList(std::string_view text) {
    std::vector<std::string> hotwords;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find_first_of(",\r\n", start);
        const auto part = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        auto hotword = Trim(std::string(part));
        if (!hotword.empty() && std::find(hotwords.begin(), hotwords.end(), hotword) == hotwords.end()) {
            hotwords.push_back(std::move(hotword));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return hotwords;
}

} // namespace voicestick
