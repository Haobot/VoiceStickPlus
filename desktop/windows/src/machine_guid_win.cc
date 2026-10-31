#include "machine_guid_win.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>

namespace voicestick {

std::optional<std::string> ReadMachineGuid() {
    wchar_t buffer[64] = {};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                     L"MachineGuid", RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    // bytes 含终止 NUL：构造后统一 trim 尾部 NUL 与空白。
    std::wstring wide(buffer, bytes / sizeof(wchar_t));
    while (!wide.empty() &&
           (wide.back() == L'\0' || iswspace(wide.back()) != 0)) {
        wide.pop_back();
    }
    if (wide.empty()) return std::nullopt;
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::nullopt;
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), length, nullptr, nullptr);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace voicestick
