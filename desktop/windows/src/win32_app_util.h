#pragma once
// N7 cut3：win32_app.cc 拆分共享层——尾段（设备偏好/覆盖层/托盘域）方法所用
// 的文件级助手迁入本头（匿名 ns 每 TU 内部链接零 ODR）；preamble 逐字沿用原文件。
#include "win32_app.h"

#include "asr_client_win.h"
#include "asr_client_tencent.h"
#include "ble_central_win.h"
#include "hotword_extractor.h"
#include "hotword_selector.h"  // ValidateHotword（B14 统一口径）
#include "license_runtime.h"
#include "local_asr_client_win.h"
#include "machine_guid_win.h"
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#include "llama_cpp_engine.h"
#include "local_refinement_client.h"
#endif
#include "localization.h"
#include "log.h"
#include "mic_mode_hotkey.h"
#include "push_to_talk_key.h"
#include "resource.h"
#include "wasapi_mic_capture.h"

#include <Shellapi.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <winsparkle.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace voicestick {
namespace {
constexpr UINT kTrayIconId = 1;

void LogLine(std::string_view message) {
    voicestick::LogApp(message);
}

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string FormatUtf8(std::string text, std::initializer_list<std::string> values) {
    for (const auto& value : values) {
        const auto pos = text.find("%s");
        if (pos == std::string::npos) break;
        text.replace(pos, 2, value);
    }
    return text;
}

// 拉起 VoiceStickFlash.exe（COM 口固件烧录工具）：与 VoiceStick.exe 同级目录，
// 开发构建在 build-x64，MSI 安装在 INSTALLFOLDER。未安装时给出明确提示。
void LaunchFlashToolExe(HWND owner) {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) return;
    path.resize(length);
    const auto flash_exe = std::filesystem::path(path).parent_path() / L"VoiceStickFlash.exe";
    std::error_code ec;
    if (!std::filesystem::exists(flash_exe, ec)) {
        MessageBoxW(owner,
                    L"未找到 VoiceStickFlash.exe（应与 VoiceStick.exe 同目录）。\n"
                    L"MSI 安装版自带该工具；便携版暂不包含。",
                    L"VoiceStick", MB_OK | MB_ICONWARNING);
        return;
    }
    ShellExecuteW(owner, L"open", flash_exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}
} // namespace voicestick
