#include "voice_stick_flash_tool.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace voicestick {

namespace {

// 大小写不敏感比较扩展名。
bool HasBinExtension(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".bin";
}

void EmitFinished(std::function<void(const FlashEvent&)> on_event,
                  bool success, bool cancelled) {
    FlashEvent e;
    e.kind = FlashEvent::kFinished;
    e.text = success ? L"烧录完成" : (cancelled ? L"已取消" : L"烧录失败");
    e.success = success;
    e.cancelled = cancelled;
    if (on_event) on_event(e);
}

} // namespace

FlashTool::FlashTool(FlashOptions options,
                     std::filesystem::path python_exe,
                     IFlashProcessRunner* runner,
                     std::function<void(const FlashEvent&)> on_event)
    : options_(std::move(options)),
      python_exe_(std::move(python_exe)),
      runner_(runner),
      on_event_(std::move(on_event)) {}

bool FlashTool::Run() {
    // 校验：串口非空。
    if (options_.serial_port.empty()) {
        EmitFinished(on_event_, false, false);
        return false;
    }
    // 校验：固件后缀 .bin。
    if (!HasBinExtension(options_.firmware_path)) {
        EmitFinished(on_event_, false, false);
        return false;
    }
    // 校验：固件文件存在。
    std::error_code ec;
    if (!std::filesystem::exists(options_.firmware_path, ec)) {
        EmitFinished(on_event_, false, false);
        return false;
    }

    // 构造命令序列。
    const auto commands = BuildEsptoolCommandSequence(options_, python_exe_);

    // 逐条运行。
    for (const auto& argv : commands) {
        EsptoolProgressParser parser(on_event_);
        const int exit_code = runner_->Run(argv, [&](const std::string& line) {
            parser.FeedLine(line);
        });

        if (exit_code == 0) continue;  // 成功，继续下一条

        if (exit_code == kFlashCancelledExitCode) {
            // 用户取消。
            EmitFinished(on_event_, false, true);
            return false;
        }

        // 失败：发错误事件 + 结束事件。
        FlashEvent err;
        err.kind = FlashEvent::kError;
        err.text = L"esptool 退出码 " + std::to_wstring(exit_code);
        if (on_event_) on_event_(err);
        EmitFinished(on_event_, false, false);
        return false;
    }

    // 全部命令成功。
    EmitFinished(on_event_, true, false);
    return true;
}

void FlashTool::Cancel() {
    if (runner_) runner_->Cancel();
}

} // namespace voicestick
