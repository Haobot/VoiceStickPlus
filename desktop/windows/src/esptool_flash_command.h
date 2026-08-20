#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace voicestick {

// 烧录模式。
enum class FlashMode {
    kFullMerged,    // 整包烧录：write_flash @ 0x0
    kAppOnly,       // 仅应用分区：write_flash @ 0x10000
    kEraseThenFull,  // 先完全擦除再整包：erase_flash + write_flash @ 0x0（两步命令）
};

// 烧录参数。
struct FlashOptions {
    std::wstring serial_port;                  // 串口名，如 L"COM5"
    std::filesystem::path firmware_path;       // 固件路径（含中文，宽字符）
    int baud = 460800;                         // 波特率
    FlashMode mode = FlashMode::kFullMerged;   // 烧录模式
};

// 构造 esptool 命令序列。kFullMerged/kAppOnly 返回 1 条命令，
// kEraseThenFull 返回 2 条（erase_flash 在前，write_flash 在后）。
// 每条命令形如：
//   [python_exe, L"-m", L"esptool", L"--chip", L"esp32s3",
//    L"--port", port, L"--baud", baud, L"--before", L"default_reset",
//    L"--after", L"no_reset", ... 模式相关参数 ...]
std::vector<std::vector<std::wstring>> BuildEsptoolCommandSequence(
    const FlashOptions& options, const std::filesystem::path& python_exe);

// 把 argv 拼接为命令行字符串。含空格或非 ASCII 字符的参数加引号。
std::wstring JoinCommandLine(const std::vector<std::wstring>& argv);

} // namespace voicestick
