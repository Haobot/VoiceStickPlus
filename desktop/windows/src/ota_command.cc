#include "ota_command.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>

namespace voicestick {

namespace {

bool IsHexString(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool IsSha256Hex(std::string_view text) {
    return text.size() == 64 && IsHexString(text);
}

bool StartsWithInsensitive(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string Trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return std::string(text.substr(first, last - first));
}

std::string JsonString(const cJSON* root, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return {};
    return item->valuestring;
}

int JsonInt(const cJSON* root, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) return fallback;
    return item->valueint;
}

bool JsonBool(const cJSON* root, const char* key, bool fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

void AddString(cJSON* root, const char* key, const std::string& value) {
    cJSON_AddStringToObject(root, key, value.c_str());
}

void SetError(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

} // namespace

bool ShouldCompleteOtaHealthy(const OtaHealthyDecisionInput& input) {
    return input.saw_success &&
           input.saw_disconnect_after_success &&
           input.saw_reconnect_after_success &&
           input.wifi_status_after_reconnect &&
           !input.ota_pending_verify;
}

bool ShouldSendOtaCommit(const OtaHealthyDecisionInput& input, bool commit_sent) {
    return input.saw_success &&
           input.saw_disconnect_after_success &&
           input.saw_reconnect_after_success &&
           input.wifi_status_after_reconnect &&
           input.ota_pending_verify &&
           !commit_sent;
}

std::string OtaWaitModeName(OtaWaitMode mode) {
    switch (mode) {
    case OtaWaitMode::kSuccess: return "success";
    case OtaWaitMode::kHealthy: return "healthy";
    }
    return "healthy";
}

OtaWaitMode OtaWaitModeFromName(std::string_view name) {
    return name == "success" ? OtaWaitMode::kSuccess : OtaWaitMode::kHealthy;
}

std::string NormalizeOtaDeviceId(std::string_view device_id) {
    std::string out = Trim(device_id);
    if (StartsWithInsensitive(out, "VS-")) out.erase(0, 3);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

std::optional<OtaPullCommand> ParseOtaCommandLine(const std::vector<std::string>& args,
                                                  std::string* error) {
    if (args.empty() || args[0] != "ota-pull") {
        SetError(error, "仅支持 ota-pull 命令");
        return std::nullopt;
    }

    OtaPullCommand command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto require_value = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                SetError(error, std::string(name) + " 缺少参数值");
                return std::nullopt;
            }
            return args[++i];
        };

        if (arg == "--device") {
            auto value = require_value("--device");
            if (!value) return std::nullopt;
            command.device_id = NormalizeOtaDeviceId(*value);
        } else if (arg == "--url") {
            auto value = require_value("--url");
            if (!value) return std::nullopt;
            command.url = Trim(*value);
        } else if (arg == "--sha256") {
            auto value = require_value("--sha256");
            if (!value) return std::nullopt;
            command.sha256_hex = Trim(*value);
        } else if (arg == "--wait") {
            auto value = require_value("--wait");
            if (!value) return std::nullopt;
            if (*value != "success" && *value != "healthy") {
                SetError(error, "--wait 只能是 success 或 healthy");
                return std::nullopt;
            }
            command.wait_mode = OtaWaitModeFromName(*value);
        } else if (arg == "--timeout") {
            auto value = require_value("--timeout");
            if (!value) return std::nullopt;
            command.timeout_sec = std::max(1, std::atoi(value->c_str()));
        } else if (arg == "--json") {
            command.json_output = true;
        } else if (arg == "--save-config") {
            command.save_config = true;
        } else {
            SetError(error, "未知参数: " + arg);
            return std::nullopt;
        }
    }
    if (error) error->clear();
    return command;
}

bool ResolveOtaPullCommandFromConfig(const AppConfig& config,
                                     OtaPullCommand* command,
                                     std::string* error) {
    if (!command) {
        SetError(error, "内部错误：command 为空");
        return false;
    }
    if (command->device_id.empty()) {
        if (config.paired_device_ids.size() == 1) {
            command->device_id = NormalizeOtaDeviceId(config.paired_device_ids.front());
        } else {
            SetError(error, "存在多个或零个配对设备，请用 --device 指定目标设备");
            return false;
        }
    }

    auto it = config.device_wifi_profiles.find(command->device_id);
    if (it != config.device_wifi_profiles.end()) {
        if (command->url.empty()) command->url = it->second.ota_url;
        if (command->sha256_hex.empty()) command->sha256_hex = it->second.ota_sha256_hex;
    }

    return ValidateOtaPullCommand(*command, error);
}

bool ValidateOtaPullCommand(const OtaPullCommand& command, std::string* error) {
    if (command.device_id.empty()) {
        SetError(error, "缺少设备 ID");
        return false;
    }
    if (command.url.empty()) {
        SetError(error, "缺少 OTA URL");
        return false;
    }
    const bool is_http = StartsWithInsensitive(command.url, "http://");
    const bool is_https = StartsWithInsensitive(command.url, "https://");
    if (!is_http && !is_https) {
        SetError(error, "OTA URL 必须以 http:// 或 https:// 开头");
        return false;
    }
    if (!command.sha256_hex.empty() && !IsSha256Hex(command.sha256_hex)) {
        SetError(error, "sha256 必须是 64 位十六进制字符串");
        return false;
    }
    if (is_http && command.sha256_hex.empty()) {
        SetError(error, "http OTA 必须提供 sha256");
        return false;
    }
    if (error) error->clear();
    return true;
}

std::string SerializeOtaIpcRequest(const OtaPullCommand& command) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "ota_pull");
    AddString(root, "request_id", command.request_id);
    AddString(root, "reply_pipe", command.reply_pipe);
    AddString(root, "device_id", command.device_id);
    AddString(root, "url", command.url);
    AddString(root, "sha256_hex", command.sha256_hex);
    AddString(root, "wait", OtaWaitModeName(command.wait_mode));
    cJSON_AddNumberToObject(root, "timeout_sec", command.timeout_sec);
    cJSON_AddBoolToObject(root, "json", command.json_output);
    cJSON_AddBoolToObject(root, "save_config", command.save_config);
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

std::optional<OtaPullCommand> ParseOtaIpcRequest(std::string_view json, std::string* error) {
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) {
        SetError(error, "IPC JSON 解析失败");
        return std::nullopt;
    }
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> guard(root, cJSON_Delete);
    if (JsonString(root, "action") != "ota_pull") {
        SetError(error, "IPC action 不是 ota_pull");
        return std::nullopt;
    }
    OtaPullCommand command;
    command.request_id = JsonString(root, "request_id");
    command.reply_pipe = JsonString(root, "reply_pipe");
    command.device_id = NormalizeOtaDeviceId(JsonString(root, "device_id"));
    command.url = JsonString(root, "url");
    command.sha256_hex = JsonString(root, "sha256_hex");
    command.wait_mode = OtaWaitModeFromName(JsonString(root, "wait"));
    command.timeout_sec = JsonInt(root, "timeout_sec", 180);
    command.json_output = JsonBool(root, "json", false);
    command.save_config = JsonBool(root, "save_config", false);
    if (error) error->clear();
    return command;
}

} // namespace voicestick
