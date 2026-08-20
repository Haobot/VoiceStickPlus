#pragma once

#include <functional>
#include <string>

namespace voicestick {

// 烧录事件（解析 esptool 输出或工具状态变更产生）。
struct FlashEvent {
    enum Kind {
        kLogLine,   // 普通日志行
        kStage,     // 阶段切换
        kProgress,  // 进度百分比
        kError,     // 错误
        kFinished,  // 烧录结束（成功/失败/取消）
    };
    Kind kind = kLogLine;
    std::wstring text;
    int percent = 0;
    bool success = false;     // kFinished 用
    bool cancelled = false;   // kFinished 用
};

// 逐行解析 esptool stdout/stderr，映射为 FlashEvent。
// 阶段：连接中 / 检测芯片 / 写入 / 擦除 / 校验
// 进度格式：
//   4.x: "Writing at 0x... (X %)"
//   5.x: "Writing at 0x... [====>  ] X.X% N/M bytes"
// 错误：以 "A fatal error occurred:" 开头的行
// 黑名单行（不含进度百分比事件）：含 Eras/Verif/Hash/Compress/Check/CRC/Leaving/Reset/Connecting
class EsptoolProgressParser {
public:
    explicit EsptoolProgressParser(std::function<void(const FlashEvent&)> callback);
    void FeedLine(const std::string& line);

private:
    std::function<void(const FlashEvent&)> callback_;
    bool stage_connecting_sent_ = false;
    bool stage_detecting_sent_ = false;
    bool stage_writing_sent_ = false;
    bool stage_erasing_sent_ = false;
    bool stage_verifying_sent_ = false;
};

} // namespace voicestick
