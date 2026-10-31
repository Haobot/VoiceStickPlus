#pragma once

// UTF-8 解码 + 近似 UAX #29 扩展 grapheme cluster 切分（双端对齐 Doc §7.2）。
// 从 voice_edit_command.cc 抽出为 inline 共享头：voice_edit_command.cc 与
// voice_targeted_edit.cc 共用同一份实现，避免词条/边界逻辑漂移。
//
// 覆盖：CJK / emoji / 组合字符 / ZWJ 连字 / 国旗 / 变体选择符与 emoji 肤色修饰符。
// 说明：按常见子集实现（不含完整 UAX #29，复杂 Indic 组合可能多计）。

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace voicestick {
namespace utf8 {

struct CodePoint {
    char32_t value = 0;
    std::size_t length = 1;
};

inline CodePoint DecodeUtf8(std::string_view text, std::size_t pos) {
    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char first = byte(pos);
    if (first < 0x80) return {first, 1};
    std::size_t length = 0;
    char32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    } else {
        return {first, 1};  // 非法首字节：按单字节容错（不崩，计数误差可接受）
    }
    if (pos + length > text.size()) return {first, 1};
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char cont = byte(pos + i);
        if ((cont & 0xC0) != 0x80) return {first, 1};
        value = (value << 6) | (cont & 0x3F);
    }
    return {value, length};
}

inline bool IsCombiningMark(char32_t cp) {
    // 常见组合标记区间（approximate Unicode Mn/Me）：拉丁/希腊/西里尔扩展、希伯来/
    // 阿拉伯元音符号、日文浊点、组合用符号；变体选择符由单独规则处理。
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0x3099 && cp <= 0x309A) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

inline bool IsRegionalIndicator(char32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

struct GraphemeSpan {
    std::size_t offset = 0;   // UTF-8 字节偏移
    std::size_t length = 0;   // UTF-8 字节长度
};

// 近似 UAX #29 扩展 grapheme cluster：
//  - CRLF 同簇；
//  - 组合标记 / 变体选择符 / 键帽 U+20E3 / emoji 肤色修饰符并入前一簇；
//  - ZWJ（U+200D）自身与其后一个码点并入前一簇（emoji 连字）；
//  - 连续区域指示符成对（国旗）。
inline std::vector<GraphemeSpan> SplitGraphemes(std::string_view text) {
    std::vector<GraphemeSpan> spans;
    spans.reserve(text.size() / 3 + 1);
    std::size_t pos = 0;
    char32_t prev = 0;
    int regional_run = 0;
    while (pos < text.size()) {
        const CodePoint cp = DecodeUtf8(text, pos);
        bool extend = false;
        if (!spans.empty()) {
            if (prev == U'\r' && cp.value == U'\n') {
                extend = true;
            } else if (IsCombiningMark(cp.value)) {
                extend = true;
            } else if (cp.value == 0xFE0E || cp.value == 0xFE0F || cp.value == 0x20E3) {
                extend = true;
            } else if (cp.value >= 0x1F3FB && cp.value <= 0x1F3FF) {
                extend = true;
            } else if (cp.value == 0x200D || prev == 0x200D) {
                extend = true;
            } else if (IsRegionalIndicator(cp.value) && (regional_run % 2) == 1) {
                extend = true;
            }
        }
        if (extend) {
            spans.back().length += cp.length;
        } else {
            spans.push_back(GraphemeSpan{pos, cp.length});
        }
        if (IsRegionalIndicator(cp.value)) {
            ++regional_run;
        } else {
            regional_run = 0;
        }
        prev = cp.value;
        pos += cp.length;
    }
    return spans;
}

// UTF-8 文本的 UTF-16 码元长度（BMP=1，增补平面=2 surrogate）。
inline std::size_t Utf16Length(std::string_view text) {
    std::size_t units = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const CodePoint cp = DecodeUtf8(text, pos);
        units += cp.value >= 0x10000 ? 2 : 1;
        pos += cp.length;
    }
    return units;
}

}  // namespace utf8
}  // namespace voicestick
