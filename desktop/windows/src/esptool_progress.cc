#include "esptool_progress.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace voicestick {

namespace {

// UTF-8 字符串转宽字符（用于事件 text）。
std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    std::wstring wide;
    for (std::size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            wide.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            wide.push_back(static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(utf8[i + 1]) & 0x3F)));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            wide.push_back(static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(utf8[i + 2]) & 0x3F)));
            i += 3;
        } else {
            wide.push_back(static_cast<wchar_t>(c));
            ++i;
        }
    }
    return wide;
}

// 大小写不敏感子串查找。
bool ContainsCI(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

// 黑名单关键字（含这些词的行不产生进度事件，与 scripts/idf_cli.py 一致）。
bool IsBlacklisted(std::string_view line) {
    static const char* kKeywords[] = {
        "Eras", "Verif", "Hash", "Compress",
        "Check", "CRC", "Leaving", "Reset", "Connecting",
    };
    for (const char* kw : kKeywords) {
        if (ContainsCI(line, kw)) return true;
    }
    return false;
}

// 尝试解析 4.x 进度格式 "(X %)" → 返回百分比，失败返回 -1。
int ParseProgressV4(std::string_view line) {
    // 查找 '(' 后面的数字和 '%'。
    for (std::size_t i = 0; i + 2 < line.size(); ++i) {
        if (line[i] == '(') {
            // 跳过空格
            std::size_t j = i + 1;
            while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            // 解析整数
            int num = 0;
            bool any = false;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                num = num * 10 + (line[j] - '0');
                any = true;
                ++j;
            }
            if (!any) continue;
            // 跳过空格
            while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            // 期望 '%'
            if (j < line.size() && line[j] == '%') return num;
        }
    }
    return -1;
}

// 尝试解析 5.x 进度格式 "X.X% N/M bytes" → 返回四舍五入后的整数百分比，失败返回 -1。
int ParseProgressV5(std::string_view line) {
    // 查找 "X.X% ... N/M bytes" 模式。
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        // 找到一个数字起始
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) continue;

        // 解析整数部分
        std::size_t j = i;
        int int_part = 0;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
            int_part = int_part * 10 + (line[j] - '0');
            ++j;
        }
        // 可选小数部分
        double value = static_cast<double>(int_part);
        if (j < line.size() && line[j] == '.') {
            ++j;
            double frac = 0;
            double scale = 0.1;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                frac += (line[j] - '0') * scale;
                scale *= 0.1;
                ++j;
            }
            value += frac;
        }
        // 期望 '%'
        if (j >= line.size() || line[j] != '%') continue;

        // 后续应有 " N/M bytes" 模式（至少有数字/数字 bytes）
        ++j; // 跳过 '%'
        // 跳过空格
        while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        // 解析 N
        bool has_n = false;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
            has_n = true;
            ++j;
        }
        if (!has_n) continue;
        // 期望 '/'
        if (j >= line.size() || line[j] != '/') continue;
        ++j;
        // 解析 M
        bool has_m = false;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
            has_m = true;
            ++j;
        }
        if (!has_m) continue;
        // 期望 " bytes"
        while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        if (j + 5 <= line.size() &&
            (line[j] == 'b' || line[j] == 'B') &&
            (line[j + 1] == 'y' || line[j + 1] == 'Y') &&
            (line[j + 2] == 't' || line[j + 2] == 'T') &&
            (line[j + 3] == 'e' || line[j + 3] == 'E') &&
            (line[j + 4] == 's' || line[j + 4] == 'S')) {
            return static_cast<int>(std::lround(value));
        }
    }
    return -1;
}

} // namespace

EsptoolProgressParser::EsptoolProgressParser(
    std::function<void(const FlashEvent&)> callback)
    : callback_(std::move(callback)) {}

void EsptoolProgressParser::FeedLine(const std::string& line) {
    const std::string_view sv(line);
    bool emitted = false;

    // 1. 错误检测：以 "A fatal error occurred:" 开头。
    if (sv.rfind("A fatal error occurred:", 0) == 0) {
        FlashEvent e;
        e.kind = FlashEvent::kError;
        // text 为去掉前缀后的内容（保留有用信息）。
        std::string_view msg = sv.substr(23);  // 跳过 "A fatal error occurred:"
        // 去掉前导空格
        while (!msg.empty() && std::isspace(static_cast<unsigned char>(msg.front()))) {
            msg.remove_prefix(1);
        }
        e.text = ToWide(msg.empty() ? sv : msg);
        if (callback_) callback_(e);
        return;  // 错误行不重复发 LogLine
    }

    // 2. 阶段检测（每个阶段只发一次）。
    if (ContainsCI(sv, "Stub running") || ContainsCI(sv, "Stub flasher running")) {
        if (!stage_connecting_sent_) {
            stage_connecting_sent_ = true;
            FlashEvent e;
            e.kind = FlashEvent::kStage;
            e.text = L"连接中";
            if (callback_) callback_(e);
            emitted = true;
        }
    } else if (ContainsCI(sv, "Detected chip type") ||
               ContainsCI(sv, "Detecting chip type")) {
        if (!stage_detecting_sent_) {
            stage_detecting_sent_ = true;
            FlashEvent e;
            e.kind = FlashEvent::kStage;
            e.text = L"检测芯片";
            if (callback_) callback_(e);
            emitted = true;
        }
    } else if (ContainsCI(sv, "Writing at")) {
        if (!stage_writing_sent_) {
            stage_writing_sent_ = true;
            FlashEvent e;
            e.kind = FlashEvent::kStage;
            e.text = L"写入";
            if (callback_) callback_(e);
            emitted = true;
        }
    } else if (ContainsCI(sv, "Erasing flash")) {
        if (!stage_erasing_sent_) {
            stage_erasing_sent_ = true;
            FlashEvent e;
            e.kind = FlashEvent::kStage;
            e.text = L"擦除";
            if (callback_) callback_(e);
            emitted = true;
        }
    } else if (ContainsCI(sv, "Hash of data verified")) {
        if (!stage_verifying_sent_) {
            stage_verifying_sent_ = true;
            FlashEvent e;
            e.kind = FlashEvent::kStage;
            e.text = L"校验";
            if (callback_) callback_(e);
            emitted = true;
        }
    }

    // 3. 进度检测（黑名单行跳过）。
    if (!IsBlacklisted(sv)) {
        int percent = ParseProgressV4(sv);
        if (percent < 0) percent = ParseProgressV5(sv);
        if (percent >= 0) {
            FlashEvent e;
            e.kind = FlashEvent::kProgress;
            e.percent = percent;
            if (callback_) callback_(e);
            emitted = true;
        }
    }

    // 4. 未发任何事件时作为普通日志行。
    if (!emitted) {
        FlashEvent e;
        e.kind = FlashEvent::kLogLine;
        e.text = ToWide(sv);
        if (callback_) callback_(e);
    }
}

} // namespace voicestick
