#include "voice_targeted_edit.h"

#include <algorithm>
#include <span>

#include "utf8_grapheme.h"
#include "voice_targeted_domain.h"

namespace voicestick {

using utf8::DecodeUtf8;
using utf8::GraphemeSpan;
using utf8::SplitGraphemes;
using utf8::Utf16Length;

std::size_t VoiceTargetedUtf16Length(std::string_view text) {
    return Utf16Length(text);
}

std::string VoiceTargetedUtf8Tail(std::string_view text, std::size_t max_utf16,
                                  std::size_t* out_utf16_length) {
    std::size_t utf16 = 0;
    std::size_t start = text.size();
    while (start > 0 && utf16 < max_utf16) {
        // 回退到上一个字符的首字节（跳过 continuation byte 0b10xxxxxx）。
        std::size_t position = start - 1;
        while (position > 0 &&
               (static_cast<unsigned char>(text[position]) & 0xC0u) == 0x80u) {
            --position;
        }
        const unsigned char lead = static_cast<unsigned char>(text[position]);
        // 4 字节 UTF-8（>= 0xF0）= 1 个码点 = 2 个 UTF-16 码元。
        const std::size_t units = lead >= 0xF0u ? 2u : 1u;
        if (utf16 + units > max_utf16) break;
        utf16 += units;
        start = position;
    }
    if (out_utf16_length != nullptr) *out_utf16_length = utf16;
    return std::string(text.substr(start));
}

VoiceTargetedPositionOutcome SelectTargetedOccurrence(
    const VoiceTargetedCommand& command,
    const std::vector<VoiceTargetedOccurrence>& matches,
    VoiceTargetedOccurrence* selected, int* count_out) {
    if (count_out != nullptr) *count_out = static_cast<int>(matches.size());
    if (matches.empty()) return VoiceTargetedPositionOutcome::kOutOfRange;
    switch (command.position) {
        case VoiceTargetedPosition::kFirst:
            *selected = matches.front();
            return VoiceTargetedPositionOutcome::kSelected;
        case VoiceTargetedPosition::kLast:
            *selected = matches.back();
            return VoiceTargetedPositionOutcome::kSelected;
        case VoiceTargetedPosition::kOrdinal: {
            if (command.position_n < 1 ||
                static_cast<std::size_t>(command.position_n) > matches.size()) {
                return VoiceTargetedPositionOutcome::kOutOfRange;
            }
            *selected = matches[static_cast<std::size_t>(command.position_n) - 1];
            return VoiceTargetedPositionOutcome::kSelected;
        }
        case VoiceTargetedPosition::kFromEnd: {
            if (command.position_n < 1 ||
                static_cast<std::size_t>(command.position_n) > matches.size()) {
                return VoiceTargetedPositionOutcome::kOutOfRange;
            }
            *selected = matches[matches.size() - static_cast<std::size_t>(command.position_n)];
            return VoiceTargetedPositionOutcome::kSelected;
        }
        case VoiceTargetedPosition::kUnspecified:
            if (matches.size() != 1) return VoiceTargetedPositionOutcome::kAmbiguous;
            *selected = matches.front();
            return VoiceTargetedPositionOutcome::kSelected;
    }
    return VoiceTargetedPositionOutcome::kAmbiguous;
}

std::optional<std::vector<VoiceTargetedOccurrence>> SelectTargetedBatch(
    const VoiceTargetedCommand& command,
    const std::vector<VoiceTargetedOccurrence>& matches, int* count_out,
    bool* too_many_out) {
    if (count_out != nullptr) *count_out = static_cast<int>(matches.size());
    if (too_many_out != nullptr) *too_many_out = false;
    if (matches.empty()) return std::nullopt;
    if (command.position == VoiceTargetedPosition::kAll) {
        if (matches.size() > static_cast<std::size_t>(kVoiceTargetedBatchMaxOccurrences)) {
            if (too_many_out != nullptr) *too_many_out = true;
            return std::nullopt;
        }
        return matches;
    }
    if (command.position != VoiceTargetedPosition::kRange) return std::nullopt;
    // 端点归一化到 1 基正序（>0 正序；<0 倒数，-1 = 最后一个）。
    const auto normalize = [&](int endpoint) -> std::optional<std::size_t> {
        if (endpoint == 0) return std::nullopt;
        const std::size_t n =
            endpoint > 0 ? static_cast<std::size_t>(endpoint)
                         : matches.size() + 1 - static_cast<std::size_t>(-endpoint);
        if (n < 1 || n > matches.size()) return std::nullopt;
        return n;
    };
    const auto start = normalize(command.range_start);
    const auto end = normalize(command.range_end);
    if (!start.has_value() || !end.has_value() || *start > *end) return std::nullopt;
    std::vector<VoiceTargetedOccurrence> batch;
    batch.reserve(*end - *start + 1);
    for (std::size_t i = *start; i <= *end; ++i) batch.push_back(matches[i - 1]);
    return batch;
}

namespace {

// ============================ 解析器白名单 ============================
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceTargetedEdit.swift 逐字一致
//（双端同向量：任一侧增删词条必须同步另一侧与两端的测试表）。

constexpr std::string_view kScopeTokens[] = {"刚才的", "刚才", "刚刚的", "刚刚"};
constexpr std::string_view kPositionFirst = "第一个";
constexpr std::string_view kPositionLast = "最后一个";
constexpr std::string_view kPositionDaoShuDi = "倒数第";
constexpr std::string_view kPositionDi = "第";
constexpr std::string_view kPositionUnitGe = "个";
constexpr std::string_view kPositionUnitChu = "处";
// 批量（Doc/Plan/voice-edit-batch-replace.md）：全部 / 范围。
constexpr std::string_view kPositionAllTokens[] = {"全部的", "所有的", "全部", "所有", "都"};
constexpr std::string_view kRangeSeparators[] = {"到", "至"};
constexpr std::string_view kRangeFrom = "从";
constexpr std::string_view kRangeHead = "头";
constexpr std::string_view kRangeFront = "前";
constexpr std::string_view kRangeLast = "最后";
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

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
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

// 位置限定词（可选；批次 2 扩展）：第一个/最后一个专词优先，其后 第N个/第N处/
// 解析一个范围端点：第N个/处 / 倒数第N个/处 → >0 正序、<0 倒数。
bool ParseRangeEndpoint(std::string_view text, std::size_t* index, int* out) {
    std::size_t probe = *index;
    if (ConsumeToken(text, &probe, kPositionDaoShuDi)) {
        const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
        if (n.has_value() && (ConsumeToken(text, &probe, kPositionUnitGe) ||
                              ConsumeToken(text, &probe, kPositionUnitChu))) {
            *index = probe;
            *out = -*n;
            return true;
        }
    }
    probe = *index;
    if (ConsumeToken(text, &probe, kPositionDi)) {
        const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
        if (n.has_value() && (ConsumeToken(text, &probe, kPositionUnitGe) ||
                              ConsumeToken(text, &probe, kPositionUnitChu))) {
            *index = probe;
            *out = *n;
            return true;
        }
    }
    return false;
}

// 位置解析（前置）：范围优先于单点（「第一个到第三个」不能被「第一个」抢走）；
// 另收「全部/所有/都」与「从A到B」「头N个/前N个/最后N个」。
// 命中消费并回填 *position/*position_n/*range_start/*range_end；未命中 index 不动。
void ConsumePosition(std::string_view text, std::size_t* index,
                     VoiceTargetedPosition* position, int* position_n,
                     int* range_start, int* range_end) {
    *position = VoiceTargetedPosition::kUnspecified;
    *position_n = 0;
    *range_start = 0;
    *range_end = 0;
    // ---- 范围：从? 第A到第B（端点支持 第N / 倒数第N 任意组合）----
    {
        std::size_t probe = *index;
        ConsumeToken(text, &probe, kRangeFrom);  // 可选的「从」
        int a = 0;
        int b = 0;
        if (ParseRangeEndpoint(text, &probe, &a) &&
            ConsumeAny(text, &probe, kRangeSeparators) &&
            ParseRangeEndpoint(text, &probe, &b)) {
            *index = probe;
            *position = VoiceTargetedPosition::kRange;
            *range_start = a;
            *range_end = b;
            return;
        }
    }
    // ---- 范围：头N个 / 前N个 → (1, N)；最后N个（N≥2）→ (-N, -1) ----
    {
        std::size_t probe = *index;
        if (ConsumeToken(text, &probe, kRangeHead) ||
            ConsumeToken(text, &probe, kRangeFront)) {
            const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
            if (n.has_value() && ConsumeToken(text, &probe, kPositionUnitGe)) {
                *index = probe;
                *position = VoiceTargetedPosition::kRange;
                *range_start = 1;
                *range_end = *n;
                return;
            }
        }
        probe = *index;
        if (ConsumeToken(text, &probe, kRangeLast)) {
            const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
            // N<=2 让位旧指令（「删除最后两个字」= 删最后 2 字，协调器旧解析优先；
            // 解析器层面保持 kUnspecified 交给旧路径）。
            if (n.has_value() && *n >= 3 && ConsumeToken(text, &probe, kPositionUnitGe)) {
                *index = probe;
                *position = VoiceTargetedPosition::kRange;
                *range_start = -*n;
                *range_end = -1;
                return;
            }
        }
    }
    // ---- 单点：第一个 / 最后一个 ----
    if (ConsumeToken(text, index, kPositionFirst)) {
        *position = VoiceTargetedPosition::kFirst;
        return;
    }
    if (ConsumeToken(text, index, kPositionLast)) {
        *position = VoiceTargetedPosition::kLast;
        return;
    }
    // ---- 全部 / 所有 / 都 ----
    for (const auto token : kPositionAllTokens) {
        if (ConsumeToken(text, index, token)) {
            *position = VoiceTargetedPosition::kAll;
            return;
        }
    }
    // 倒数第 N 个/处
    {
        std::size_t probe = *index;
        if (ConsumeToken(text, &probe, kPositionDaoShuDi)) {
            const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
            if (n.has_value() &&
                (ConsumeToken(text, &probe, kPositionUnitGe) ||
                 ConsumeToken(text, &probe, kPositionUnitChu))) {
                *index = probe;
                *position = VoiceTargetedPosition::kFromEnd;
                *position_n = *n;
                return;
            }
        }
    }
    // 第 N 个/处
    {
        std::size_t probe = *index;
        if (ConsumeToken(text, &probe, kPositionDi)) {
            const auto n = ParseVoiceEditOrdinalNumber(text, &probe, 99);
            if (n.has_value() &&
                (ConsumeToken(text, &probe, kPositionUnitGe) ||
                 ConsumeToken(text, &probe, kPositionUnitChu))) {
                *index = probe;
                *position = VoiceTargetedPosition::kOrdinal;
                *position_n = *n;
                return;
            }
        }
    }
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
                  VoiceTargetedPosition position, int position_n,
                  int range_start, int range_end, bool has_explicit_scope,
                  VoiceTargetedCommand* out) {
    std::string target = TrimWhitespace(target_raw);
    if (target.empty()) return false;
    // 后置「全部/所有/都」：自然说法「把北京全部改成上海」——剥离并升级为 kAll。
    if (position == VoiceTargetedPosition::kUnspecified) {
        for (const auto token : kPositionAllTokens) {
            if (EndsWith(target, token) && target.size() > token.size()) {
                target.resize(target.size() - token.size());
                target = TrimWhitespace(target);
                position = VoiceTargetedPosition::kAll;
                break;
            }
        }
    }
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
    out->position_n = position_n;
    out->range_start = range_start;
    out->range_end = range_end;
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
        int position_n = 0;
        int range_start = 0;
        int range_end = 0;
        ConsumePosition(text, &index, &position, &position_n, &range_start, &range_end);
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
            if (!BuildCommand(std::move(action), target, position, position_n, range_start,
                              range_end, has_explicit_scope, cmd_out)) {
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
                              target, position, position_n, range_start, range_end,
                              has_explicit_scope, cmd_out)) {
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
        int position_n = 0;
        int range_start = 0;
        int range_end = 0;
        ConsumePosition(text, &index, &position, &position_n, &range_start, &range_end);
        const std::string_view target = text.substr(index);
        // 「删除刚才的」与单独「删除」都交旧解析器。
        if (target.empty()) return false;
        if (ContainsNegation(target)) {
            return FailIfScoped(has_explicit_scope, invalid_out);
        }
        if (!BuildCommand(VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}}, target,
                          position, position_n, range_start, range_end, has_explicit_scope,
                          cmd_out)) {
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
        case VoiceTargetedFailure::kAnchorAmbiguous: return "anchor_ambiguous";
        case VoiceTargetedFailure::kFieldTooLong: return "field_too_long";
        case VoiceTargetedFailure::kPositionOutOfRange: return "position_out_of_range";
        case VoiceTargetedFailure::kBatchTooMany: return "batch_too_many";
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

namespace {

// UTF-8 文本中 UTF-16 偏移 target 处的字节偏移（解码前进累计；target 越界返回
// text.size()）。
std::size_t ByteOffsetAtUtf16(std::string_view text, std::size_t target_utf16) {
    std::size_t utf16_offset = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (utf16_offset >= target_utf16) return pos;
        const auto decoded = utf8::DecodeUtf8(text, pos);
        utf16_offset += decoded.value <= 0xFFFF ? 1 : 2;
        pos += decoded.length;
    }
    return text.size();
}

// 匹配 + 计划主体（两个签名共用）：occurrence 选择失败/可读性拒绝各自回填 plan。
bool BuildTargetedPlan(const VoiceTargetedCommand& command,
                       std::string_view remaining_text, bool field_readable,
                       VoiceTargetedPlan* plan, VoiceTargetedOccurrence* selected) {
    const auto matches = FindTargetedOccurrences(command.target, remaining_text);
    if (matches.empty()) {
        plan->failure = VoiceTargetedFailure::kNotFound;
        return false;
    }
    switch (SelectTargetedOccurrence(command, matches, selected)) {
        case VoiceTargetedPositionOutcome::kSelected:
            break;
        case VoiceTargetedPositionOutcome::kAmbiguous:
            plan->failure = VoiceTargetedFailure::kAmbiguous;
            return false;
        case VoiceTargetedPositionOutcome::kOutOfRange:
            plan->failure = VoiceTargetedFailure::kPositionOutOfRange;
            return false;
    }
    // L1：targeted 恒 verified——任何 verify_mode 下都强制字段可读校验。
    if (!field_readable) {
        plan->failure = VoiceTargetedFailure::kVerifyUnavailable;
        return false;
    }
    return true;
}

// 锚定结构 → 计划字段填充（执行后的 remainingText + tail 窗口内锚定拆解）。
// anchor 为 tail 窗口内 UTF-16 偏移（P1c 由调用方以字段末尾锚传入，语义与旧
// 后缀拆解逐字节一致）。
void FillTargetedPlanFromAnchor(const VoiceTargetedCommand& command,
                                std::string_view remaining_text, bool pressed_enter,
                                std::string_view field_tail, const VoiceEditAnchor& anchor,
                                const VoiceTargetedOccurrence& selected,
                                VoiceTargetedPlan* plan) {
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

    const std::size_t remaining_utf16 = Utf16Length(remaining_text);
    const std::size_t anchor_bytes_start = ByteOffsetAtUtf16(field_tail, anchor.anchor_start);
    const std::size_t anchor_bytes_end = ByteOffsetAtUtf16(field_tail, anchor.anchor_end);
    plan->ok = true;
    plan->target_graphemes = selected.grapheme_end - selected.grapheme_start;
    plan->occurrence = selected;
    plan->new_remaining_text = std::move(new_remaining);
    plan->tail_prefix = std::string(field_tail.substr(0, anchor_bytes_start));
    plan->has_recorded_newline =
        pressed_enter && anchor.anchor_end - anchor.anchor_start == remaining_utf16 + 1;
    plan->remaining_utf16_length = remaining_utf16;
    plan->anchor_start_utf16 = anchor.anchor_start;
    plan->anchor_end_utf16 = anchor.anchor_end;
    plan->anchor_after_text = std::string(field_tail.substr(anchor_bytes_end));
}

}  // namespace

VoiceTargetedPlan PlanVoiceTargeted(const VoiceTargetedCommand& command,
                                    std::string_view remaining_text, bool pressed_enter,
                                    bool field_readable, std::string_view field_tail,
                                    bool /*caret_at_end*/, bool /*selection_collapsed*/) {
    VoiceTargetedPlan plan;
    plan.action = command.action;

    VoiceTargetedOccurrence selected;
    if (!BuildTargetedPlan(command, remaining_text, field_readable, &plan, &selected)) {
        return plan;
    }
    // 完整 remainingText 后缀校验（= 定位器 P1c 字段末尾锚的等价实现：允许末尾
    // 一个**已记录** auto_enter 换行，回落裸 remaining）；不使用「最长前缀」容错。
    // UIA 尾部窗口比 remaining 短时后缀必然不成立 → field_mismatch（安全方向）。
    const auto anchor = LocateVoiceEditFieldEndAnchor(field_tail, remaining_text, pressed_enter);
    if (anchor.anchor_start == anchor.anchor_end) {
        plan.failure = VoiceTargetedFailure::kFieldMismatch;
        return plan;
    }
    FillTargetedPlanFromAnchor(command, remaining_text, pressed_enter, field_tail, anchor,
                               selected, &plan);
    return plan;
}

VoiceTargetedPlan PlanVoiceTargetedAnchored(const VoiceTargetedCommand& command,
                                            std::string_view remaining_text,
                                            bool pressed_enter, bool field_readable,
                                            std::string_view field_tail,
                                            const VoiceEditAnchor& anchor) {
    VoiceTargetedPlan plan;
    plan.action = command.action;

    VoiceTargetedOccurrence selected;
    if (!BuildTargetedPlan(command, remaining_text, field_readable, &plan, &selected)) {
        return plan;
    }
    // 锚已由调用方经 LocateVoiceEditAnchor 定出（anchored 段在字段中精确成立），
    // 这里只填计划字段（防御：锚区间退化视为不可用）。
    if (anchor.anchor_start >= anchor.anchor_end) {
        plan.failure = VoiceTargetedFailure::kFieldMismatch;
        return plan;
    }
    FillTargetedPlanFromAnchor(command, remaining_text, pressed_enter, field_tail, anchor,
                               selected, &plan);
    return plan;
}

// 整框替换构造：簇表边界精确匹配（落在簇中间 /
// 越界 → nullopt，防字素切坏），切片 前段 + replacement + 后段。
std::optional<std::string> BuildWholeFieldReplacement(std::string_view field_full_utf8,
                                                      std::size_t target_start_utf16,
                                                      std::size_t target_end_utf16,
                                                      std::string_view replacement) {
    if (target_start_utf16 >= target_end_utf16) return std::nullopt;
    const auto graphemes = utf8::SplitGraphemes(field_full_utf8);
    std::optional<std::size_t> start_byte, end_byte;
    std::size_t utf16_offset = 0;
    for (std::size_t i = 0;
         i < graphemes.size() && (!start_byte.has_value() || !end_byte.has_value()); ++i) {
        if (utf16_offset == target_start_utf16) start_byte = graphemes[i].offset;
        if (utf16_offset == target_end_utf16) end_byte = graphemes[i].offset;
        utf16_offset += utf8::Utf16Length(
            field_full_utf8.substr(graphemes[i].offset, graphemes[i].length));
    }
    if (utf16_offset == target_start_utf16) start_byte = field_full_utf8.size();
    if (utf16_offset == target_end_utf16) end_byte = field_full_utf8.size();
    if (!start_byte.has_value() || !end_byte.has_value()) return std::nullopt;
    std::string out;
    out.reserve(field_full_utf8.size() - (*end_byte - *start_byte) + replacement.size());
    out.append(field_full_utf8.substr(0, *start_byte));
    out.append(replacement);
    out.append(field_full_utf8.substr(*end_byte));
    return out;
}

std::optional<std::string> BuildWholeFieldBatchReplacement(
    std::string_view field_full_utf8,
    const std::vector<VoiceTargetedOccurrence>& occurrences,
    std::string_view replacement) {
    if (occurrences.empty()) return std::nullopt;
    const auto graphemes = utf8::SplitGraphemes(field_full_utf8);
    // UTF-16 偏移 → 字节偏移（grapheme 边界校验；越界/落在字素中间 → nullopt）。
    const auto to_byte = [&](std::size_t utf16_offset) -> std::optional<std::size_t> {
        std::size_t offset = 0;
        for (const auto& g : graphemes) {
            if (offset == utf16_offset) return g.offset;
            offset += utf8::Utf16Length(field_full_utf8.substr(g.offset, g.length));
        }
        if (offset == utf16_offset) return field_full_utf8.size();
        return std::nullopt;
    };
    std::string out;
    out.reserve(field_full_utf8.size());
    std::size_t pos = 0;
    for (const auto& occ : occurrences) {
        const auto start = to_byte(occ.utf16_start);
        const auto end = to_byte(occ.utf16_start + occ.utf16_length);
        if (!start.has_value() || !end.has_value() || *start < pos || *end <= *start) {
            return std::nullopt;  // 越界/重叠/字素切坏
        }
        out.append(field_full_utf8.substr(pos, *start - pos));
        out.append(replacement);
        pos = *end;
    }
    out.append(field_full_utf8.substr(pos));
    return out;
}

}  // namespace voicestick
