#include "voice_targeted_edit.h"

#include <algorithm>
#include <span>

#include "utf8_grapheme.h"

namespace voicestick {

using utf8::DecodeUtf8;
using utf8::GraphemeSpan;
using utf8::SplitGraphemes;
using utf8::Utf16Length;

std::size_t VoiceTargetedUtf16Length(std::string_view text) {
    return Utf16Length(text);
}

namespace {

// ============================ 解析器白名单 ============================
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceTargetedEdit.swift 逐字一致
//（双端同向量：任一侧增删词条必须同步另一侧与两端的测试表）。

constexpr std::string_view kScopeTokens[] = {"刚才的", "刚才", "刚刚的", "刚刚"};
constexpr std::string_view kPositionFirst = "第一个";
constexpr std::string_view kPositionLast = "最后一个";
// 前行动词（动宾式「VERB 刚才的 …」）。
constexpr std::string_view kLeadingDeleteVerbs[] = {"删除", "删掉", "删去", "去掉"};
// 后置删除动词（把字句尾部）。
constexpr std::string_view kTrailingDeleteVerbs[] = {"删掉", "删除", "去掉", "删去"};
// 替换分隔符（把字句尾部，replacement 为其余全部内容）。
constexpr std::string_view kReplaceSeparators[] = {"替换成", "替换为", "改成", "改为", "换成"};
constexpr std::string_view kPolitePrefixes[] = {"请帮我", "麻烦帮我", "帮我", "麻烦", "请"};
constexpr std::string_view kTrailingParticles[] = {"吧", "啊", "呀", "哦", "嘛"};
constexpr std::string_view kTrailingPunctuation[] = {
    "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、",
};

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool StripSuffix(std::string* text, std::string_view suffix) {
    if (text->size() < suffix.size()) return false;
    const std::size_t offset = text->size() - suffix.size();
    if (text->compare(offset, suffix.size(), suffix) != 0) return false;
    text->erase(offset);
    return true;
}

bool IsTrailingParticle(std::string_view token) {
    for (const auto particle : kTrailingParticles) {
        if (token == particle) return true;
    }
    return false;
}

// 最后一个码点（逆序跳过 UTF-8 续行字节），*start_out 回填其起始字节偏移。
utf8::CodePoint LastCodePoint(std::string_view text, std::size_t* start_out) {
    std::size_t start = text.size();
    while (start > 0) {
        --start;
        if ((static_cast<unsigned char>(text[start]) & 0xC0) != 0x80) break;
    }
    if (start_out != nullptr) *start_out = start;
    return DecodeUtf8(text, start);
}

// 去首尾空白（含全角空格 U+3000 / NBSP）；操作数内部空白保留。
std::string TrimWhitespace(std::string_view text) {
    auto is_space = [](char32_t cp) {
        return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000 ||
               cp == 0x00A0;
    };
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto cp = DecodeUtf8(text, begin);
        if (!is_space(cp.value)) break;
        begin += cp.length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        // 从尾部找最后一个码点的起点（ASCII 直接退 1；UTF-8 续行字节跳过）。
        std::size_t start = end - 1;
        while (start > begin && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        const auto cp = DecodeUtf8(text, start);
        if (!is_space(cp.value)) break;
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

// 归一化：去首尾空白 → 循环去尾部标点（每次去后再 trim）→ 全角数字转半角。
// 与 macOS normalize 等价：**操作数内部有意义空白保留**（不照搬旧解析器的
// 全量去空白——targeted 操作数可能是含空格的短语）。
std::string NormalizeCommandText(std::string_view text) {
    std::string out = TrimWhitespace(text);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto token : kTrailingPunctuation) {
            if (StripSuffix(&out, token)) {
                out = TrimWhitespace(out);
                changed = true;
                break;
            }
        }
    }
    return FullWidthDigitsToAscii(out);
}

bool ConsumeToken(std::string_view text, std::size_t* index, std::string_view token) {
    if (*index > text.size() || text.size() - *index < token.size()) return false;
    if (text.compare(*index, token.size(), token) != 0) return false;
    *index += token.size();
    return true;
}

bool ConsumeAny(std::string_view text, std::size_t* index, std::span<const std::string_view> tokens) {
    for (const auto token : tokens) {
        if (ConsumeToken(text, index, token)) return true;
    }
    return false;
}

struct TokenHit {
    std::size_t index = 0;
    std::string_view token;
};

// 找**最后一个**动词/分隔符 token 的出现位置（与 macOS lastTokenIndex 同语义）。
// 匹配成功即推进到 token 之后（不额外跳一个 grapheme），保证紧邻的下一个 token
// 仍会被扫描到（「把刚才的删除删掉」的句尾动词必须成为 best）；循环条件保证
// index < text.size()，ConsumeToken 不会越界。
std::optional<TokenHit> LastTokenIndex(std::string_view text,
                                       std::span<const std::string_view> tokens) {
    std::optional<TokenHit> best;
    std::size_t index = 0;
    while (index < text.size()) {
        bool matched = false;
        for (const auto token : tokens) {
            std::size_t probe = index;
            if (ConsumeToken(text, &probe, token)) {
                best = TokenHit{index, token};
                index = probe;
                matched = true;
                break;
            }
        }
        if (!matched) {
            index += DecodeUtf8(text, index).length;
        }
    }
    return best;
}

bool ContainsAnyToken(std::string_view text, std::span<const std::string_view> tokens) {
    for (const auto token : tokens) {
        std::size_t index = 0;
        while (index < text.size()) {
            if (ConsumeToken(text, &index, token)) return true;
            index += DecodeUtf8(text, index).length;
        }
    }
    return false;
}

// 否定副词检测（target 内出现「不要」或以「别」开头 → 否定式，拒绝执行）。
bool ContainsNegation(std::string_view target) {
    const std::string trimmed = TrimWhitespace(target);
    if (StartsWith(trimmed, "别")) return true;
    return trimmed.find("不要") != std::string::npos;
}

// 由 target（已定位的原始片段）构建指令；空目标/超长/空替换返回 false。
bool BuildCommand(VoiceTargetedAction action, std::string_view target_raw,
                  VoiceTargetedPosition position, bool has_explicit_scope,
                  VoiceTargetedCommand* out) {
    const std::string target = TrimWhitespace(target_raw);
    if (target.empty()) return false;
    // P0 限单行：目标/替换含换行（\n 或 \r，防 CRLF 绕过）→ 非法指令。
    if (target.find('\n') != std::string::npos || target.find('\r') != std::string::npos) {
        return false;
    }
    if (SplitGraphemes(target).size() > kVoiceTargetedMaxTargetGraphemes) return false;
    if (action.kind == VoiceTargetedActionKind::kReplace) {
        if (action.replacement.empty()) return false;
        if (action.replacement.find('\n') != std::string::npos ||
            action.replacement.find('\r') != std::string::npos) {
            return false;
        }
        if (SplitGraphemes(action.replacement).size() > kVoiceTargetedMaxReplacementGraphemes) {
            return false;
        }
    }
    out->action = std::move(action);
    out->target = target;
    out->position = position;
    out->has_explicit_scope = has_explicit_scope;
    return true;
}

// 显式作用域的异常结构吞掉并提示；自然简写结构异常时按普通文本处理。
bool FailIfScoped(bool has_explicit_scope, bool* invalid_out) {
    if (!has_explicit_scope) return false;
    *invalid_out = true;
    return true;
}

// 语法主体：A. 动宾式 VERB [SCOPE] [POSITION] TARGET；B. 把字句
// 把 [SCOPE] [POSITION] TARGET (TRAILING_DELETE_VERB | REPLACE_SEPARATOR REST)。
// SCOPE 可省略；简写由协调器按最近注入中的实际匹配决定是否接管。
// 返回 true = 结构命中（cmd_out 有效或 *invalid_out）；false = 非指令。
bool ParseBody(std::string_view text, VoiceTargetedCommand* cmd_out, bool* invalid_out) {
    if (StartsWith(text, "把")) {
        // 注意 UTF-8 字节偏移：「把」= 3 字节（Swift 版按 Character 计数）。
        std::size_t index = std::string_view("把").size();
        const bool has_explicit_scope = ConsumeAny(text, &index, kScopeTokens);
        VoiceTargetedPosition position = VoiceTargetedPosition::kUnspecified;
        if (ConsumeToken(text, &index, kPositionFirst)) {
            position = VoiceTargetedPosition::kFirst;
        } else if (ConsumeToken(text, &index, kPositionLast)) {
            position = VoiceTargetedPosition::kLast;
        }
        const auto separator = LastTokenIndex(text, kReplaceSeparators);
        if (separator.has_value()) {
            const std::string_view target = text.substr(index, separator->index - index);
            const std::string_view replacement =
                text.substr(separator->index + separator->token.size());
            if (ContainsAnyToken(target, kReplaceSeparators) ||
                ContainsAnyToken(replacement, kReplaceSeparators)) {
                return FailIfScoped(has_explicit_scope, invalid_out);
            }
            VoiceTargetedAction action{VoiceTargetedActionKind::kReplace,
                                       std::string(TrimWhitespace(replacement))};
            if (!BuildCommand(std::move(action), target, position, has_explicit_scope, cmd_out)) {
                return FailIfScoped(has_explicit_scope, invalid_out);
            }
            return true;
        }
        // 删除分支只在**没有替换分隔符**且删除动词恰好位于整句末尾时成立。
        const auto delete_verb = LastTokenIndex(text, kTrailingDeleteVerbs);
        if (delete_verb.has_value()) {
            if (delete_verb->index + delete_verb->token.size() != text.size()) return false;
            const std::string_view target = text.substr(index, delete_verb->index - index);
            if (ContainsNegation(target)) {
                return FailIfScoped(has_explicit_scope, invalid_out);
            }
            if (!BuildCommand(VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}},
                              target, position, has_explicit_scope, cmd_out)) {
                return FailIfScoped(has_explicit_scope, invalid_out);
            }
            return true;
        }
        return false;
    }
    for (const auto verb : kLeadingDeleteVerbs) {
        if (!StartsWith(text, verb)) continue;
        std::size_t index = verb.size();
        const bool has_explicit_scope = ConsumeAny(text, &index, kScopeTokens);
        VoiceTargetedPosition position = VoiceTargetedPosition::kUnspecified;
        if (ConsumeToken(text, &index, kPositionFirst)) {
            position = VoiceTargetedPosition::kFirst;
        } else if (ConsumeToken(text, &index, kPositionLast)) {
            position = VoiceTargetedPosition::kLast;
        }
        const std::string_view target = text.substr(index);
        // 「删除刚才的」与单独「删除」都交旧解析器。
        if (target.empty()) return false;
        if (ContainsNegation(target)) {
            return FailIfScoped(has_explicit_scope, invalid_out);
        }
        if (!BuildCommand(VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}}, target,
                          position, has_explicit_scope, cmd_out)) {
            return FailIfScoped(has_explicit_scope, invalid_out);
        }
        return true;
    }
    return false;
}

}  // namespace

