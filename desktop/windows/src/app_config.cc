#include "app_config.h"

#include "builtin_secrets.h"

#include "ble_protocol.h"
#include "key_spec.h"
#include "log.h"
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

// UTF-8 <-> UTF-16 转换：std::filesystem::path 在 Windows 按 ACP(GBK) 解析 std::string，
// 而 TOML 字符串是 UTF-8。直接 path(utf8_string) 会让含非 ASCII 的路径乱码；
// path.string() 按 ACP 输出时，含 ACP 无法表示字符会抛 system_error。用这两个 helper
// 在 path 与 UTF-8 字符串间显式转换，绕开 ACP。
std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), len);
    return wide;
}

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

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

int IntValue(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    try {
        std::size_t pos = 0;
        const int parsed = std::stoi(value, &pos);
        if (pos != value.size()) return fallback;
        return parsed;
    } catch (...) {
        return fallback;
    }
}

double DoubleValue(const std::string& value, double fallback) {
    if (value.empty()) return fallback;
    try {
        std::size_t pos = 0;
        const double parsed = std::stod(value, &pos);
        if (pos != value.size()) return fallback;
        return parsed;
    } catch (...) {
        return fallback;
    }
}

bool IsValidEncoderButtonAction(const std::string& value) {
    return value == "recording" || value == "key";
}

