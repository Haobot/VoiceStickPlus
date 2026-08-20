#include "esptool_flash_command.h"

#include <algorithm>
#include <string>

namespace voicestick {

namespace {

// 构造 esptool 公共参数前缀（不含模式相关部分）。
std::vector<std::wstring> BuildCommonArgs(const FlashOptions& options,
                                          const std::filesystem::path& python_exe) {
    return {
        python_exe.wstring(),
        L"-m",
        L"esptool",
        L"--chip",
        L"esp32s3",
        L"--port",
        options.serial_port,
        L"--baud",
        std::to_wstring(options.baud),
        L"--before",
        L"default_reset",
        L"--after",
        L"no_reset",  // 本板 hard_reset 无效，烧完提示手动重启
    };
}

} // namespace

std::vector<std::vector<std::wstring>> BuildEsptoolCommandSequence(
    const FlashOptions& options, const std::filesystem::path& python_exe) {
    std::vector<std::vector<std::wstring>> commands;
    const auto common = BuildCommonArgs(options, python_exe);

    switch (options.mode) {
        case FlashMode::kFullMerged: {
            auto cmd = common;
            cmd.push_back(L"write_flash");
            cmd.push_back(L"0x0");
            cmd.push_back(options.firmware_path.wstring());
            commands.push_back(std::move(cmd));
            break;
        }
        case FlashMode::kAppOnly: {
            auto cmd = common;
            cmd.push_back(L"write_flash");
            cmd.push_back(L"0x10000");
            cmd.push_back(options.firmware_path.wstring());
            commands.push_back(std::move(cmd));
            break;
        }
        case FlashMode::kEraseThenFull: {
            // 第一步：完全擦除。
            auto erase_cmd = common;
            erase_cmd.push_back(L"erase_flash");
            commands.push_back(std::move(erase_cmd));

            // 第二步：整包烧录。
            auto write_cmd = common;
            write_cmd.push_back(L"write_flash");
            write_cmd.push_back(L"0x0");
            write_cmd.push_back(options.firmware_path.wstring());
            commands.push_back(std::move(write_cmd));
            break;
        }
    }

    return commands;
}

std::wstring JoinCommandLine(const std::vector<std::wstring>& argv) {
    std::wstring result;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) result.push_back(L' ');
        const std::wstring& arg = argv[i];

        // 含空格或非 ASCII 字符的参数需要加引号。
        bool need_quote = false;
        for (wchar_t c : arg) {
            if (c == L' ' || static_cast<unsigned>(c) > 127) {
                need_quote = true;
                break;
            }
        }

        if (need_quote) {
            result.push_back(L'"');
            result.append(arg);
            result.push_back(L'"');
        } else {
            result.append(arg);
        }
    }
    return result;
}

} // namespace voicestick