// ============================ 公开 API：解析器 ============================

std::optional<VoiceTargetedParseResult> ParseVoiceTargetedCommand(std::string_view text) {
    std::string command = NormalizeCommandText(text);
    if (command.empty()) return std::nullopt;
    if (SplitGraphemes(command).size() > kVoiceTargetedMaxCommandGraphemes) {
        return std::nullopt;
    }
    // 礼貌前缀：仅绝对开头剥一层。
    for (const auto prefix : kPolitePrefixes) {
        if (StartsWith(command, prefix)) {
            command.erase(0, prefix.size());
            break;
        }
    }
    if (command.empty()) return std::nullopt;
    // 疑问语气尾（吗/呢）：疑问句按普通文本处理。
    {
        std::size_t last_start = 0;
        const auto last = LastCodePoint(command, &last_start);
        if (last.value == U'吗' || last.value == U'呢') return std::nullopt;
    }
    // 语气尾：剥一层再解析（与旧解析器同口径）；剥后落空再回退未剥重试。
    {
        std::size_t last_start = 0;
        LastCodePoint(command, &last_start);
        if (IsTrailingParticle(command.substr(last_start))) {
            std::string stripped = command.substr(0, last_start);
            VoiceTargetedCommand cmd;
            bool invalid = false;
            if (ParseBody(stripped, &cmd, &invalid)) {
                VoiceTargetedParseResult result;
                result.invalid = invalid;
                result.command = std::move(cmd);
                return result;
            }
        }
    }
    VoiceTargetedCommand cmd;
    bool invalid = false;
    if (!ParseBody(command, &cmd, &invalid)) return std::nullopt;
    VoiceTargetedParseResult result;
    result.invalid = invalid;
    result.command = std::move(cmd);
    return result;
}

