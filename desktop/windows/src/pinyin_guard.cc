#include "pinyin_guard.h"

#include "pinyin_data.h"

#include <algorithm>
#include <cstdint>

namespace voicestick {
namespace {

// 模糊音韵母对（M0 spike 实测口径）：an/ang 等鼻音前后混淆、e/i 卷舌弱化
// （设/识、这/之、车/吃）。声母模糊对（n/l 等）在「韵母有交集即同音」口径下
// 是死分支，不移植；("un","ün") 为死条目（pypinyin 体系 ü 写作 v），同样不移植。
constexpr const char* kFuzzyFinalPairs[][2] = {
    {"an", "ang"}, {"en", "eng"}, {"in", "ing"},
    {"ian", "iang"}, {"uan", "uang"}, {"e", "i"},
};

bool IsCjk(std::uint32_t cp) {
    return cp >= 0x4e00 && cp <= 0x9fff;
}

std::uint32_t ToLowerAscii(std::uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp;
}

// 逗号分隔列表（kTable 的 initials/finals 格式）中是否含 item。
bool ListContains(std::string_view list, std::string_view item) {
    std::size_t pos = 0;
    while (pos <= list.size()) {
        std::size_t end = list.find(',', pos);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(pos, end - pos) == item) return true;
        if (end >= list.size()) break;
        pos = end + 1;
    }
    return false;
}

const pinyin_data::Entry* FindEntry(std::uint32_t cp) {
    const auto* first = pinyin_data::kTable;
    const auto* last = pinyin_data::kTable + pinyin_data::kTableSize;
    const auto* it = std::lower_bound(first, last, cp,
                                      [](const pinyin_data::Entry& e, std::uint32_t c) {
                                          return e.cp < c;
                                      });
    return (it != last && it->cp == cp) ? it : nullptr;
}

bool SetsIntersect(std::string_view a, std::string_view b) {
    std::size_t pos = 0;
    while (pos <= a.size()) {
        std::size_t end = a.find(',', pos);
        if (end == std::string_view::npos) end = a.size();
        if (ListContains(b, a.substr(pos, end - pos))) return true;
        if (end >= a.size()) break;
        pos = end + 1;
    }
    return false;
}

bool FuzzyFinalHit(const pinyin_data::Entry* ea, const pinyin_data::Entry* eb) {
    for (const auto& pair : kFuzzyFinalPairs) {
        const bool forward = ListContains(ea->finals, pair[0]) &&
                             ListContains(eb->finals, pair[1]);
        const bool backward = ListContains(ea->finals, pair[1]) &&
                              ListContains(eb->finals, pair[0]);
        if (forward || backward) return true;
    }
    return false;
}

// ---- UTF-8 码点工具（风格对齐 text_refiner.cc 手写解码）----

std::uint32_t DecodeCodepoint(std::string_view s, std::size_t& i) {
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
        if ((b & 0xc0) != 0x80) {
            ++i;
            return lead;
        }
        cp = (cp << 6) | (b & 0x3f);
    }
    i += len;
    return cp;
}

std::size_t CodepointCount(std::string_view s) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        DecodeCodepoint(s, i);
        ++count;
    }
    return count;
}

std::string_view TrimAscii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

bool Contains(std::string_view haystack, std::string_view needle) {
    return needle.empty() || haystack.find(needle) != std::string_view::npos;
}

bool ContainsAsciiAlnum(std::string_view s) {
    for (const char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if ((uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
            (uc >= 'A' && uc <= 'Z')) {
            return true;
        }
    }
    return false;
}

// 替换对逐字近音校验（调用方已保证码点数相等）。
bool SameOrNearPerCodepoint(std::string_view src, std::string_view dst) {
    std::size_t i = 0, j = 0;
    while (i < src.size() && j < dst.size()) {
        const auto a = DecodeCodepoint(src, i);
        const auto b = DecodeCodepoint(dst, j);
        if (!PinyinSameOrNear(a, b)) return false;
    }
    return true;
}

} // namespace

bool PinyinSameOrNear(std::uint32_t a, std::uint32_t b) {
    if (a == b) return true;
    if (!IsCjk(a) || !IsCjk(b)) {
        // 非双方汉字：ASCII 折叠大小写后相等（spike 口径的守卫侧简化，
        // 非 ASCII 大小写折叠判不等=保守拒绝）。
        return ToLowerAscii(a) == ToLowerAscii(b);
    }
    const auto* ea = FindEntry(a);
    const auto* eb = FindEntry(b);
    if (ea == nullptr || eb == nullptr) return false;  // 表外字按不同音
    // 同音（M0 spike 实测口径）：韵母集合有交集即放行——声母不参与，
    // 「渍 zì/词 cí」「马 mǎ/打 dǎ」类声母不同的真机纠错依赖本条。
    if (SetsIntersect(ea->finals, eb->finals)) return true;
    // 近音：韵母命中模糊对且声母集合有交集（设 shè/识 shí）。
    return SetsIntersect(ea->initials, eb->initials) && FuzzyFinalHit(ea, eb);
}

CorrectionOutcome ApplyPinyinCorrections(std::string_view asr,
                                         std::string_view instructions,
                                         std::string_view context) {
    CorrectionOutcome out;
    const auto instr_trimmed = TrimAscii(instructions);
    if (instr_trimmed.empty() || instr_trimmed == "无" || instr_trimmed == "无。" ||
        instr_trimmed == "none" || instr_trimmed == "None") {
        out.text = std::string(asr);
        return out;
    }

    std::string result(asr);
    std::size_t line_start = 0;
    while (line_start <= instructions.size()) {
        std::size_t line_end = instructions.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = instructions.size();
        auto line = TrimAscii(instructions.substr(line_start, line_end - line_start));
        line_start = line_end + 1;
        if (line.empty() || line == "无" || line == "无。" ||
            line == "处理：" || line == "处理:") {
            continue;
        }
        const auto arrow = line.find("→");
        if (arrow != std::string_view::npos) {
            const auto src = TrimAscii(line.substr(0, arrow));
            const auto dst = TrimAscii(line.substr(arrow + 3));  // → 为 3 字节 UTF-8
            if (src.empty() || dst.empty() || !Contains(result, src) ||
                CodepointCount(src) != CodepointCount(dst) ||
                !Contains(context, dst) ||
                !SameOrNearPerCodepoint(src, dst)) {
                out.rejected.emplace_back(line);
                continue;
            }
            const auto pos = result.find(src);
            result.replace(pos, src.size(), dst);
        } else {
            if (ContainsAsciiAlnum(line) || !Contains(result, line)) {
                out.rejected.emplace_back(line);
                continue;
            }
            const auto pos = result.find(line);
            result.erase(pos, line.size());
        }
    }

    if (CodepointCount(result) < CodepointCount(asr) * 0.4) {
        out.text = std::string(asr);  // 删除幅度过大：整体回退原文
        out.rejected.emplace_back("total-ratio");
        return out;
    }
    out.text = std::move(result);
    return out;
}

} // namespace voicestick