bool IsValidEncoderLedColor(const std::string& value) {
    return value == "red" || value == "green" || value == "blue" || value == "yellow" ||
           value == "purple" || value == "cyan" || value == "white" || value == "off";
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

std::optional<std::string> TomlTrimmedString(const toml::table& table, std::string_view key) {
    if (auto value = TomlString(table, key)) return Trim(*value);
    return std::nullopt;
}

std::optional<bool> TomlBool(const toml::table& table, std::string_view key) {
    return table[key].value<bool>();
}

std::optional<int> TomlInt(const toml::table& table, std::string_view key) {
    return table[key].value<int>();
}

std::optional<double> TomlDouble(const toml::table& table, std::string_view key) {
    // TOML 里数值可能被写成整数（如 1）或浮点（1.0），两者都接受。
    if (auto value = table[key].value<double>()) return value;
    if (auto value = table[key].value<int>()) return static_cast<double>(*value);
    return std::nullopt;
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

bool AppConfig::SeedConfigFromTemplate(const std::filesystem::path& template_path,
                                       const std::filesystem::path& target_path) {
    std::error_code ec;
    // 目标已存在 → 不覆盖，保护用户已有配置（含升级场景）。
    if (std::filesystem::exists(target_path, ec)) return false;
    // 模板不存在（如未随安装分发）→ 静默跳过，回退 Defaults。
    if (!std::filesystem::exists(template_path, ec)) return false;
    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
    }
    std::filesystem::copy_file(template_path, target_path, ec);
    return !ec;
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
    if (key == "volcengine_boosting_table_id") config.volcengine_boosting_table_id = value;
    if (key == "volcengine_correct_table_id") config.volcengine_correct_table_id = value;
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
    if (key == "hotword_process_enabled") config.hotword_process_enabled = BoolValue(value, config.hotword_process_enabled);
    if (key == "hotword_mining_enabled") config.hotword_mining_enabled = BoolValue(value, config.hotword_mining_enabled);
    if (key == "hotword_process_prompt") config.hotword_process_prompt = value;
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
    if (key == "wechat_input_method_hotkey") config.wechat_input_method.hotkey = value;
    if (key == "wechat_input_method_hotkey_hold") config.wechat_input_method.hotkey_hold = value;
    if (key == "wechat_input_method_hotkey_click") config.wechat_input_method.hotkey_click = value;
    if (key == "wechat_input_method_virtual_mic") config.wechat_input_method.virtual_mic_playback_name = value;
    if (key == "wechat_input_method_virtual_mic_capture_name") config.wechat_input_method.virtual_mic_capture_name = value;
    if (key == "wechat_input_method_auto_switch") {
        config.wechat_input_method.auto_switch_default_recording_device = BoolValue(value, false);
    }
    if (key == "auto_enter") config.auto_enter = BoolValue(value, config.auto_enter);
    if (key == "global_hotkey_enabled") config.global_hotkey_enabled = BoolValue(value, config.global_hotkey_enabled);
    if (key == "global_hotkey") config.global_hotkey = value;
    if (key == "show_imu_debug") config.show_imu_debug = BoolValue(value, config.show_imu_debug);
    if (key == "imu_wake_sensitivity") config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivityFromName(value);
    if (key == "tap_to_arrow") config.default_interaction_settings.tap_to_arrow = BoolValue(value, config.default_interaction_settings.tap_to_arrow);
    if (key == "encoder_to_arrow") config.default_encoder_settings.to_arrow = BoolValue(value, config.default_encoder_settings.to_arrow);
    if (key == "encoder_rotation_invert") config.default_encoder_settings.rotation_invert = BoolValue(value, config.default_encoder_settings.rotation_invert);
    if (key == "encoder_rotate_cw_key" && ParseKeySpec(value).has_value()) config.default_encoder_settings.rotate_cw_key = value;
    if (key == "encoder_rotate_ccw_key" && ParseKeySpec(value).has_value()) config.default_encoder_settings.rotate_ccw_key = value;
    if (key == "encoder_rotate_fast_threshold") {
        const int parsed = IntValue(value, config.default_encoder_settings.rotate_fast_threshold);
        if (parsed > 0) config.default_encoder_settings.rotate_fast_threshold = parsed;
    }
    if (key == "encoder_rotate_cw_fast_key" && ParseKeySpec(value).has_value()) config.default_encoder_settings.rotate_cw_fast_key = value;
    if (key == "encoder_rotate_ccw_fast_key" && ParseKeySpec(value).has_value()) config.default_encoder_settings.rotate_ccw_fast_key = value;
    if (key == "encoder_rotate_decide_window_ms") {
        const int parsed = IntValue(value, config.default_encoder_settings.rotate_decide_window_ms);
        if (parsed >= 0) config.default_encoder_settings.rotate_decide_window_ms = parsed;
    }
    if (key == "encoder_led_color" && IsValidEncoderLedColor(value)) config.default_encoder_settings.led_color = value;
    if (key == "encoder_press_action" && IsValidEncoderButtonAction(value)) config.default_encoder_settings.press_action = value;
    if (key == "encoder_press_key" && (value.empty() || ParseKeySpec(value).has_value())) config.default_encoder_settings.press_key = value;
    if (key == "encoder_double_click_action" && IsValidEncoderButtonAction(value)) config.default_encoder_settings.double_click_action = value;
    if (key == "encoder_double_click_key" && ParseKeySpec(value).has_value()) config.default_encoder_settings.double_click_key = value;
    if (key == "tap_sensitivity") config.default_interaction_settings.tap_sensitivity = TapSensitivityClamp(IntValue(value, config.default_interaction_settings.tap_sensitivity));
    if (key == "air_mouse_sensitivity_x") config.default_interaction_settings.air_mouse_sensitivity_x = AirMouseSensitivityClamp(IntValue(value, config.default_interaction_settings.air_mouse_sensitivity_x));
    if (key == "air_mouse_sensitivity_y") config.default_interaction_settings.air_mouse_sensitivity_y = AirMouseSensitivityClamp(IntValue(value, config.default_interaction_settings.air_mouse_sensitivity_y));
    if (key == "air_mouse_tau") config.air_mouse_tau = AirMouseTauClamp(DoubleValue(value, config.air_mouse_tau));
    if (key == "air_mouse_invert_y") config.air_mouse_invert_y = BoolValue(value, config.air_mouse_invert_y);
    if (key == "air_mouse_curve_low_thresh") config.air_mouse_curve_low_thresh = DoubleValue(value, config.air_mouse_curve_low_thresh);
    if (key == "air_mouse_curve_high_thresh") config.air_mouse_curve_high_thresh = DoubleValue(value, config.air_mouse_curve_high_thresh);
    if (key == "air_mouse_curve_low_factor") config.air_mouse_curve_low_factor = DoubleValue(value, config.air_mouse_curve_low_factor);
    if (key == "air_mouse_curve_high_factor") config.air_mouse_curve_high_factor = DoubleValue(value, config.air_mouse_curve_high_factor);
    if (key == "air_mouse_neutral_deadzone") config.air_mouse_neutral_deadzone = AirMouseNeutralDeadzoneClamp(DoubleValue(value, config.air_mouse_neutral_deadzone));
    if (key == "air_mouse_control_mode") config.air_mouse_control_mode = AirMouseControlModeName(AirMouseControlModeFromName(value));
    if (key == "air_mouse_rate_gain") config.air_mouse_rate_gain = AirMouseRateGainClamp(DoubleValue(value, config.air_mouse_rate_gain));
    if (key == "air_mouse_rate_friction") config.air_mouse_rate_friction = AirMouseRateFrictionClamp(DoubleValue(value, config.air_mouse_rate_friction));
    if (key == "air_mouse_rate_max_speed") config.air_mouse_rate_max_speed = AirMouseRateMaxSpeedClamp(DoubleValue(value, config.air_mouse_rate_max_speed));
    if (key == "launch_at_login") config.launch_at_login = BoolValue(value, config.launch_at_login);
    if (key == "selection_hotword_enabled") config.selection_hotword_enabled = BoolValue(value, config.selection_hotword_enabled);
    if (key == "debug_audio_cache") config.debug_audio_cache = BoolValue(value, config.debug_audio_cache);
    if (key == "debug_audio_dir" && !value.empty()) config.debug_audio_directory = std::filesystem::path(Utf16FromUtf8(value));
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

// 解析 [device.<id>.encoder] 覆盖表：以全局默认填平所有字段，出现的键逐项覆盖；
// 非法值（非法 key_spec/action/led_color、非正阈值）保留 fallback，与顶层解析语义一致。
EncoderSettings ParseEncoderSettings(const toml::table& table, const EncoderSettings& fallback) {
    EncoderSettings settings = fallback;
    if (auto value = TomlBool(table, "to_arrow")) settings.to_arrow = *value;
    if (auto value = TomlBool(table, "rotation_invert")) settings.rotation_invert = *value;
    if (auto value = TomlString(table, "rotate_cw_key"); value && ParseKeySpec(*value).has_value()) settings.rotate_cw_key = *value;
    if (auto value = TomlString(table, "rotate_ccw_key"); value && ParseKeySpec(*value).has_value()) settings.rotate_ccw_key = *value;
    if (auto value = TomlInt(table, "rotate_fast_threshold"); value && *value > 0) settings.rotate_fast_threshold = static_cast<int>(*value);
    if (auto value = TomlString(table, "rotate_cw_fast_key"); value && ParseKeySpec(*value).has_value()) settings.rotate_cw_fast_key = *value;
    if (auto value = TomlString(table, "rotate_ccw_fast_key"); value && ParseKeySpec(*value).has_value()) settings.rotate_ccw_fast_key = *value;
    if (auto value = TomlInt(table, "rotate_decide_window_ms"); value && *value >= 0) settings.rotate_decide_window_ms = static_cast<int>(*value);
    if (auto value = TomlString(table, "led_color"); value && IsValidEncoderLedColor(*value)) settings.led_color = *value;
    if (auto value = TomlString(table, "press_action"); value && IsValidEncoderButtonAction(*value)) settings.press_action = *value;
    if (auto value = TomlString(table, "press_key"); value && (value->empty() || ParseKeySpec(*value).has_value())) settings.press_key = *value;
    if (auto value = TomlString(table, "double_click_action"); value && IsValidEncoderButtonAction(*value)) settings.double_click_action = *value;
    if (auto value = TomlString(table, "double_click_key"); value && ParseKeySpec(*value).has_value()) settings.double_click_key = *value;
    return settings;
}

// 解析 [device.<id>.interaction] 覆盖表：以全局默认填平所有字段，出现的键逐项覆盖；
// 非法值（越界灵敏度）保留 fallback，与顶层解析语义一致。
InteractionSettings ParseInteractionSettings(const toml::table& table, const InteractionSettings& fallback) {
    InteractionSettings settings = fallback;
    if (auto value = TomlString(table, "imu_wake_sensitivity")) settings.imu_wake_sensitivity = ImuWakeSensitivityFromName(*value);
    if (auto value = TomlBool(table, "tap_to_arrow")) settings.tap_to_arrow = *value;
    if (auto value = TomlInt(table, "tap_sensitivity")) settings.tap_sensitivity = TapSensitivityClamp(*value);
    if (auto value = TomlInt(table, "air_mouse_sensitivity_x")) settings.air_mouse_sensitivity_x = AirMouseSensitivityClamp(*value);
    if (auto value = TomlInt(table, "air_mouse_sensitivity_y")) settings.air_mouse_sensitivity_y = AirMouseSensitivityClamp(*value);
    return settings;
}

// 修复早期设置/引导对话框在 ASR 提供商切换时的字段映射 bug：Tencent SecretId
// 被误写入 volcengine_api_key。触发回迁的条件：
// - 当前提供商为 Tencent
// - volcengine_api_key 看起来像 Tencent SecretId（以 AKID 开头）
// - tencent_secret_id 为空或不以 AKID 开头
// 满足时把 volcengine_api_key 移回 tencent_secret_id，避免 4002 密钥不存在。
bool MaybeRecoverTencentSecretId(AppConfig& config) {
    if (config.asr_provider != AsrProvider::kTencent) return false;
    if (config.volcengine_api_key.empty()) return false;
    if (!config.volcengine_api_key.starts_with("AKID")) return false;
    if (!config.tencent_secret_id.empty() &&
        config.tencent_secret_id.starts_with("AKID")) {
        return false;
    }
    config.tencent_secret_id = config.volcengine_api_key;
    config.volcengine_api_key.clear();
    return true;
}

} // namespace

AppConfig AppConfig::Load() {
    // 非便携模式下，首次启动从 exe 同级模板种子一份配置到 %APPDATA%，方便 MSI 分发预设配置。
    // 已有配置不覆盖；便携模式直接读 exe 目录，无需种子。
    if (!IsPortableMode()) {
        SeedConfigFromTemplate(PortableBaseDirectory() / L"config.template.toml", ConfigPath());
    }
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
        bool needs_wechat_trigger_migration_save = false;

        if (auto value = TomlString(table, "asr_provider")) config.asr_provider = AsrProviderFromName(*value);
        if (auto value = TomlTrimmedString(table, "voicestick_api_key")) config.voicestick_api_key = *value;
        if (auto value = TomlString(table, "voicestick_cloud_url")) config.voicestick_cloud_url = *value;
        if (auto value = TomlTrimmedString(table, "volcengine_api_key")) config.volcengine_api_key = *value;
        if (auto value = TomlTrimmedString(table, "api_key")) config.volcengine_api_key = *value;
        if (auto value = TomlTrimmedString(table, "volcengine_boosting_table_id")) config.volcengine_boosting_table_id = *value;
        if (auto value = TomlTrimmedString(table, "volcengine_correct_table_id")) config.volcengine_correct_table_id = *value;
        if (auto value = TomlTrimmedString(table, "tencent_secret_id")) config.tencent_secret_id = *value;
        if (auto value = TomlTrimmedString(table, "tencent_secret_key")) config.tencent_secret_key = *value;
        if (auto value = TomlTrimmedString(table, "tencent_appid")) config.tencent_appid = *value;
        if (auto value = TomlString(table, "tencent_engine_model_type")) config.tencent_engine_model_type = *value;
        if (auto value = TomlTrimmedString(table, "tencent_hotword_id")) config.tencent_hotword_id = *value;
        if (auto value = TomlTrimmedString(table, "llm_base_url")) config.llm_base_url = *value;
        if (auto value = TomlTrimmedString(table, "llm_api_key")) config.llm_api_key = *value;
        if (auto value = TomlString(table, "llm_model")) config.llm_model = *value;
        if (auto value = TomlBool(table, "refine_enabled")) config.refine_enabled = *value;
        if (auto value = TomlString(table, "refine_prompt")) config.refine_prompt = *value;
        if (auto value = TomlBool(table, "hotword_process_enabled")) config.hotword_process_enabled = *value;
        if (auto value = TomlBool(table, "hotword_mining_enabled")) config.hotword_mining_enabled = *value;
        if (auto value = TomlString(table, "hotword_process_prompt")) config.hotword_process_prompt = *value;
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
        if (const auto* wechat = table["wechat_input_method"].as_table()) {
            // legacy hotkey 字段：旧配置只有它，回退到 hotkey_hold/hotkey_click，不丢用户自定义。
            const std::optional<std::string> legacy_hotkey = TomlString(*wechat, "hotkey");
            if (legacy_hotkey) {
                config.wechat_input_method.hotkey = *legacy_hotkey;
            }
            if (auto value = TomlString(*wechat, "hotkey_hold")) {
                config.wechat_input_method.hotkey_hold = *value;
            } else if (legacy_hotkey) {
                config.wechat_input_method.hotkey_hold = *legacy_hotkey;
            }
            if (auto value = TomlString(*wechat, "hotkey_click")) {
                config.wechat_input_method.hotkey_click = *value;
            } else if (legacy_hotkey) {
                config.wechat_input_method.hotkey_click = *legacy_hotkey;
            }
            if (auto value = TomlString(*wechat, "virtual_mic_playback_name")) {
                config.wechat_input_method.virtual_mic_playback_name = *value;
            }
            if (auto value = TomlString(*wechat, "virtual_mic_capture_name")) {
                config.wechat_input_method.virtual_mic_capture_name = *value;
            }
            if (auto value = TomlBool(*wechat, "auto_switch_default_recording_device")) {
                config.wechat_input_method.auto_switch_default_recording_device = *value;
            }
            if (auto value = TomlString(*wechat, "trigger_mode")) {
                config.wechat_input_method.trigger_mode = InteractionModeFromName(*value);
            } else {
                // 旧配置迁移：trigger_mode 字段缺失，从顶层 interaction_mode 继承（保留用户
                // 之前为 wechat 选的点按式），并把顶层 interaction_mode 重置为 kHoldToTalk
                //（focused_app/字幕不再继承 wechat 的点按式，修复切输出目标后长按失效）。
                config.wechat_input_method.trigger_mode = config.interaction_mode;
                config.interaction_mode = InteractionMode::kHoldToTalk;
                needs_wechat_trigger_migration_save = true;
            }
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
                if (const auto* encoder = (*device_table)["encoder"].as_table()) {
                    config.device_encoder_settings[device_id] = ParseEncoderSettings(
                        *encoder, config.default_encoder_settings);
                }
                if (const auto* interaction = (*device_table)["interaction"].as_table()) {
                    config.device_interaction_settings[device_id] = ParseInteractionSettings(
                        *interaction, config.default_interaction_settings);
                }
            }
        }
        if (auto value = TomlBool(table, "auto_enter")) config.auto_enter = *value;
        if (auto value = TomlBool(table, "global_hotkey_enabled")) config.global_hotkey_enabled = *value;
        if (auto value = TomlString(table, "global_hotkey")) config.global_hotkey = *value;
        if (auto value = TomlBool(table, "show_imu_debug")) config.show_imu_debug = *value;
        if (auto value = TomlString(table, "imu_wake_sensitivity")) config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivityFromName(*value);
        if (auto value = TomlBool(table, "tap_to_arrow")) config.default_interaction_settings.tap_to_arrow = *value;
        if (auto value = TomlBool(table, "encoder_to_arrow")) config.default_encoder_settings.to_arrow = *value;
        if (auto value = TomlBool(table, "encoder_rotation_invert")) config.default_encoder_settings.rotation_invert = *value;
        if (auto value = TomlString(table, "encoder_rotate_cw_key"); value && ParseKeySpec(*value).has_value()) config.default_encoder_settings.rotate_cw_key = *value;
        if (auto value = TomlString(table, "encoder_rotate_ccw_key"); value && ParseKeySpec(*value).has_value()) config.default_encoder_settings.rotate_ccw_key = *value;
        if (auto value = TomlInt(table, "encoder_rotate_fast_threshold"); value && *value > 0) config.default_encoder_settings.rotate_fast_threshold = static_cast<int>(*value);
        if (auto value = TomlString(table, "encoder_rotate_cw_fast_key"); value && ParseKeySpec(*value).has_value()) config.default_encoder_settings.rotate_cw_fast_key = *value;
        if (auto value = TomlString(table, "encoder_rotate_ccw_fast_key"); value && ParseKeySpec(*value).has_value()) config.default_encoder_settings.rotate_ccw_fast_key = *value;
        if (auto value = TomlInt(table, "encoder_rotate_decide_window_ms"); value && *value >= 0) config.default_encoder_settings.rotate_decide_window_ms = static_cast<int>(*value);
        if (auto value = TomlString(table, "encoder_led_color"); value && IsValidEncoderLedColor(*value)) config.default_encoder_settings.led_color = *value;
        if (auto value = TomlString(table, "encoder_press_action"); value && IsValidEncoderButtonAction(*value)) config.default_encoder_settings.press_action = *value;
        if (auto value = TomlString(table, "encoder_press_key"); value && (value->empty() || ParseKeySpec(*value).has_value())) config.default_encoder_settings.press_key = *value;
        if (auto value = TomlString(table, "encoder_double_click_action"); value && IsValidEncoderButtonAction(*value)) config.default_encoder_settings.double_click_action = *value;
        if (auto value = TomlString(table, "encoder_double_click_key"); value && ParseKeySpec(*value).has_value()) config.default_encoder_settings.double_click_key = *value;
        if (auto value = TomlInt(table, "tap_sensitivity")) config.default_interaction_settings.tap_sensitivity = TapSensitivityClamp(*value);
        if (auto value = TomlInt(table, "air_mouse_sensitivity_x")) config.default_interaction_settings.air_mouse_sensitivity_x = AirMouseSensitivityClamp(*value);
        if (auto value = TomlInt(table, "air_mouse_sensitivity_y")) config.default_interaction_settings.air_mouse_sensitivity_y = AirMouseSensitivityClamp(*value);
        if (auto value = TomlDouble(table, "air_mouse_tau")) config.air_mouse_tau = AirMouseTauClamp(*value);
        if (auto value = TomlBool(table, "air_mouse_invert_y")) config.air_mouse_invert_y = *value;
        if (auto value = TomlDouble(table, "air_mouse_curve_low_thresh")) config.air_mouse_curve_low_thresh = *value;
        if (auto value = TomlDouble(table, "air_mouse_curve_high_thresh")) config.air_mouse_curve_high_thresh = *value;
        if (auto value = TomlDouble(table, "air_mouse_curve_low_factor")) config.air_mouse_curve_low_factor = *value;
        if (auto value = TomlDouble(table, "air_mouse_curve_high_factor")) config.air_mouse_curve_high_factor = *value;
        if (auto value = TomlDouble(table, "air_mouse_neutral_deadzone")) config.air_mouse_neutral_deadzone = AirMouseNeutralDeadzoneClamp(*value);
        if (auto value = TomlString(table, "air_mouse_control_mode")) config.air_mouse_control_mode = AirMouseControlModeName(AirMouseControlModeFromName(*value));
        if (auto value = TomlDouble(table, "air_mouse_rate_gain")) config.air_mouse_rate_gain = AirMouseRateGainClamp(*value);
        if (auto value = TomlDouble(table, "air_mouse_rate_friction")) config.air_mouse_rate_friction = AirMouseRateFrictionClamp(*value);
        if (auto value = TomlDouble(table, "air_mouse_rate_max_speed")) config.air_mouse_rate_max_speed = AirMouseRateMaxSpeedClamp(*value);
        if (auto value = TomlBool(table, "launch_at_login")) config.launch_at_login = *value;
        if (auto value = TomlBool(table, "selection_hotword_enabled")) config.selection_hotword_enabled = *value;
        if (auto value = TomlBool(table, "debug_audio_cache")) config.debug_audio_cache = *value;
        if (auto value = TomlString(table, "debug_audio_dir"); value && !value->empty()) {
            config.debug_audio_directory = std::filesystem::path(Utf16FromUtf8(*value));
        }
        for (const auto& value : TomlStringArray(table, "paired_device")) {
            auto entry = ParsePairedDeviceEntry(value);
            if (!entry.device_id.empty()) config.paired_devices.push_back(entry);
        }
        if (MaybeRecoverTencentSecretId(config)) {
            config.Save(path);
        } else if (needs_wechat_trigger_migration_save) {
            config.Save(path);
        }
        return config;
    } catch (const toml::parse_error&) {
        input.clear();
        input.seekg(0);
        AppConfig legacy_config = LoadLegacyConfig(input);
        // legacy 扁平格式无 trigger_mode 字段：从顶层 interaction_mode 继承并重置为 hold。
        legacy_config.wechat_input_method.trigger_mode = legacy_config.interaction_mode;
        legacy_config.interaction_mode = InteractionMode::kHoldToTalk;
        if (MaybeRecoverTencentSecretId(legacy_config)) {
            legacy_config.Save(path);
        }
        return legacy_config;
    }
    return config;
}