std::string_view VoiceTargetedFailureName(VoiceTargetedFailure failure) {
    switch (failure) {
        case VoiceTargetedFailure::kNone: return "none";
        case VoiceTargetedFailure::kNotFound: return "target_not_found";
        case VoiceTargetedFailure::kAmbiguous: return "target_ambiguous";
        case VoiceTargetedFailure::kInvalid: return "target_invalid";
        case VoiceTargetedFailure::kFieldMismatch: return "field_mismatch";
        case VoiceTargetedFailure::kCaretNotEnd: return "caret_not_end";
        case VoiceTargetedFailure::kSelectionPresent: return "selection_present";
        case VoiceTargetedFailure::kVerifyUnavailable: return "verify_unavailable";
        case VoiceTargetedFailure::kResultUnconfirmed: return "result_unconfirmed";
        case VoiceTargetedFailure::kNoInjection: return "no_injection";
        case VoiceTargetedFailure::kScopeExpired: return "scope_expired";
        case VoiceTargetedFailure::kAppChanged: return "app_changed";
        case VoiceTargetedFailure::kAccessibilityDenied: return "accessibility_denied";
        case VoiceTargetedFailure::kForegroundElevated: return "foreground_elevated";
    }
    return "unknown";
}

// ============================ 公开 API：匹配器 ============================

