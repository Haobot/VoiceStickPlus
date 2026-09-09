// selection_correction.h 的实现：划词纠错候选生成纯逻辑。

#include "selection_correction.h"

#include "pinyin_guard.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace voicestick {
namespace {

constexpr std::size_t kMaxCandidateBytes = 64;  // 与热词长度上限同口径

std::string_view TrimAscii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// 序号后的分隔标点：ASCII 与全角变体（）、：、、）。
constexpr std::string_view kListMarks[] = {
    ".", ",", ")", ":",
    "\xEF\xBC\x89",  // ）
    "\xEF\xBC\x9A",  // ：
    "\xE3\x80\x81",  // 、
};

// 剥候选行的序号前缀：「1.」「1、」「1)」「- 」及其后空格。
std::string_view StripListPrefix(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i > 0 && i < line.size()) {
        for (const auto mark : kListMarks) {
            if (line.substr(i).starts_with(mark)) {
                return TrimAscii(line.substr(i + mark.size()));
            }
        }
        return line;  // 数字开头但非序号（如「3D 打印」）：原样保留
    }
    if (line.starts_with("- ") || line.starts_with("* ")) {
        line.remove_prefix(2);
    }
    return line;
}

// 候选行含中英文标点（词级候选不应有），或非「1. 」类前缀的逗号分隔多词。
bool ContainsPunctuation(std::string_view s) {
    for (const unsigned char c : s) {
        if (c < 0x80) {
            if (std::ispunct(c) != 0) return true;
        } else {
            // CJK 标点 U+3000-U+303F（、。「」等）与全角 U+FF00-U+FFEF
            // （，？！等）的 UTF-8 首字节均为 0xE3/0xEF，粗判足够——
            // 误伤范围是 CJK 符号块（U+3000 区）与谚文兼容区，候选是
            // 汉字/ASCII 词，不会落在这些块。
            if (c == 0xE3 || c == 0xEF) return true;
        }
    }
    return false;
}

bool IsNoneReply(std::string_view line) {
    return line == "无" || line == "无。" || line == "none" ||
           line == "None" || line == "NONE";
}

} // namespace

std::string BuildCorrectionCandidatesPrompt(std::string_view wrong_text,
                                            std::string_view context) {
    std::string prompt = "错词：" + std::string(wrong_text) + "\n";
    if (!context.empty()) {
        prompt += "上下文：" + std::string(context) + "\n";
    }
    prompt +=
        "上面是语音识别出错的词。给出最可能的正确词，最多5个，每行一个，"
        "不要解释。正确词必须与错词字数相同，且每个字与错词对应位置的字同音"
        "或近音（声母韵母相近）。如果想不到任何符合的词，只输出：无";
    return prompt;
}

std::vector<std::string> ParseCandidateLines(std::string_view llm_output) {
    std::vector<std::string> lines;
    std::size_t line_start = 0;
    while (line_start <= llm_output.size()) {
        std::size_t line_end = llm_output.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = llm_output.size();
        auto line = TrimAscii(llm_output.substr(line_start, line_end - line_start));
        line_start = line_end + 1;
        if (line.empty()) continue;
        line = StripListPrefix(line);
        if (line.empty() || IsNoneReply(line)) continue;
        if (ContainsPunctuation(line)) continue;
        if (line.size() > kMaxCandidateBytes) continue;
        lines.emplace_back(line);
    }
    return lines;
}

std::vector<std::string> FilterCandidates(
    std::string_view wrong_text,
    const std::vector<std::string>& candidates) {
    std::vector<std::string> kept;
    for (const auto& candidate : candidates) {
        if (candidate == wrong_text) continue;
        if (!SameOrNearText(wrong_text, candidate)) continue;
        if (std::find(kept.begin(), kept.end(), candidate) != kept.end()) {
            continue;
        }
        kept.push_back(candidate);
    }
    return kept;
}

} // namespace voicestick
