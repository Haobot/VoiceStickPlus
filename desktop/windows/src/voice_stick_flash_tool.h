#pragma once

#include "esptool_flash_command.h"
#include "esptool_progress.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// esptool 子进程运行器接口（真实实现用 CreateProcess；测试注入 FakeFlashRunner）。
class IFlashProcessRunner {
public:
    virtual ~IFlashProcessRunner() = default;
    // 运行子进程，逐行回调 stdout/stderr。返回退出码。
    virtual int Run(const std::vector<std::wstring>& argv,
                    const std::function<void(const std::string& line)>& on_line) = 0;
    // 取消正在运行的子进程（TerminateProcess）。
    virtual void Cancel() = 0;
};

// 子进程被取消时返回的特殊退出码。
inline constexpr int kFlashCancelledExitCode = 0x1000;

// 烧录编排器：构造命令 → 运行子进程 → 解析进度 → 事件回调。
class FlashTool {
public:
    FlashTool(FlashOptions options,
              std::filesystem::path python_exe,
              IFlashProcessRunner* runner,
              std::function<void(const FlashEvent&)> on_event);

    // 运行烧录流程。成功返回 true，失败/取消返回 false。
    bool Run();
    // 取消正在进行的烧录。
    void Cancel();

private:
    FlashOptions options_;
    std::filesystem::path python_exe_;
    IFlashProcessRunner* runner_;
    std::function<void(const FlashEvent&)> on_event_;
};

} // namespace voicestick