void AppConfig::Save() const {
    Save(ConfigPath());
}

void AppConfig::SavePreservingDiskCredentials() const {
    SavePreservingDiskCredentials(ConfigPath());
}

void AppConfig::SavePreservingDiskCredentials(const std::filesystem::path& path) const {
    // 磁盘不存在则无凭据可保留，直接落盘。
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        Save(path);
        return;
    }
    // 重读磁盘最新凭据，拷贝到副本，再全量写回。副本凭据=磁盘值，非凭据=this 内存值。
    // 修复路径 A：程序运行时不重读 config.toml，运行时 Save 若用内存过期凭据会覆盖用户手改的 key。
    AppConfig disk = Load(path);
    AppConfig merged = *this;
    merged.voicestick_api_key = disk.voicestick_api_key;
    merged.voicestick_cloud_url = disk.voicestick_cloud_url;
    merged.volcengine_api_key = disk.volcengine_api_key;
    merged.volcengine_boosting_table_id = disk.volcengine_boosting_table_id;
    merged.volcengine_correct_table_id = disk.volcengine_correct_table_id;
    merged.tencent_secret_id = disk.tencent_secret_id;
    merged.tencent_secret_key = disk.tencent_secret_key;
    merged.tencent_appid = disk.tencent_appid;
    merged.tencent_engine_model_type = disk.tencent_engine_model_type;
    merged.tencent_hotword_id = disk.tencent_hotword_id;
    merged.llm_base_url = disk.llm_base_url;
    merged.llm_api_key = disk.llm_api_key;
    merged.llm_model = disk.llm_model;
    merged.refine_prompt = disk.refine_prompt;
    merged.hotword_process_prompt = disk.hotword_process_prompt;
    merged.Save(path);
}

