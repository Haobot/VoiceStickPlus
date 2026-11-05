#include "voice_targeted_domain.h"

#include <vector>

#include "utf8_grapheme.h"

namespace voicestick {

using utf8::DecodeUtf8;
using utf8::SplitGraphemes;
using utf8::Utf16Length;

namespace {

// ============================ 词法工具（与 voice_targeted_edit.cc 同口径） ============================

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ConsumeToken(std::string_view text, std::size_t* index, std::string_view token) {
    if (*index > text.size() || text.size() - *index < token.size()) return false;
    if (text.compare(*index, token.size(), token) != 0) return false;
    *index += token.size();
    return true;
}

bool IsSpaceCodePoint(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000 ||
           cp == 0x00A0 || cp == 0x0B || cp == 0x0C;
}

std::string Trimmed(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto cp = DecodeUtf8(text, begin);
        if (!IsSpaceCodePoint(cp.value)) break;
        begin += cp.length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        std::size_t start = end - 1;
        while (start > begin && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        const auto cp = DecodeUtf8(text, start);
        if (!IsSpaceCodePoint(cp.value)) break;
        end = start;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string FullWidthDigitsToAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto cp = DecodeUtf8(text, pos);
        if (cp.value >= 0xFF10 && cp.value <= 0xFF19) {
            out.push_back(static_cast<char>('0' + (cp.value - 0xFF10)));
        } else {
            out.append(text.substr(pos, cp.length));
        }
        pos += cp.length;
    }
    return out;
}

// 中文数字表（两 → 2；与 macOS VoiceEditNumberParsing 同口径）。
int ChineseDigit(char32_t cp) {
    switch (cp) {
        case U'零': return 0;
        case U'〇': return 0;
        case U'一': return 1;
        case U'二': return 2;
        case U'两': return 2;
        case U'三': return 3;
        case U'四': return 4;
        case U'五': return 5;
        case U'六': return 6;
        case U'七': return 7;
        case U'八': return 8;
        case U'九': return 9;
        default: return -1;
    }
}

bool CodePointAt(std::string_view text, std::size_t index, char32_t* out) {
    if (index >= text.size()) return false;
    *out = DecodeUtf8(text, index).value;
    return true;
}

}  // namespace

// ============================ 序数数字解析 ============================

std::optional<int> ParseVoiceEditOrdinalNumber(std::string_view text,
                                               std::size_t* index, int max) {
    if (*index >= text.size()) return std::nullopt;
    char32_t first = DecodeUtf8(text, *index).value;
    // 阿拉伯数字（全角由调用方归一化转半角）。
    if (first >= U'0' && first <= U'9') {
        int value = 0;
        std::size_t probe = *index;
        while (probe < text.size()) {
            char32_t cp = DecodeUtf8(text, probe).value;
            if (cp < U'0' || cp > U'9') break;
            value = value * 10 + static_cast<int>(cp - U'0');
            if (value > max) return std::nullopt;  // 越界整串不识别
            probe += DecodeUtf8(text, probe).length;
        }
        if (value < 1) return std::nullopt;
        *index = probe;
        return value;
    }
    // 中文数字：十 / 十Y / X / X十 / X十Y。
    std::size_t probe = *index;
    int value = 0;
    if (first == U'十') {
        value = 10;
        probe += DecodeUtf8(text, probe).length;
        char32_t next = 0;
        if (CodePointAt(text, probe, &next)) {
            const int unit = ChineseDigit(next);
            if (unit >= 1 && unit <= 9) {
                value += unit;
                probe += DecodeUtf8(text, probe).length;
            }
        }
    } else {
        const int digit = ChineseDigit(first);
        if (digit < 1 || digit > 9) return std::nullopt;
        value = digit;
        probe += DecodeUtf8(text, probe).length;
        char32_t next = 0;
        if (CodePointAt(text, probe, &next) && next == U'十') {
            value = digit * 10;
            probe += DecodeUtf8(text, probe).length;
            if (CodePointAt(text, probe, &next)) {
                const int unit = ChineseDigit(next);
                if (unit >= 1 && unit <= 9) {
                    value += unit;
                    probe += DecodeUtf8(text, probe).length;
                }
            }
        }
    }
    if (value < 1 || value > max) return std::nullopt;
    *index = probe;
    return value;
}

// ============================ 搜索域解析 ============================

VoiceTargetedDomainOutcome ResolveVoiceTargetedDomain(
    const VoiceTargetedCommand& command,
    const VoiceTargetedRecentDomain* recent,
    std::string_view field_text) {
    // 批量分支（Doc/Plan/voice-edit-batch-replace.md）：kAll / kRange 命中 ≥1 处时
    // 返回 batch；非批量位置返回 nullopt 交单点逻辑。
    const auto resolve_batch = [&](const std::vector<VoiceTargetedOccurrence>& matches,
                                   VoiceTargetedDomainKind domain_kind)
        -> std::optional<VoiceTargetedDomainOutcome> {
        if (command.position != VoiceTargetedPosition::kAll &&
            command.position != VoiceTargetedPosition::kRange) {
            return std::nullopt;
        }
        VoiceTargetedDomainOutcome result;
        result.domain = domain_kind;
        bool too_many = false;
        auto batch = SelectTargetedBatch(command, matches, &result.count, &too_many);
        if (!batch.has_value()) {
            result.kind = VoiceTargetedDomainOutcome::Kind::kFailure;
            result.failure = too_many ? VoiceTargetedFailure::kBatchTooMany
                                      : VoiceTargetedFailure::kPositionOutOfRange;
            return result;
        }
        result.kind = VoiceTargetedDomainOutcome::Kind::kExecute;
        result.is_batch = true;
        result.batch = std::move(*batch);
        return result;
    };

    // 整框域通用分支：超长保护 → 匹配 → 按位置选择。
    const auto resolve_whole_field = [&]() -> VoiceTargetedDomainOutcome {
        if (Utf16Length(field_text) > kVoiceTargetedFieldScopeMaxUtf16) {
            VoiceTargetedDomainOutcome failure;
            failure.kind = VoiceTargetedDomainOutcome::Kind::kFailure;
            failure.failure = VoiceTargetedFailure::kFieldTooLong;
            return failure;
        }
        const auto matches = FindTargetedOccurrences(command.target, field_text);
        if (matches.empty()) {
            VoiceTargetedDomainOutcome failure;
            failure.kind = VoiceTargetedDomainOutcome::Kind::kFailure;
            failure.failure = VoiceTargetedFailure::kNotFound;
            return failure;
        }
        if (auto batch = resolve_batch(matches, VoiceTargetedDomainKind::kWholeField)) {
            return *batch;
        }
        VoiceTargetedDomainOutcome result;
        result.domain = VoiceTargetedDomainKind::kWholeField;
        switch (SelectTargetedOccurrence(command, matches, &result.selected, &result.count)) {
            case VoiceTargetedPositionOutcome::kSelected:
                result.kind = VoiceTargetedDomainOutcome::Kind::kExecute;
                return result;
            case VoiceTargetedPositionOutcome::kAmbiguous:
                result.kind = VoiceTargetedDomainOutcome::Kind::kClarify;
                return result;
            case VoiceTargetedPositionOutcome::kOutOfRange:
                result.kind = VoiceTargetedDomainOutcome::Kind::kOutOfRange;
                return result;
        }
        return result;
    };

    // 最近一次语音输入域分支（调用方保证 recent 可用）。
    const auto resolve_recent = [&](const VoiceTargetedRecentDomain& domain)
        -> VoiceTargetedDomainOutcome {
        const auto matches = FindTargetedOccurrences(command.target, domain.remaining_text);
        if (matches.empty()) {
            VoiceTargetedDomainOutcome failure;
            failure.kind = VoiceTargetedDomainOutcome::Kind::kFailure;
            failure.failure = VoiceTargetedFailure::kNotFound;
            return failure;
        }
        if (auto batch = resolve_batch(matches, VoiceTargetedDomainKind::kLastInjection)) {
            return *batch;
        }
        VoiceTargetedDomainOutcome result;
        result.domain = VoiceTargetedDomainKind::kLastInjection;
        switch (SelectTargetedOccurrence(command, matches, &result.selected, &result.count)) {
            case VoiceTargetedPositionOutcome::kSelected:
                result.kind = VoiceTargetedDomainOutcome::Kind::kExecute;
                return result;
            case VoiceTargetedPositionOutcome::kAmbiguous:
                result.kind = VoiceTargetedDomainOutcome::Kind::kClarify;
                return result;
            case VoiceTargetedPositionOutcome::kOutOfRange:
                result.kind = VoiceTargetedDomainOutcome::Kind::kOutOfRange;
                return result;
        }
        return result;
    };

    if (command.has_explicit_scope) {
        // 显式作用域：可定位 → 严格限于最近一次语音输入（目标不在其中 → not_found，
        // 不回落）；不可定位（被改过/歧义/过期/无记录）→ 整框。
        if (recent != nullptr) return resolve_recent(*recent);
        return resolve_whole_field();
    }
    if (command.position == VoiceTargetedPosition::kUnspecified) {
        // 无作用域无位置：Tier 1（recent 可用且恰 1 处）→ 否则整框。
        if (recent != nullptr) {
            const auto matches =
                FindTargetedOccurrences(command.target, recent->remaining_text);
            if (matches.size() == 1) {
                VoiceTargetedDomainOutcome result;
                result.kind = VoiceTargetedDomainOutcome::Kind::kExecute;
                result.domain = VoiceTargetedDomainKind::kLastInjection;
                result.selected = matches.front();
                return result;
            }
        }
        return resolve_whole_field();
    }
    // 无作用域 + 位置限定：整框按文档顺序（不启用 Tier 1）。
    return resolve_whole_field();
}

// ============================ 光标恢复平移 ============================

std::size_t VoiceTargetedRestoredCaret(std::size_t original_caret_utf16,
                                       std::size_t range_start_utf16,
                                       std::size_t range_length_utf16,
                                       std::size_t inserted_length_utf16) {
    if (original_caret_utf16 <= range_start_utf16) return original_caret_utf16;
    const std::size_t range_end = range_start_utf16 + range_length_utf16;
    if (original_caret_utf16 >= range_end) {
        return original_caret_utf16 - range_length_utf16 + inserted_length_utf16;
    }
    return range_start_utf16 + inserted_length_utf16;
}

// ============================ 整框编辑 → remaining 映射 ============================

std::string MapWholeFieldEditToRemaining(std::string_view remaining_text,
                                         std::size_t rel_start_utf16,
                                         std::size_t length_utf16,
                                         std::string_view replacement_utf8) {
    // UTF-16 偏移 → 字节偏移（解码前进累计）。
    const auto byte_offset_at = [](std::string_view text,
                                   std::size_t target_utf16) -> std::size_t {
        std::size_t utf16_offset = 0;
        std::size_t pos = 0;
        while (pos < text.size()) {
            if (utf16_offset >= target_utf16) return pos;
            const auto decoded = DecodeUtf8(text, pos);
            utf16_offset += decoded.value <= 0xFFFF ? 1 : 2;
            pos += decoded.length;
        }
        return text.size();
    };
    const std::size_t start_bytes = byte_offset_at(remaining_text, rel_start_utf16);
    const std::size_t end_bytes = byte_offset_at(remaining_text,
                                                 rel_start_utf16 + length_utf16);
    std::string out;
    out.reserve(remaining_text.size());
    out.append(remaining_text.substr(0, start_bytes));
    out.append(replacement_utf8);
    out.append(remaining_text.substr(end_bytes));
    return out;
}

bool VoiceTargetedWholeFieldMatches(std::string_view actual,
                                    std::string_view expected) {
    if (actual == expected) return true;
    // Chromium 系 contenteditable 的空文档 GetText 返回占位段落换行（ZCode
    // 真机实测 "\n"），删除目标全文为空时它不代表有内容；仅在这种情况下
    // 容忍纯换行差异。非空期望严格比较——真实差异必须报 result_unconfirmed。
    if (!expected.empty()) return false;
    for (char c : actual) {
        if (c != '\n' && c != '\r') return false;
    }
    return true;
}

// ============================ 序数应答解析 ============================

namespace {

constexpr std::string_view kReplyPrefixes[] = {"就是", "选择", "就", "选", "是", "要", "改"};
constexpr std::string_view kReplySuffixes[] = {"就行", "那个", "吧", "啊", "呀", "的"};
constexpr std::string_view kReplyTrailingPunctuation[] = {
    "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、"};
constexpr std::string_view kReplyCancelForms[] = {"取消", "算了", "不改了", "不用了", "不要了"};

bool EndsWithToken(std::string_view text, std::string_view token) {
    return text.size() >= token.size() &&
           text.compare(text.size() - token.size(), token.size(), token) == 0;
}

bool StripSuffixToken(std::string* text, std::string_view token) {
    if (!EndsWithToken(*text, token)) return false;
    text->erase(text->size() - token.size());
    return true;
}

// 剩余部分为空，或恰为 pending 目标词（「第二个北京」）。
bool MatchOptionalTarget(std::string_view rest, std::string_view target) {
    if (rest.empty()) return true;
    if (target.empty()) return false;
    return rest == target;
}

// 单位（个/处，可省，至多一个）+ 可选目标词。
bool MatchUnitAndOptionalTarget(std::string_view rest, std::string_view target) {
    if (StartsWith(rest, "个")) {
        return MatchOptionalTarget(rest.substr(std::string_view("个").size()), target);
    }
    if (StartsWith(rest, "处")) {
        return MatchOptionalTarget(rest.substr(std::string_view("处").size()), target);
    }
    return MatchOptionalTarget(rest, target);
}

// 批量应答（Doc/Plan/voice-edit-batch-replace.md）：全部 / 范围。
constexpr std::string_view kReplyAllForms[] = {"全部", "所有", "都"};
constexpr std::string_view kReplyRangeSeparators[] = {"到", "至"};

// 追问应答的范围端点：第N个/处 / 倒数第N个/处 → >0 正序、<0 倒数。
std::optional<int> ParseReplyRangeEndpoint(std::string_view text, std::size_t* index) {
    std::size_t probe = *index;
    bool from_end = false;
    if (ConsumeToken(text, &probe, "倒数第")) {
        from_end = true;
    } else if (!ConsumeToken(text, &probe, "第")) {
        return std::nullopt;
    }
    const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
    if (!n.has_value()) return std::nullopt;
    if (!ConsumeToken(text, &probe, "个") && !ConsumeToken(text, &probe, "处")) {
        return std::nullopt;
    }
    *index = probe;
    return from_end ? -*n : *n;
}

}  // namespace

std::optional<VoiceEditOrdinalReply> ParseVoiceEditOrdinalReply(std::string_view text,
                                                                std::string_view target) {
    std::string body = FullWidthDigitsToAscii(Trimmed(text));
    // 剥尾部标点（可叠加，每次剥后再 trim）。
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto token : kReplyTrailingPunctuation) {
            if (StripSuffixToken(&body, token)) {
                body = Trimmed(body);
                changed = true;
                break;
            }
        }
    }
    if (body.empty()) return std::nullopt;
    // 剥一层前缀（最长优先）与一层尾缀（最长优先）。
    for (const auto prefix : kReplyPrefixes) {
        if (StartsWith(body, prefix)) {
            body.erase(0, prefix.size());
            break;
        }
    }
    for (const auto suffix : kReplySuffixes) {
        if (StripSuffixToken(&body, suffix)) {
            break;
        }
    }
    body = Trimmed(body);
    if (body.empty()) return std::nullopt;

    // 取消类（整词精确匹配）。
    for (const auto form : kReplyCancelForms) {
        if (body == form) {
            return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kCancel, 0};
        }
    }

    const std::string_view view = body;
    // 全部（整词；可带目标词后缀）。
    for (const auto form : kReplyAllForms) {
        if (view == form || view == std::string(form) + std::string(target)) {
            return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kAll, 0};
        }
    }
    // 范围：从? 第A到第B（端点支持 第N / 倒数第N）。
    {
        std::size_t index = 0;
        ConsumeToken(view, &index, "从");
        const auto start = ParseReplyRangeEndpoint(view, &index);
        bool consumed_separator = false;
        if (start.has_value()) {
            for (const auto separator : kReplyRangeSeparators) {
                if (ConsumeToken(view, &index, separator)) {
                    consumed_separator = true;
                    break;
                }
            }
        }
        if (consumed_separator) {
            const auto end = ParseReplyRangeEndpoint(view, &index);
            if (end.has_value() && MatchUnitAndOptionalTarget(view.substr(index), target)) {
                VoiceEditOrdinalReply reply;
                reply.kind = VoiceEditOrdinalReply::Kind::kRange;
                reply.range_start = *start;
                reply.range_end = *end;
                return reply;
            }
        }
    }
    constexpr std::string_view kDi = "第";
    constexpr std::string_view kDaoShuDi = "倒数第";
    constexpr std::string_view kZuiHou = "最后";
    // 倒数第 N 个/处/N（+ 可选目标词后缀）。
    {
        std::size_t index = 0;
        if (ConsumeToken(view, &index, kDaoShuDi)) {
            const auto n = ParseVoiceEditOrdinalNumber(view, &index, 99);
            if (n.has_value() && MatchUnitAndOptionalTarget(view.substr(index), target)) {
                return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kFromEnd, *n};
            }
            return std::nullopt;
        }
    }
    // 最后一个 / 最后那个 / 最后一处（→ fromEnd(1)，+ 可选目标词后缀）。
    //「最后那个」会先被尾部语气剥成裸「最后」——同样按 fromEnd(1) 接受。
    {
        std::size_t index = 0;
        if (ConsumeToken(view, &index, kZuiHou)) {
            std::string_view rest = view.substr(index);
            if (StartsWith(rest, "一个")) {
                rest = rest.substr(std::string_view("一个").size());
            } else if (StartsWith(rest, "那个")) {
                rest = rest.substr(std::string_view("那个").size());
            } else if (StartsWith(rest, "一处")) {
                rest = rest.substr(std::string_view("一处").size());
            }
            if (MatchOptionalTarget(rest, target)) {
                return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kFromEnd, 1};
            }
            return std::nullopt;
        }
    }
    // 第 N 个/处/N（+ 可选目标词后缀）。
    {
        std::size_t index = 0;
        if (ConsumeToken(view, &index, kDi)) {
            const auto n = ParseVoiceEditOrdinalNumber(view, &index, 99);
            if (n.has_value() && MatchUnitAndOptionalTarget(view.substr(index), target)) {
                return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kOrdinal, *n};
            }
            return std::nullopt;
        }
    }
    // 裸数字（中文一…二十 / 阿拉伯 1–20；须整串恰为数字）。
    {
        std::size_t index = 0;
        const auto n = ParseVoiceEditOrdinalNumber(view, &index, 20);
        if (n.has_value() && index == view.size()) {
            return VoiceEditOrdinalReply{VoiceEditOrdinalReply::Kind::kOrdinal, *n};
        }
    }
    return std::nullopt;
}

}  // namespace voicestick
