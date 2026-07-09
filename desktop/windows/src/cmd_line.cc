#include "cmd_line.h"

#include <Windows.h>

#include <string>

namespace voicestick {

namespace {

// 宽字符转 UTF-8。WideCharToMultiByte 失败返回空串。
std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

std::optional<OtaCliRequest> ParseOtaCliArgs(int argc, const wchar_t* const argv[]) {
    std::optional<std::string> file_path;
    std::optional<std::string> device_id;

    for (int i = 1; i < argc; ++i) {
        const std::wstring token = argv[i];
        if (token == L"--ota") {
            if (i + 1 >= argc) return std::nullopt;  // --ota 缺路径
            file_path = WideToUtf8(argv[++i]);
            if (file_path->empty()) return std::nullopt;
        } else if (token == L"--device") {
            if (i + 1 >= argc) continue;  // --device 缺值，忽略
            device_id = WideToUtf8(argv[++i]);
        }
        // 其他参数（含 --relaunch）忽略，不影响 --ota 解析。
    }

    if (!file_path.has_value()) return std::nullopt;
    return OtaCliRequest{std::move(*file_path), std::move(device_id)};
}

} // namespace voicestick
