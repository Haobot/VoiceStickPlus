#pragma once

#include <optional>
#include <string>

namespace voicestick {

// 命令行触发的 BLE OTA 请求。
struct OtaCliRequest {
    std::string file_path;                   // 本地固件 bin 路径（UTF-8）
    std::optional<std::string> device_id;    // 可选目标设备 ID（4 位十六进制）
};

// 解析命令行 argv 中的 --ota <path> [--device <id>]。
// argv[0] 视为程序名，从 argv[1] 起扫描；--ota 与 --device 顺序无关。
// 无 --ota 或 --ota 缺路径时返回 nullopt；--device 缺值时忽略该选项。
std::optional<OtaCliRequest> ParseOtaCliArgs(int argc, const wchar_t* const argv[]);

// 虚拟麦克风改名请求（提权子进程执行，见 virtual_mic_renamer.h）。
struct RenameVirtualMicCliRequest {
    std::wstring endpoint_id;  // 录音设备 endpoint id（"{0.0.1.00000000}.{guid}"）
    std::wstring new_name;     // 新设备显示名（如 "VS-091C"；可能含空格，宽字符原样传递）
};

// 解析命令行 argv 中的 --rename-virtual-mic <endpoint-id> <name...>。
// argv[0] 视为程序名；name 取该选项后剩余全部参数以空格连接（支持含空格的名字，
// 调用方需整体加引号）。无该选项或缺任一参数返回 nullopt。
std::optional<RenameVirtualMicCliRequest> ParseRenameVirtualMicArgs(
    int argc, const wchar_t* const argv[]);

}  // namespace voicestick
