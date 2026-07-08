// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "device_switch_state.h"

#include <cJSON.h>
#include <windows.h>

#include <fstream>
#include <optional>
#include <sstream>

namespace voicestick {

std::string WStringToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWString(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

namespace {

// 读文件全文；文件不存在或无法打开返回 nullopt。
std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

}  // namespace

bool LoadDeviceSwitchState(const std::filesystem::path& path, DeviceSwitchState& out) {
    // 先清空：文件不存在也返回未切换状态。
    out = {};
    const auto text = ReadFile(path);
    if (!text) return true;  // 文件不存在视为未切换。
    cJSON* root = cJSON_Parse(text->c_str());
    if (root == nullptr) return false;
    const cJSON* sw = cJSON_GetObjectItemCaseSensitive(root, "switched");
    if (sw != nullptr && cJSON_IsBool(sw)) {
        out.switched = cJSON_IsTrue(sw);
    }
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "saved_default_capture_id");
    if (id != nullptr && cJSON_IsString(id) && id->valuestring != nullptr) {
        out.saved_default_capture_id = id->valuestring;
    }
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(root, "saved_default_capture_name");
    if (name != nullptr && cJSON_IsString(name) && name->valuestring != nullptr) {
        out.saved_default_capture_name = name->valuestring;
    }
    cJSON_Delete(root);
    return true;
}

bool SaveDeviceSwitchState(const std::filesystem::path& path, const DeviceSwitchState& state) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return false;
    cJSON_AddBoolToObject(root, "switched", state.switched);
    cJSON_AddStringToObject(root, "saved_default_capture_id",
                            state.saved_default_capture_id.c_str());
    cJSON_AddStringToObject(root, "saved_default_capture_name",
                            state.saved_default_capture_name.c_str());
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == nullptr) return false;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        cJSON_free(text);
        return false;
    }
    ofs << text;
    ofs.flush();
    cJSON_free(text);
    return ofs.good();
}

bool ClearDeviceSwitchState(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return true;
    return std::filesystem::remove(path);
}

}  // namespace voicestick
