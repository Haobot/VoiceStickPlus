#include "text_refiner.h"

#include "llm_refinement_client.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <vector>

namespace voicestick {
namespace {

// ---- UTF-8 码点工具（风格对齐 hotword_selector.cc 的手写解码）----

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

bool IsCjk(std::uint32_t cp) {
    return (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3400 && cp <= 0x4dbf);
}

bool IsLatinLetter(std::uint32_t cp) {
    return cp < 0x80 && std::isalpha(static_cast<int>(cp)) != 0;
}

bool IsSpaceChar(std::uint32_t cp) {
    if (cp < 0x80) return std::isspace(static_cast<int>(cp)) != 0;
    return cp >= 0x2000 && cp <= 0x200f;
}

// 规则引擎与守卫归一化共用的标点判定（同一集合保证“LLM 只动标点空白”
// 的变化在守卫侧天然放行）。
bool IsPunct(std::uint32_t cp) {
    if (cp < 0x80) {
        switch (cp) {
            case '.': case ',': case '!': case '?': case ';': case ':':
            case '(': case ')': case '"': case '\'':
                return true;
            default:
                return false;  // $ # % & + = 等内容符号不算标点
        }
    }
    switch (cp) {
        case 0x2018: case 0x2019:  // ‘ ’
        case 0x201c: case 0x201d:  // “ ”
        case 0x2026:               // …
        case 0x2014: case 0x2013:  // — –
        case 0x3001: case 0x3002:  // 、 。
        case 0x300a: case 0x300b:  // 《 》
        case 0xff01: case 0xff0c:  // ！ ，
        case 0xff1a: case 0xff1b:  // ： ；
        case 0xff1f:               // ？
        case 0xff08: case 0xff09:  // （ ）
            return true;
        default:
            return false;
    }
}

bool IsPunctOrSpace(std::uint32_t cp) {
    return IsPunct(cp) || IsSpaceChar(cp);
}

std::uint32_t AsciiLower(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
}

// ---- 规则引擎 ----

bool IsLaughChar(std::uint32_t cp) {
    return cp == U'哈' || cp == U'嘿' || cp == U'呵' || cp == U'嘻';
}

// 嗯/呃 无条件删；啊/哦/噢/唉/哎/诶 须后跟标点或空白才删（防误伤实义开头）。
bool IsUnconditionalLeadFiller(std::uint32_t cp) {
    return cp == U'嗯' || cp == U'呃';
}

bool IsConditionalLeadFiller(std::uint32_t cp) {
    return cp == U'啊' || cp == U'哦' || cp == U'噢' ||
           cp == U'唉' || cp == U'哎' || cp == U'诶';
}

std::vector<std::uint32_t> RuleRefineCodepoints(const std::vector<std::uint32_t>& in) {
    std::vector<std::uint32_t> cps = in;

    // 1) 句首语气词 + 其后紧随的标点/空白
    if (!cps.empty()) {
        const std::uint32_t head = cps[0];
        const bool remove_lead =
            IsUnconditionalLeadFiller(head) ||
            (IsConditionalLeadFiller(head) && cps.size() > 1 &&
             IsPunctOrSpace(cps[1]));
        if (remove_lead) {
            std::size_t end = 1;
            while (end < cps.size() && IsPunctOrSpace(cps[end])) ++end;
            cps.erase(cps.begin(), cps.begin() + static_cast<long>(end));
        }
    }

    // 2) CJK 叠字：连续同字 >=3 时，笑声字保留 2 个，其余保留 1 个
    std::vector<std::uint32_t> out;
    out.reserve(cps.size());
    for (std::size_t i = 0; i < cps.size();) {
        std::size_t run = 1;
        while (i + run < cps.size() && cps[i + run] == cps[i]) ++run;
        if (run >= 3 && IsCjk(cps[i])) {
            const std::size_t keep = IsLaughChar(cps[i]) ? 2 : 1;
            out.insert(out.end(), keep, cps[i]);
        } else {
            out.insert(out.end(), cps.begin() + static_cast<long>(i),
                       cps.begin() + static_cast<long>(i + run));
        }
        i += run;
    }
    cps.swap(out);

    // 3) CJK 之间的空白删除（拉丁/数字周围的合法空格保留）
    out.clear();
    for (std::size_t i = 0; i < cps.size();) {
        if (IsSpaceChar(cps[i]) && !out.empty() && IsCjk(out.back())) {
            std::size_t j = i;
            while (j < cps.size() && IsSpaceChar(cps[j])) ++j;
            if (j < cps.size() && IsCjk(cps[j])) {  // CJK 空白… CJK → 删空白
                i = j;
                continue;
            }
        }
        out.push_back(cps[i]);
        ++i;
    }
    cps.swap(out);

    // 4) 重复标点收敛：同一标点连续出现收敛为 1；省略号 … 的合法长度是 2
    out.clear();
    for (std::size_t i = 0; i < cps.size();) {
        if (IsPunct(cps[i])) {
            std::size_t run = 1;
            while (i + run < cps.size() && cps[i + run] == cps[i]) ++run;
            std::size_t keep = 1;
            if (cps[i] == 0x2026) keep = (run >= 2) ? 2 : 1;
            out.insert(out.end(), keep, cps[i]);
            i += run;
        } else {
            out.push_back(cps[i]);
            ++i;
        }
    }
    cps.swap(out);

    // 5) 剥离句首孤立标点/空白（语气词删除后残留的悬挂标点）
    std::size_t start = 0;
    while (start < cps.size() && IsPunctOrSpace(cps[start])) ++start;
    cps.erase(cps.begin(), cps.begin() + static_cast<long>(start));

    return cps;
}

// ---- 守卫 ----

// 口水词表按 m0/refine spike 的实际删除行为校准（详见
// Doc/Plan/local-text-refinement.md §2）：放行过宽会漏拦误删实词，
// 过严会让正常精修整句回退。norm 形态不含空白，you know 记作 youknow。
const std::vector<std::u32string_view> kCjkFillerPhrases = {
    U"怎么说呢", U"你知道吧", U"就是说", U"然后呢", U"youknow",
    U"怎么说", U"你知道", U"那个", U"这个", U"就是", U"然后",
    U"对了",   U"好的",   U"一下", U"等等",
};
const std::vector<std::u32string_view> kLatinFillerTokens = {
    U"um", U"uh", U"er", U"ah", U"oh", U"hmm", U"erm",
};
const std::vector<std::uint32_t> kCjkSingleFillers = {
    U'嗯', U'呃', U'啊', U'哦', U'噢', U'唉', U'哎', U'诶',
    U'呀', U'哈', U'嘛', U'吧', U'呢', U'啦',
};

std::vector<std::uint32_t> NormalizeForGuard(std::string_view text) {
    std::string s(text);
    std::vector<std::uint32_t> norm;
    for (std::size_t i = 0; i < s.size();) {
        const std::uint32_t cp = DecodeCodepoint(s, i);
        if (IsPunctOrSpace(cp)) continue;
        norm.push_back(AsciiLower(cp));
    }
    return norm;
}

bool IsCjkSingleFiller(std::uint32_t cp) {
    return std::find(kCjkSingleFillers.begin(), kCjkSingleFillers.end(), cp) !=
           kCjkSingleFillers.end();
}

// 贪心切分一个删除片段：整段叠字直接放行；否则 CJK 按短语表（长词优先）、
// 叠字 run、单字语气词消耗，拉丁连续字母段整体查英文语气词表；
// 任何位置无法消耗即判非口水词（含数字必拦）。
bool IsFillerSegment(const std::vector<std::uint32_t>& seg) {
    if (seg.empty()) return true;
    bool all_same = true;
    for (std::uint32_t cp : seg) {
        if (cp != seg[0]) {
            all_same = false;
            break;
        }
    }
    if (all_same && seg.size() >= 2) return true;  // 叠字删减（我我/哈哈哈哈）

    std::size_t pos = 0;
    while (pos < seg.size()) {
        if (IsLatinLetter(seg[pos])) {
            std::size_t end = pos;
            while (end < seg.size() && IsLatinLetter(seg[end])) ++end;
            // uint32_t 码点流转 char32_t 视图（同宽同符号，reinterpret 安全）
            const std::u32string_view tok(
                reinterpret_cast<const char32_t*>(seg.data()) + pos, end - pos);
            bool hit = false;
            for (const auto& w : kLatinFillerTokens) {
                if (tok == w) {
                    hit = true;
                    break;
                }
            }
            if (!hit) return false;
            pos = end;
            continue;
        }
        std::size_t run = 1;
        while (pos + run < seg.size() && seg[pos + run] == seg[pos]) ++run;
        if (run >= 2 && IsCjk(seg[pos])) {  // 局部叠字（我我那个）
            pos += run;
            continue;
        }
        bool matched = false;
        for (const auto& phrase : kCjkFillerPhrases) {
            if (seg.size() - pos >= phrase.size() &&
                std::equal(phrase.begin(), phrase.end(), seg.begin() + pos)) {
                pos += phrase.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (IsCjk(seg[pos]) && IsCjkSingleFiller(seg[pos])) {
            ++pos;
            continue;
        }
        return false;
    }
    return true;
}

// 分治最长公共子串对齐：字符级贪心/LCS 会把英文口水词借字切碎（如 uh 的
// u 被 use 借走剩下孤字 h），块级锚定能让 um/uh/use 各自整体对齐。
// current 为跨递归共享的“正在积累的删除段”，锚点处结算成段。
bool DiffDeletedSegmentsRec(const std::vector<std::uint32_t>& original,
                            std::size_t ob, std::size_t oe,
                            const std::vector<std::uint32_t>& refined,
                            std::size_t rb, std::size_t re,
                            std::vector<std::vector<std::uint32_t>>& deleted,
                            std::vector<std::uint32_t>& current) {
    // 找最长公共子串（块）
    std::size_t best_len = 0, best_o = ob, best_r = rb;
    for (std::size_t i = ob; i + best_len < oe; ++i) {
        for (std::size_t j = rb; j + best_len < re; ++j) {
            std::size_t len = 0;
            while (i + len < oe && j + len < re &&
                   original[i + len] == refined[j + len]) {
                ++len;
            }
            if (len > best_len) {
                best_len = len;
                best_o = i;
                best_r = j;
            }
        }
    }
    if (best_len == 0) {
        // 无公共块：original 侧整段记为删除；refined 侧若非空即新增
        if (rb < re) return false;
        for (std::size_t i = ob; i < oe; ++i) current.push_back(original[i]);
        return true;
    }
    if (!DiffDeletedSegmentsRec(original, ob, best_o, refined, rb, best_r,
                                deleted, current)) {
        return false;
    }
    // 锚点块对齐：结算左侧积累的删除段
    if (!current.empty()) {
        deleted.push_back(std::move(current));
        current.clear();
    }
    return DiffDeletedSegmentsRec(original, best_o + best_len, oe,
                                  refined, best_r + best_len, re, deleted, current);
}

// 返回 nullopt 表示 refined 含 original 没有的字符（新增/改写）。
std::optional<std::vector<std::vector<std::uint32_t>>> DiffDeletedSegments(
    const std::vector<std::uint32_t>& original,
    const std::vector<std::uint32_t>& refined) {
    std::vector<std::vector<std::uint32_t>> deleted;
    std::vector<std::uint32_t> current;
    if (!DiffDeletedSegmentsRec(original, 0, original.size(),
                                refined, 0, refined.size(), deleted, current)) {
        return std::nullopt;
    }
    if (!current.empty()) deleted.push_back(std::move(current));
    return deleted;
}

} // namespace

std::string RuleRefineText(std::string_view text) {
    std::string s(text);
    std::vector<std::uint32_t> cps;
    cps.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        cps.push_back(DecodeCodepoint(s, i));
    }
    const auto refined = RuleRefineCodepoints(cps);
    std::string out;
    out.reserve(refined.size() * 3);
    for (std::uint32_t cp : refined) {
        AppendCodepoint(out, cp);
    }
    return out;
}

bool RefineResultSafe(std::string_view original,
                      std::string_view refined,
                      const std::vector<std::string>& hotwords) {
    if (!LLMRefinementClient::RefineResultKeepsHotwords(
            std::string(original), std::string(refined), hotwords)) {
        return false;
    }
    const auto orig_norm = NormalizeForGuard(original);
    const auto refined_norm = NormalizeForGuard(refined);
    const auto deleted = DiffDeletedSegments(orig_norm, refined_norm);
    if (!deleted.has_value()) return false;  // 存在新增/改写字符
    for (const auto& seg : *deleted) {
        if (!IsFillerSegment(seg)) return false;
    }
    return true;
}

} // namespace voicestick
