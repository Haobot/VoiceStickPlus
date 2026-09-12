#include "cmd_line.h"

#include <Windows.h>

#include <cwchar>
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

std::optional<RenameVirtualMicCliRequest> ParseRenameVirtualMicArgs(
    int argc, const wchar_t* const argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--rename-virtual-mic") != 0) continue;
        if (i + 2 >= argc) return std::nullopt;  // 缺 endpoint-id 或 name
        RenameVirtualMicCliRequest request;
        request.endpoint_id = argv[i + 1];
        // name 取该选项后剩余全部参数以空格连接：调用方整体加引号时
        // CommandLineToArgvW 已拆成多个 argv，这里拼回原名（支持含空格的名字）。
        for (int j = i + 2; j < argc; ++j) {
            if (j > i + 2) request.new_name += L' ';
            request.new_name += argv[j];
        }
        if (request.endpoint_id.empty() || request.new_name.empty()) return std::nullopt;
        return request;
    }
    return std::nullopt;
}

}  // namespace voicestick