void AppConfig::Save(const std::filesystem::path& path) const {
    // create_directories 用 error_code 版本：目录已存在或创建失败都不抛，
    // 避免路径无效时抛 filesystem_error（ofstream 后续会兜底报错）。
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
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
    output << "volcengine_boosting_table_id = \"" << TomlEscape(volcengine_boosting_table_id) << "\"\n";
    output << "volcengine_correct_table_id = \"" << TomlEscape(volcengine_correct_table_id) << "\"\n";
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
    output << "hotword_process_enabled = " << (hotword_process_enabled ? "true" : "false") << "\n";
    output << "hotword_mining_enabled = " << (hotword_mining_enabled ? "true" : "false") << "\n";
    output << "hotword_process_prompt = \"" << TomlEscape(hotword_process_prompt) << "\"\n";
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
    output << "show_imu_debug = " << (show_imu_debug ? "true" : "false") << "\n";
    output << "imu_wake_sensitivity = \"" << ImuWakeSensitivityName(default_interaction_settings.imu_wake_sensitivity) << "\"\n";
    output << "tap_to_arrow = " << (default_interaction_settings.tap_to_arrow ? "true" : "false") << "\n";
    output << "encoder_to_arrow = " << (default_encoder_settings.to_arrow ? "true" : "false") << "\n";
    output << "encoder_rotation_invert = " << (default_encoder_settings.rotation_invert ? "true" : "false") << "\n";
    output << "encoder_rotate_cw_key = \"" << TomlEscape(default_encoder_settings.rotate_cw_key) << "\"\n";
    output << "encoder_rotate_ccw_key = \"" << TomlEscape(default_encoder_settings.rotate_ccw_key) << "\"\n";
    output << "encoder_rotate_fast_threshold = " << default_encoder_settings.rotate_fast_threshold << "\n";
    output << "encoder_rotate_cw_fast_key = \"" << TomlEscape(default_encoder_settings.rotate_cw_fast_key) << "\"\n";
    output << "encoder_rotate_ccw_fast_key = \"" << TomlEscape(default_encoder_settings.rotate_ccw_fast_key) << "\"\n";
    output << "encoder_rotate_decide_window_ms = " << default_encoder_settings.rotate_decide_window_ms << "\n";
    output << "encoder_led_color = \"" << TomlEscape(default_encoder_settings.led_color) << "\"\n";
    output << "encoder_press_action = \"" << TomlEscape(default_encoder_settings.press_action) << "\"\n";
    output << "encoder_press_key = \"" << TomlEscape(default_encoder_settings.press_key) << "\"\n";
    output << "encoder_double_click_action = \"" << TomlEscape(default_encoder_settings.double_click_action) << "\"\n";
    output << "encoder_double_click_key = \"" << TomlEscape(default_encoder_settings.double_click_key) << "\"\n";
    output << "tap_sensitivity = " << default_interaction_settings.tap_sensitivity << "\n";
    output << "air_mouse_sensitivity_x = " << default_interaction_settings.air_mouse_sensitivity_x << "\n";
    output << "air_mouse_sensitivity_y = " << default_interaction_settings.air_mouse_sensitivity_y << "\n";
    output << "air_mouse_tau = " << air_mouse_tau << "\n";
    output << "air_mouse_invert_y = " << (air_mouse_invert_y ? "true" : "false") << "\n";
    output << "air_mouse_curve_low_thresh = " << air_mouse_curve_low_thresh << "\n";
    output << "air_mouse_curve_high_thresh = " << air_mouse_curve_high_thresh << "\n";
    output << "air_mouse_curve_low_factor = " << air_mouse_curve_low_factor << "\n";
    output << "air_mouse_curve_high_factor = " << air_mouse_curve_high_factor << "\n";
    output << "air_mouse_neutral_deadzone = " << air_mouse_neutral_deadzone << "\n";
    output << "air_mouse_control_mode = \"" << AirMouseControlModeName(AirMouseControlModeFromName(air_mouse_control_mode)) << "\"\n";
    output << "air_mouse_rate_gain = " << air_mouse_rate_gain << "\n";
    output << "air_mouse_rate_friction = " << air_mouse_rate_friction << "\n";
    output << "air_mouse_rate_max_speed = " << air_mouse_rate_max_speed << "\n";
    output << "launch_at_login = " << (launch_at_login ? "true" : "false") << "\n";
    output << "selection_hotword_enabled = " << (selection_hotword_enabled ? "true" : "false") << "\n";
    output << "debug_audio_cache = " << (debug_audio_cache ? "true" : "false") << "\n";
    output << "debug_audio_dir = \"" << TomlEscape(Utf8FromUtf16(debug_audio_directory.wstring())) << "\"\n";
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
    output << "\n[wechat_input_method]\n";
    output << "hotkey_hold = \"" << TomlEscape(wechat_input_method.hotkey_hold) << "\"\n";
    output << "hotkey_click = \"" << TomlEscape(wechat_input_method.hotkey_click) << "\"\n";
    output << "trigger_mode = \"" << InteractionModeName(wechat_input_method.trigger_mode) << "\"\n";
    output << "virtual_mic_playback_name = \"" << TomlEscape(wechat_input_method.virtual_mic_playback_name) << "\"\n";
    output << "virtual_mic_capture_name = \"" << TomlEscape(wechat_input_method.virtual_mic_capture_name) << "\"\n";
    output << "auto_switch_default_recording_device = "
           << (wechat_input_method.auto_switch_default_recording_device ? "true" : "false") << "\n";
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
    for (const auto& [device_id, settings] : device_encoder_settings) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        // 与全局默认相同则跳过，不落盘冗余覆盖。
        if (settings == default_encoder_settings) continue;
        // 覆盖表全量写出 13 个字段（键名去 encoder_ 前缀），保证表自含、加载顺序无关。
        output << "\n[device." << device_id << ".encoder]\n";
        output << "to_arrow = " << (settings.to_arrow ? "true" : "false") << "\n";
        output << "rotation_invert = " << (settings.rotation_invert ? "true" : "false") << "\n";
        output << "rotate_cw_key = \"" << TomlEscape(settings.rotate_cw_key) << "\"\n";
        output << "rotate_ccw_key = \"" << TomlEscape(settings.rotate_ccw_key) << "\"\n";
        output << "rotate_fast_threshold = " << settings.rotate_fast_threshold << "\n";
        output << "rotate_cw_fast_key = \"" << TomlEscape(settings.rotate_cw_fast_key) << "\"\n";
        output << "rotate_ccw_fast_key = \"" << TomlEscape(settings.rotate_ccw_fast_key) << "\"\n";
        output << "rotate_decide_window_ms = " << settings.rotate_decide_window_ms << "\n";
        output << "led_color = \"" << TomlEscape(settings.led_color) << "\"\n";
        output << "press_action = \"" << TomlEscape(settings.press_action) << "\"\n";
        output << "press_key = \"" << TomlEscape(settings.press_key) << "\"\n";
        output << "double_click_action = \"" << TomlEscape(settings.double_click_action) << "\"\n";
        output << "double_click_key = \"" << TomlEscape(settings.double_click_key) << "\"\n";
    }
    for (const auto& [device_id, settings] : device_interaction_settings) {
        if (std::find(paired_device_ids.begin(), paired_device_ids.end(), device_id) == paired_device_ids.end()) {
            continue;
        }
        // 与全局默认相同则跳过，不落盘冗余覆盖。
        if (settings == default_interaction_settings) continue;
        // 覆盖表全量写出 5 个字段（键名去前缀），保证表自含、加载顺序无关。
        output << "\n[device." << device_id << ".interaction]\n";
        output << "imu_wake_sensitivity = \"" << ImuWakeSensitivityName(settings.imu_wake_sensitivity) << "\"\n";
        output << "tap_to_arrow = " << (settings.tap_to_arrow ? "true" : "false") << "\n";
        output << "tap_sensitivity = " << settings.tap_sensitivity << "\n";
        output << "air_mouse_sensitivity_x = " << settings.air_mouse_sensitivity_x << "\n";
        output << "air_mouse_sensitivity_y = " << settings.air_mouse_sensitivity_y << "\n";
    }
}

