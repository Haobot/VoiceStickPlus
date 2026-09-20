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

// 把免引号简写展开成控制帧 JSON：`<event>:<k>=<v>[,<k>=<v>...]`。
// 为什么需要：PowerShell/cmd 把参数里的双引号吞掉（实测 `--control '{"event":...}'` 到进程时
// 变成 `{event:...}`，不是合法 JSON），所以提供一个不需要引号的写法供脚本/调试使用。
// 取值规则：true/false → 布尔；纯十进制数字 → 整数；其余 → 字符串。
std::string ExpandControlShorthand(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0) return {};
    const std::string event = text.substr(0, colon);
    std::string json = "{\"event\":\"" + event + "\"";
    std::string rest = text.substr(colon + 1);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string pair = rest.substr(0, comma);
        if (comma == std::string::npos) {
            rest.clear();
        } else {
            rest = rest.substr(comma + 1);
        }
        if (pair.empty()) continue;
        const auto eq = pair.find('=');
        if (eq == std::string::npos || eq == 0) return {};  // 缺 key/value，视为非法
        const std::string key = pair.substr(0, eq);
        const std::string value = pair.substr(eq + 1);
        json += ",\"" + key + "\":";
        if (value == "true" || value == "false") {
            json += value;
        } else if (!value.empty() &&
                   value.find_first_not_of("0123456789") == std::string::npos) {
            json += value;
        } else {
            json += "\"" + value + "\"";
        }
    }
    json += "}";
    return json;
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

std::optional<GatewayTargetCliRequest> ParseGatewayTargetCliArgs(int argc,
                                                                 const wchar_t* const argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring token = argv[i];
        if (token != L"--gateway-target") continue;
        if (i + 1 >= argc) return std::nullopt;  // 缺取值
        const std::wstring value = argv[i + 1];
        if (value == L"self") return GatewayTargetCliRequest{true};
        if (value == L"clear") return GatewayTargetCliRequest{false};
        return std::nullopt;  // 取值非法（只接受 self/clear）
    }
    return std::nullopt;
}

std::optional<ControlCliRequest> ParseControlCliArgs(int argc, const wchar_t* const argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring token = argv[i];
        if (token != L"--control") continue;
        if (i + 1 >= argc) return std::nullopt;  // 缺取值
        auto json = WideToUtf8(argv[i + 1]);
        if (json.empty()) return std::nullopt;
        if (json[0] != '{') {
            // 免引号简写（PowerShell 会吞双引号）：gateway_menu:action=open
            json = ExpandControlShorthand(json);
            if (json.empty()) return std::nullopt;
        }
        return ControlCliRequest{std::move(json)};
    }
    return std::nullopt;
}

} // namespace voicestick
