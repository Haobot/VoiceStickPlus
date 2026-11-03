// 字段内定位器实现（纯逻辑，零 Win32 依赖；双端同构见 voice_edit_anchor.h）。

#include "voice_edit_anchor.h"

#include <cstdint>
#include <vector>

#include "utf8_grapheme.h"

namespace voicestick {

namespace {

// UTF-8 文本 → UTF-16 码元序列（匹配口径与平台选区 API 同：emoji/组合字符不拆
// surrogate，码元级精确相等）。
std::vector<std::uint16_t> Utf16Units(std::string_view text_utf8) {
    std::vector<std::uint16_t> units;
    units.reserve(text_utf8.size());
    std::size_t pos = 0;
    while (pos < text_utf8.size()) {
        const auto decoded = utf8::DecodeUtf8(text_utf8, pos);
        pos += decoded.length;
        const char32_t cp = decoded.value;
        if (cp <= 0xFFFF) {
            units.push_back(static_cast<std::uint16_t>(cp));
        } else {
            // 星面码点 → 代理对（U+10000..U+10FFFF；DecodeUtf8 对非法首字节按单
            // 字节容错，不会产生越界值）。
            const char32_t value = cp - 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xD800 + (value >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00 + (value & 0x3FF)));
        }
    }
    return units;
}

bool UnitsEqual(const std::vector<std::uint16_t>& haystack, std::size_t start,
                const std::vector<std::uint16_t>& needle) {
    if (start > haystack.size() || haystack.size() - start < needle.size()) return false;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
        if (haystack[start + offset] != needle[offset]) return false;
    }
    return true;
}

bool IsAsciiAlnum(char32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

bool IsWhitespaceCodePoint(char32_t cp) {
    // Unicode White_Space 属性常用集（与 Swift Character.isWhitespace 对齐）。
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D ||
           cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// 逆序取末尾码点（跳过 UTF-8 续行字节），*start_out 回填其起始字节偏移。
utf8::CodePoint LastCodePoint(std::string_view text, std::size_t* start_out) {
    if (text.empty()) {
        *start_out = 0;
        return {};
    }
    std::size_t start = text.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        --start;
    }
    *start_out = start;
    return utf8::DecodeUtf8(text, start);
}

}  // namespace

VoiceEditAnchorResult LocateVoiceEditAnchor(std::string_view field_utf8,
                                             std::size_t caret_utf16,
                                             std::size_t selection_len,
                                             std::string_view anchored_utf8,
                                             bool anchored_has_recorded_newline) {
    VoiceEditAnchorResult result;
    const auto field = Utf16Units(field_utf8);
    const auto anchored = Utf16Units(anchored_utf8);
    // 空 anchored：不可能出现（调用方保证 remaining 非空；防御，按全 miss 处理）。
    // 注意 anchored 比字段长**不是**拒绝条件——P1c 裸回落（用户已手删已记录换行）
    // 仍可能命中，各锚级自带的边界守卫（UnitsEqual 越界判 false）自然处理。
    if (anchored.empty()) {
        result.failure = VoiceEditAnchorFailure::kFieldMismatch;
        return result;
    }

    // P1a 光标前缀锚：anchored 紧贴光标之前（[0, caret) 以 anchored 结尾）。
    if (selection_len == 0 && caret_utf16 >= anchored.size() &&
        caret_utf16 <= field.size() &&
        UnitsEqual(field, caret_utf16 - anchored.size(), anchored)) {
        result.ok = true;
        result.anchor.kind = VoiceEditAnchor::Kind::kCaretPrefix;
        result.anchor.anchor_start = caret_utf16 - anchored.size();
        result.anchor.anchor_end = caret_utf16;
        return result;
    }
    // P1b 光标后缀锚：anchored 紧贴光标之后（[caret, len) 以 anchored 开头）。
    if (selection_len == 0 && caret_utf16 + anchored.size() <= field.size() &&
        UnitsEqual(field, caret_utf16, anchored)) {
        result.ok = true;
        result.anchor.kind = VoiceEditAnchor::Kind::kCaretSuffix;
        result.anchor.anchor_start = caret_utf16;
        result.anchor.anchor_end = caret_utf16 + anchored.size();
        return result;
    }
    // P1c 字段末尾锚（含现行「anchored 优先、裸 remaining 回落」语义）：
    // anchored 是字段后缀直接命中；anchored_has_recorded_newline 时（anchored =
    // remaining + 已记录换行）回落裸 remaining 后缀（用户已手删换行）——与 macOS
    // locateFieldEnd 的优先级/回落顺序逐字对齐。
    if (UnitsEqual(field, field.size() - anchored.size(), anchored)) {
        result.ok = true;
        result.anchor.kind = VoiceEditAnchor::Kind::kFieldEnd;
        result.anchor.anchor_start = field.size() - anchored.size();
        result.anchor.anchor_end = field.size();
        return result;
    }
    if (anchored_has_recorded_newline && anchored.size() >= 2 &&
        anchored.back() == 0x0A) {
        const std::vector<std::uint16_t> bare(anchored.begin(), anchored.end() - 1);
        if (UnitsEqual(field, field.size() - bare.size(), bare)) {
            result.ok = true;
            result.anchor.kind = VoiceEditAnchor::Kind::kFieldEnd;
            result.anchor.anchor_start = field.size() - bare.size();
            result.anchor.anchor_end = field.size();
            return result;
        }
    }
    // P2 唯一出现锚：非重叠精确出现恰好一次；≥2 次 → anchor_ambiguous。
    std::size_t hits = 0;
    std::size_t first_start = 0;
    for (std::size_t index = 0; index + anchored.size() <= field.size();) {
        if (UnitsEqual(field, index, anchored)) {
            ++hits;
            if (hits == 1) first_start = index;
            if (hits >= 2) break;
            index += anchored.size();  // 非重叠：前进过整个匹配
            continue;
        }
        ++index;
    }
    if (hits == 1) {
        result.ok = true;
        result.anchor.kind = VoiceEditAnchor::Kind::kUniqueOccurrence;
        result.anchor.anchor_start = first_start;
        result.anchor.anchor_end = first_start + anchored.size();
        return result;
    }
    result.failure = hits >= 2 ? VoiceEditAnchorFailure::kAnchorAmbiguous
                               : VoiceEditAnchorFailure::kFieldMismatch;
    return result;
}