std::string BuiltinApiKey() {
    // 编译期常量来自 builtin_secrets.h（CMake VOICESTICK_BUILTIN_* 注入）。
    // 公开构建为空字符串，回退不生效，正常读用户配置。
    return kBuiltinAsrApiKey;
}

std::string BuiltinTencentSecretId() { return kBuiltinTencentSecretId; }
std::string BuiltinTencentSecretKey() { return kBuiltinTencentSecretKey; }
std::string BuiltinTencentAppid() { return kBuiltinTencentAppid; }
std::string BuiltinLlmApiKey() { return kBuiltinLlmApiKey; }
std::string BuiltinLlmBaseUrl() { return kBuiltinLlmBaseUrl; }
std::string BuiltinLlmModel() { return kBuiltinLlmModel; }

// 通用回退：配置值优先，空则回退内置值。供腾讯云 SecretKey/AppId 与 LLM 字段复用。
std::string ResolveActiveString(std::string_view config_value, std::string_view builtin_value) {
    return !config_value.empty() ? std::string(config_value) : std::string(builtin_value);
}

std::string ResolveActiveApiKey(AsrProvider provider,
                                std::string_view voicestick_key,
                                std::string_view volcengine_key,
                                std::string_view tencent_id,
                                std::string_view builtin_volcengine_key,
                                std::string_view builtin_tencent_id) {
    switch (provider) {
        case AsrProvider::kVoiceStickCloud: return std::string(voicestick_key);
        case AsrProvider::kVolcengine:
            // 配置文件 key 优先；空则回退编译期内置 key（预配置 MSI 分发场景）。
            return !volcengine_key.empty() ? std::string(volcengine_key) : std::string(builtin_volcengine_key);
        case AsrProvider::kTencent:
            // 配置文件 secret_id 优先；空则回退编译期内置 tencent_secret_id。
            return !tencent_id.empty() ? std::string(tencent_id) : std::string(builtin_tencent_id);
    }
    return {};
}