std::vector<VoiceTargetedOccurrence> FindTargetedOccurrences(std::string_view target,
                                                             std::string_view remaining) {
    std::vector<VoiceTargetedOccurrence> out;
    if (target.empty() || remaining.empty()) return out;
    const auto haystack = SplitGraphemes(remaining);
    const auto needle = SplitGraphemes(target);
    if (needle.empty() || needle.size() > haystack.size()) return out;

    // grapheme 边界 → UTF-16 偏移表。
    std::vector<std::size_t> utf16_offsets(haystack.size() + 1, 0);
    for (std::size_t i = 0; i < haystack.size(); ++i) {
        utf16_offsets[i + 1] =
            utf16_offsets[i] + Utf16Length(remaining.substr(haystack[i].offset, haystack[i].length));
    }
    //  needle 的 grapheme 字节序列比对（注意 haystack/needle 各自的字节源）。
    auto grapheme_bytes = [](std::string_view source, const GraphemeSpan& span) {
        return source.substr(span.offset, span.length);
    };

    std::size_t search_from = 0;
    while (search_from + needle.size() <= haystack.size()) {
        bool matched = true;
        for (std::size_t offset = 0; offset < needle.size(); ++offset) {
            if (grapheme_bytes(remaining, haystack[search_from + offset]) !=
                grapheme_bytes(target, needle[offset])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            const std::size_t end = search_from + needle.size();
            VoiceTargetedOccurrence occ;
            occ.index = out.size();
            occ.grapheme_start = search_from;
            occ.grapheme_end = end;
            occ.utf16_start = utf16_offsets[search_from];
            occ.utf16_length = utf16_offsets[end] - utf16_offsets[search_from];
            out.push_back(std::move(occ));
            search_from = end;  // 非重叠
            continue;
        }
        ++search_from;
    }
    for (auto& occ : out) occ.total = out.size();
    return out;
}

// ============================ 公开 API：计划器 ============================

VoiceTargetedPlan PlanVoiceTargeted(const VoiceTargetedCommand& command,
                                    std::string_view remaining_text, bool pressed_enter,
                                    bool field_readable, std::string_view field_tail,
                                    bool caret_at_end, bool selection_collapsed) {
    VoiceTargetedPlan plan;
    plan.action = command.action;

    const auto matches = FindTargetedOccurrences(command.target, remaining_text);
    if (matches.empty()) {
        plan.failure = VoiceTargetedFailure::kNotFound;
        return plan;
    }
    VoiceTargetedOccurrence selected;
    switch (command.position) {
        case VoiceTargetedPosition::kFirst:
            selected = matches.front();
            break;
        case VoiceTargetedPosition::kLast:
            selected = matches.back();
            break;
        case VoiceTargetedPosition::kUnspecified:
            if (matches.size() != 1) {
                plan.failure = VoiceTargetedFailure::kAmbiguous;
                return plan;
            }
            selected = matches.front();
            break;
    }

    // L1：targeted 恒 verified——任何 verify_mode 下都强制字段/光标校验。
    if (!field_readable) {
        plan.failure = VoiceTargetedFailure::kVerifyUnavailable;
        return plan;
    }
    if (!selection_collapsed) {
        plan.failure = VoiceTargetedFailure::kSelectionPresent;
        return plan;
    }
    if (!caret_at_end) {
        plan.failure = VoiceTargetedFailure::kCaretNotEnd;
        return plan;
    }
    // 完整 remainingText 后缀校验（允许末尾一个**已记录** auto_enter 换行）；
    // 不使用旧「最长前缀」容错。UIA 尾部窗口比 remaining 短时后缀必然不成立 →
    // field_mismatch（安全方向，不误改）。
    bool has_newline = false;
    std::string_view body = field_tail;
    if (pressed_enter && !body.empty() && body.back() == '\n') {
        // 先按「remaining+\n」判定（body 已知以 \n 结尾），避免 remaining 自身以
        // \n 结尾时的歧义。
        const std::string_view body_no_nl = body.substr(0, body.size() - 1);
        if (body_no_nl.size() >= remaining_text.size() &&
            body_no_nl.substr(body_no_nl.size() - remaining_text.size()) == remaining_text) {
            has_newline = true;
            body = body_no_nl.substr(0, body_no_nl.size() - remaining_text.size());
        } else if (body.size() >= remaining_text.size() &&
                   body.substr(body.size() - remaining_text.size()) == remaining_text) {
            body.remove_suffix(remaining_text.size());
        } else {
            plan.failure = VoiceTargetedFailure::kFieldMismatch;
            return plan;
        }
    } else if (body.size() >= remaining_text.size() &&
               body.substr(body.size() - remaining_text.size()) == remaining_text) {
        body.remove_suffix(remaining_text.size());
    } else {
        plan.failure = VoiceTargetedFailure::kFieldMismatch;
        return plan;
    }

    // 执行后的 remainingText：grapheme 区间替换（delete=摘除；replace=插入）。
    const auto remaining_spans = SplitGraphemes(remaining_text);
    std::string new_remaining;
    new_remaining.reserve(remaining_text.size());
    for (std::size_t i = 0; i < remaining_spans.size(); ++i) {
        if (i < selected.grapheme_start || i >= selected.grapheme_end) {
            new_remaining.append(
                remaining_text.substr(remaining_spans[i].offset, remaining_spans[i].length));
        } else if (i == selected.grapheme_start) {
            if (command.action.kind == VoiceTargetedActionKind::kReplace) {
                new_remaining.append(command.action.replacement);
            }
        }
    }

    plan.ok = true;
    plan.target_graphemes = selected.grapheme_end - selected.grapheme_start;
    plan.occurrence = selected;
    plan.new_remaining_text = std::move(new_remaining);
    plan.tail_prefix = std::string(body);
    plan.has_recorded_newline = has_newline;
    plan.remaining_utf16_length = Utf16Length(remaining_text);
    return plan;
}

}  // namespace voicestick
