// 标点独词输入：整轮 ASR final 只说了标点名（如“逗号”）时直接产出符号
// 本身（“，”）。纯逻辑、无 Win32 依赖；与 macOS SpokenPunctuation.swift
// 同表同语义（判定向量见 core_tests_spoken_punctuation.cc）。
#include "spoken_punctuation.h"

#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

namespace voicestick {
namespace {

// ---- UTF-8 码点工具（风格对齐 text_refiner.cc 的手写解码）----

std::uint32_t DecodeCodepoint(const std::string& s, std::size_t& i) {
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    std::size_t len = 1;
    std::uint32_t cp = lead;
    if ((lead & 0xe0) == 0xc0) {
        len = 2;
        cp = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
        len = 3;
        cp = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
        len = 4;
        cp = lead & 0x07;
    }
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xc0) != 0x80) {  // 非法序列按单字节吞掉，避免越界
            ++i;
            return lead;
        }
        cp = (cp << 6) | (b & 0x3f);
    }
    i += len;
    return cp;
}

void AppendCodepoint(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// ---- 首尾噪声字符（空白另算）----
// ASR 常在词后补“。”；波浪号 〜～ 在 Unicode 分类里是符号不是标点，须
// 显式列入。与 macOS SpokenPunctuation.edgeNoiseScalars 同集合。

bool IsEdgeNoise(std::uint32_t cp) {
    switch (cp) {
        // ASCII 标点
        case '.': case ',': case '!': case '?': case ';': case ':':
        case '(': case ')': case '"': case '\'': case '~':
            return true;
        // CJK 标点
        case 0x3001: case 0x3002:  // 、 。
        case 0xff01: case 0xff0c:  // ！ ，
        case 0xff1a: case 0xff1b:  // ： ；
        case 0xff1f:               // ？
        case 0xff08: case 0xff09:  // （ ）
        case 0x3008: case 0x3009:  // 〈 〉
        case 0x300a: case 0x300b:  // 《 》
        case 0x3010: case 0x3011:  // 【 】
        case 0x2018: case 0x2019:  // ‘ ’
        case 0x201c: case 0x201d:  // “ ”
        case 0x00b7:               // ·
        case 0x2026:               // …
        case 0x2014: case 0x2013:  // — –
        case 0xff5e: case 0x301c:  // ～ 〜
            return true;
        default:
            return cp < 0x80 ? std::isspace(static_cast<int>(cp)) != 0
                             : (cp >= 0x2000 && cp <= 0x200f);
    }
}

// ---- 标点名 → 符号 ----
// 成对符号只收“左/右”显式说法（裸“引号”“括号”方向歧义，不映射）；
// 英文标点名不收（ASR 为中文引擎）。与 macOS symbolsByName 同表。

struct SpokenPunctuationEntry {
    std::string_view name;
    std::string_view symbol;
};

constexpr SpokenPunctuationEntry kSpokenPunctuationTable[] = {
    {"逗号", "，"},   {"逗点", "，"},
    {"句号", "。"},   {"句点", "。"},
    {"问号", "？"},
    {"叹号", "！"},   {"感叹号", "！"}, {"惊叹号", "！"},
    {"顿号", "、"},
    {"冒号", "："},
    {"分号", "；"},
    {"省略号", "……"},
    {"破折号", "——"},
    {"波浪号", "～"},
    {"间隔号", "·"},
    {"左引号", "“"},  {"左双引号", "“"},
    {"右引号", "”"},  {"右双引号", "”"},
    {"左单引号", "‘"},
    {"右单引号", "’"},
    {"左括号", "（"},
    {"右括号", "）"},
    {"左书名号", "《"},
    {"右书名号", "》"},
};

} // namespace

std::optional<std::string> SpokenPunctuationOnlySymbol(std::string_view text) {
    const std::string s(text);
    std::vector<std::uint32_t> cps;
    cps.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        cps.push_back(DecodeCodepoint(s, i));
    }
    std::size_t start = 0;
    std::size_t end = cps.size();
    while (start < end && IsEdgeNoise(cps[start])) ++start;
    while (end > start && IsEdgeNoise(cps[end - 1])) --end;
    if (start >= end) return std::nullopt;
    std::string name;
    for (std::size_t i = start; i < end; ++i) {
        AppendCodepoint(name, cps[i]);
    }
    for (const auto& entry : kSpokenPunctuationTable) {
        if (name == entry.name) return std::string(entry.symbol);
    }
    return std::nullopt;
}

} // namespace voicestick