std::string AppConfig::ActiveApiKey() const {
    return ResolveActiveApiKey(asr_provider, voicestick_api_key, volcengine_api_key,
                               tencent_secret_id, BuiltinApiKey(), BuiltinTencentSecretId());
}

std::string AppConfig::ActiveTencentSecretId() const {
    return ResolveActiveString(tencent_secret_id, BuiltinTencentSecretId());
}
std::string AppConfig::ActiveTencentSecretKey() const {
    return ResolveActiveString(tencent_secret_key, BuiltinTencentSecretKey());
}
std::string AppConfig::ActiveTencentAppid() const {
    return ResolveActiveString(tencent_appid, BuiltinTencentAppid());
}
std::string AppConfig::ActiveLlmApiKey() const {
    return ResolveActiveString(llm_api_key, BuiltinLlmApiKey());
}
std::string AppConfig::ActiveLlmBaseUrl() const {
    return ResolveActiveString(llm_base_url, BuiltinLlmBaseUrl());
}
std::string AppConfig::ActiveLlmModel() const {
    return ResolveActiveString(llm_model, BuiltinLlmModel());
}

bool NeedsAsrStep(const AppConfig& config) {
    // 内置 key（ActiveApiKey 非空）时向导跳过 kAsr 步；公开版无 key 仍需用户填写。
    return config.ActiveApiKey().empty();
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
    device_encoder_settings.erase(device_id);
    device_interaction_settings.erase(device_id);
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

const EncoderSettings& AppConfig::EncoderSettingsForDevice(
    const std::optional<std::string>& device_id) const {
    if (!device_id.has_value()) return default_encoder_settings;
    const auto normalized = BleProtocol::NormalizeDeviceId(*device_id);
    auto it = device_encoder_settings.find(normalized);
    if (it == device_encoder_settings.end()) return default_encoder_settings;
    // 覆盖表加载时已用全局默认填平所有字段，整表返回即可。
    return it->second;
}

const InteractionSettings& AppConfig::InteractionSettingsForDevice(
    const std::optional<std::string>& device_id) const {
    if (!device_id.has_value()) return default_interaction_settings;
    const auto normalized = BleProtocol::NormalizeDeviceId(*device_id);
    auto it = device_interaction_settings.find(normalized);
    if (it == device_interaction_settings.end()) return default_interaction_settings;
    // 覆盖表加载时已用全局默认填平所有字段，整表返回即可。
    return it->second;
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
    switch (mode) {
    case InteractionMode::kClickToTalk:
        return "click_to_talk";
    case InteractionMode::kHoldToTalkInstant:
        return "hold_to_talk_instant";
    case InteractionMode::kHoldToTalk:
    default:
        return "hold_to_talk";
    }
}

InteractionMode InteractionModeFromName(std::string_view name) {
    // 不解析 hold_to_talk_instant：该值仅运行期派生，配置文件只存用户可见的两值。
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
    switch (target) {
        case OutputTarget::kSubtitle: return "subtitle";
        case OutputTarget::kWechatInputMethod: return "wechat_input_method";
        case OutputTarget::kFocusedApp: return "focused_app";
    }
    return "focused_app";
}

OutputTarget OutputTargetFromName(std::string_view name) {
    if (name == "subtitle") return OutputTarget::kSubtitle;
    if (name == "wechat_input_method") return OutputTarget::kWechatInputMethod;
    return OutputTarget::kFocusedApp;
}

std::string OutputTargetDisplayName(OutputTarget target) {
    switch (target) {
        case OutputTarget::kSubtitle: return "Subtitle";
        case OutputTarget::kWechatInputMethod: return "Third-party Input Method";
        case OutputTarget::kFocusedApp: return "Focused App";
    }
    return "Focused App";
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

int TapSensitivityClamp(int level) {
    if (level < 1 || level > 10) return 5;
    return level;
}

int AirMouseSensitivityClamp(int level) {
    // 灵敏度档位 1~10，越界回落默认 5。
    if (level < 1 || level > 10) return 5;
    return level;
}

double AirMouseTauClamp(double tau) {
    // 速度环时间常数约束在 [0.02, 0.5]，越界回落默认 0.05（手停即停）。
    if (!(tau > 0.0) || tau > 0.5 || tau < 0.02) return 0.05;
    return tau;
}

double AirMouseNeutralDeadzoneClamp(double deadzone) {
    // 方向锁中立区死区约束在 [1.0, 10.0]，越界回落默认 3.0。
    if (!(deadzone > 0.0) || deadzone < 1.0 || deadzone > 10.0) return 3.0;
    return deadzone;
}

double AirMouseRateGainClamp(double gain) {
    if (!(gain > 0.0) || gain < 10.0 || gain > 500.0) return 80.0;
    return gain;
}

double AirMouseRateFrictionClamp(double friction) {
    if (!(friction >= 0.0) || friction < 0.0 || friction > 0.5) return 0.05;
    return friction;
}

double AirMouseRateMaxSpeedClamp(double max_speed) {
    if (!(max_speed > 0.0) || max_speed < 500.0 || max_speed > 8000.0) return 4000.0;
    return max_speed;
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