VoiceEditAnchor LocateVoiceEditFieldEndAnchor(std::string_view field_utf8,
                                               std::string_view remaining_utf8,
                                               bool pressed_enter) {
    const auto field = Utf16Units(field_utf8);
    const auto remaining = Utf16Units(remaining_utf8);
    VoiceEditAnchor anchor;
    anchor.kind = VoiceEditAnchor::Kind::kFieldEnd;
    anchor.anchor_end = field.size();
    if (remaining.empty() || remaining.size() > field.size()) {
        // 防御：不可命中（调用方保证 remaining 非空；start==end 表示 miss，
        // 由调用方判 anchor_start == anchor_end 识别）。
        anchor.anchor_start = field.size();
        return anchor;
    }
    // pressed_enter 优先「remaining+\n」后缀（避免 remaining 自身以换行结尾时
    // 误配裸后缀）；回落裸 remaining（用户已手删换行）。
    if (pressed_enter && !field.empty() && field.back() == 0x0A &&
        remaining.size() + 1 <= field.size() &&
        UnitsEqual(field, field.size() - remaining.size() - 1, remaining)) {
        anchor.anchor_start = field.size() - remaining.size() - 1;
        return anchor;
    }
    if (UnitsEqual(field, field.size() - remaining.size(), remaining)) {
        anchor.anchor_start = field.size() - remaining.size();
        return anchor;
    }
    anchor.anchor_start = field.size();
    return anchor;
}

bool IsMidlineInsertion(std::string_view field_utf8, std::size_t caret_utf16) {
    // 解码字段并按 UTF-16 偏移定位光标后第一个码点（首码点即可判定空白/换行）。
    std::size_t utf16_offset = 0;
    std::size_t pos = 0;
    while (pos < field_utf8.size()) {
        const auto decoded = utf8::DecodeUtf8(field_utf8, pos);
        if (utf16_offset >= caret_utf16) {
            const char32_t cp = decoded.value;
            if (cp == 0x0A || cp == 0x0D) return false;
            return !IsWhitespaceCodePoint(cp);
        }
        utf16_offset += decoded.value <= 0xFFFF ? 1 : 2;
        pos += decoded.length;
    }
    return false;  // 光标在字段末尾（其后无字符）＝非句中
}

bool MidlinePeriodStrippable(std::string_view text_utf8) {
    if (text_utf8.empty()) return false;
    std::size_t last_start = 0;
    const auto last = LastCodePoint(text_utf8, &last_start);
    if (last.value == U'。' || last.value == U'．') return true;
    if (last.value == U'.') {
        const auto head = text_utf8.substr(0, last_start);
        if (!head.empty()) {
            std::size_t before_start = 0;
            const auto before = LastCodePoint(head, &before_start);
            if (IsAsciiAlnum(before.value)) return false;  // 「3.」「v1.」：恒保留
        }
        return true;
    }
    return false;
}

std::string StripMidlinePeriod(std::string_view text_utf8, bool is_midline) {
    if (!is_midline || text_utf8.empty()) return std::string(text_utf8);
    std::size_t last_start = 0;
    const auto last = LastCodePoint(text_utf8, &last_start);
    if (last.value == U'。' || last.value == U'．') {
        return std::string(text_utf8.substr(0, last_start));
    }
    if (last.value == U'.') {
        const auto head = text_utf8.substr(0, last_start);
        if (!head.empty()) {
            std::size_t before_start = 0;
            const auto before = LastCodePoint(head, &before_start);
            if (IsAsciiAlnum(before.value)) {
                return std::string(text_utf8);  // 「3.」「v1.」：保留
            }
        }
        return std::string(head);
    }
    return std::string(text_utf8);
}

}  // namespace voicestick
