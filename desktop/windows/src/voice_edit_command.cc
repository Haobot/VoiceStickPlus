#include "voice_edit_command.h"

#include <algorithm>
#include <cstdint>
#include <span>

#include "utf8_grapheme.h"

namespace voicestick {

using utf8::CodePoint;
using utf8::DecodeUtf8;
using utf8::GraphemeSpan;
using utf8::IsCombiningMark;
using utf8::IsRegionalIndicator;
using utf8::SplitGraphemes;

// ============================ UTF-8 / grapheme ============================

namespace {

bool IsSentenceDelimiter(std::string_view token) {
    // 与 macOS sentenceTerminators 一致：。！？；+ ASCII !?; + 换行（\r 已被
    // NormalizeVoiceEditLineBreaks 归一为 \n）。
    return token == "\n" || token == "。" || token == "！" || token == "？" ||
           token == "；" || token == "!" || token == "?" || token == ";";
}

// ============================ 解析器白名单 ============================
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceEditCommand.swift 逐字一致
//（双端同向量：任一侧增删词条必须同步另一侧与两端的测试表）。

// 动词白名单（方案 §5.1）；多字动词在前，“删”最后（单独“删”只在带数量时接受）。
constexpr std::string_view kVerbs[] = {
    "删除", "删掉", "删去", "删了", "去掉", "移除", "清除", "取消", "删",
};
constexpr std::string_view kPositionWords[] = {"最后", "末尾", "结尾", "后面"};
constexpr std::string_view kDemonstratives[] = {"这", "那"};
constexpr std::string_view kClassifiers[] = {"个"};
constexpr std::string_view kCharUnits[] = {"字符", "汉字", "文字", "字"};
constexpr std::string_view kSentenceUnits[] = {"句话", "句子", "句"};
constexpr std::string_view kPolitePrefixes[] = {
    "请帮我", "麻烦帮我", "帮我", "麻烦", "请",
};
constexpr std::string_view kTrailingParticles[] = {"吧", "啊", "呀", "哦", "嘛"};
constexpr std::string_view kTrailingPunctuation[] = {
    "。", "．", ".", "！", "!", "？", "?", "，", ",", "；", ";", "、",
};

// 单独动词 → 默认删除字数（决策点 D1：default_chars 默认 1）。
constexpr std::string_view kBareDeleteForms[] = {
    "删除", "删掉", "去掉", "删除一下", "删掉一下", "去掉一下",
};

// 整段删除白名单（“撤销/撤回”= 撤销本次输入；方案 §5.1 表格 + D1）。
// 全部为整句精确匹配，不参与其它语法组合（与 macOS deleteAllForms 39 条逐字一致）。
constexpr std::string_view kDeleteAllForms[] = {
    "删除刚才的", "删除刚才", "删掉刚才的", "删掉刚才",
    "删除刚才的内容", "删掉刚才的内容", "删除刚才说的", "删掉刚才说的",
    "删除刚才输入", "删除刚才输入的", "删掉刚才输入的",
    "删除刚输入的", "删掉刚输入的", "删除上次输入", "删掉上次输入",
    "删除本次输入", "删掉本次输入", "删除这次输入", "删掉这次输入",
    "删除全部", "删掉全部", "全部删除", "全部删掉", "都删掉", "都删除",
    "删除所有", "删掉所有", "清空", "清空刚才的", "撤销", "撤回",
    "撤销刚才的", "撤回刚才的",
    "把刚才的删掉", "把刚才删掉", "把刚才的删除",
    "把刚才输入的删掉", "把本次输入删掉", "把这次输入删掉",
};

// 指令文本长度上限（grapheme）：避免长句里偶然出现白名单词形（Doc §5.3 #2）。
constexpr std::size_t kMaxCommandGraphemes = 24;

struct DigitToken {
    std::string_view token;
    int value = 0;
};

constexpr DigitToken kDigitTokens[] = {
    {"零", 0}, {"〇", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"俩", 2},
    {"三", 3}, {"四", 4}, {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
};

// ============================ 词法工具 ============================

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool MatchToken(std::string_view text, std::size_t* index, std::string_view token) {
    if (*index > text.size() || text.size() - *index < token.size()) return false;
    if (text.compare(*index, token.size(), token) != 0) return false;
    *index += token.size();
    return true;
}

// 依次尝试一组 token，命中即消费并返回（最长匹配由列表顺序保证，与 macOS 一致）。
std::optional<std::string_view> MatchFirstToken(std::string_view text, std::size_t* index,
                                                std::span<const std::string_view> tokens) {
    for (const auto token : tokens) {
        std::size_t probe = *index;
        if (MatchToken(text, &probe, token)) {
            *index = probe;
            return token;
        }
    }
    return std::nullopt;
}

bool StripSuffix(std::string* text, std::string_view suffix) {
    if (text->size() < suffix.size()) return false;
    const std::size_t offset = text->size() - suffix.size();
    if (text->compare(offset, suffix.size(), suffix) != 0) return false;
    text->erase(offset);
    return true;
}

// 去全部空白（含全角空格 U+3000 / NBSP）：语音指令匹配忽略内部空白
//（“最后 2 个字”）；真正的正文不会被本解析器改写。
std::string StripWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const CodePoint cp = DecodeUtf8(text, pos);
        const bool is_space = cp.value == U' ' || cp.value == U'\t' || cp.value == U'\r' ||
                              cp.value == U'\n' || cp.value == 0x3000 || cp.value == 0x00A0;
        if (!is_space) out.append(text.substr(pos, cp.length));
        pos += cp.length;
    }
    return out;
}

std::string FullWidthDigitsToAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const CodePoint cp = DecodeUtf8(text, pos);
        if (cp.value >= 0xFF10 && cp.value <= 0xFF19) {
            out.push_back(static_cast<char>('0' + (cp.value - 0xFF10)));
        } else {
            out.append(text.substr(pos, cp.length));
        }
        pos += cp.length;
    }
    return out;
}

// macOS normalize：trim → 循环去尾部标点（每次 trim）→ 全角数字转半角 → 去空白。
// 本实现先去掉全部空白再循环去尾部标点，结果等价。
std::string NormalizeCommandText(std::string_view text) {
    std::string out = StripWhitespace(text);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto token : kTrailingPunctuation) {
            if (StripSuffix(&out, token)) {
                changed = true;
                break;
            }
        }
    }
    return FullWidthDigitsToAscii(out);
}

// 数量解析（阿拉伯数字连续 / 中文数字 十/十X/X十/X十Y）：与 macOS parseNumber 一致。
std::optional<int> ParseNumberAt(std::string_view text, std::size_t* index) {
    const std::size_t start = *index;
    if (start >= text.size()) return std::nullopt;
    // 阿拉伯数字（全角已在归一化中转半角）。
    if (text[start] >= '0' && text[start] <= '9') {
        int value = 0;
        std::size_t pos = start;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + (text[pos] - '0');
            ++pos;
            if (value > 999) return std::nullopt;  // 长数字串不识别，防误判
        }
        *index = pos;
        return value;
    }
    // 中文数字：十 / 十X。
    if (text.compare(start, 3, "十") == 0) {
        std::size_t pos = start + 3;
        int value = 10;
        for (const auto& digit : kDigitTokens) {
            if (digit.value < 1 || digit.value > 9) continue;
            if (MatchToken(text, &pos, digit.token)) {
                value += digit.value;
                break;
            }
        }
        *index = pos;
        return value;
    }
    // 中文数字：X / X十 / X十Y。
    for (const auto& digit : kDigitTokens) {
        if (digit.value < 1 || digit.value > 9) continue;
        std::size_t pos = start;
        if (!MatchToken(text, &pos, digit.token)) continue;
        if (text.compare(pos, 3, "十") == 0) {
            pos += 3;
            int value = digit.value * 10;
            for (const auto& unit : kDigitTokens) {
                if (unit.value < 1 || unit.value > 9) continue;
                if (MatchToken(text, &pos, unit.token)) {
                    value += unit.value;
                    break;
                }
            }
            *index = pos;
            return value;
        }
        *index = pos;
        return digit.value;
    }
    return std::nullopt;
}

struct ObjectMatch {
    VoiceEditAction action = VoiceEditAction::kDeleteLastChars;
    int count = 0;
    VoiceEditCommandSource source = VoiceEditCommandSource::kExplicitCount;
    bool had_quantity = false;
};

// 解析 [POS] [DEMON] [QTY] [个] UNIT；数量对“字”类单位必填、对“句”类可缺省 1。
std::optional<ObjectMatch> ParseObject(std::string_view text, std::size_t* index, int limit) {
    std::size_t pos = *index;
    MatchFirstToken(text, &pos, kPositionWords);
    MatchFirstToken(text, &pos, kDemonstratives);
    const std::optional<int> quantity = ParseNumberAt(text, &pos);
    MatchFirstToken(text, &pos, kClassifiers);
    const bool had_quantity = quantity.has_value();

    std::size_t probe = pos;
    if (MatchFirstToken(text, &probe, kCharUnits)) {
        if (!quantity.has_value()) return std::nullopt;
        if (*quantity < 1 || *quantity > limit) return std::nullopt;
        *index = probe;
        return ObjectMatch{VoiceEditAction::kDeleteLastChars, *quantity,
                           VoiceEditCommandSource::kExplicitCount, true};
    }
    probe = pos;
    if (MatchFirstToken(text, &probe, kSentenceUnits)) {
        const int count = quantity.value_or(1);
        if (count < 1 || count > limit) return std::nullopt;
        *index = probe;
        return ObjectMatch{VoiceEditAction::kDeleteLastSentences, count,
                           had_quantity ? VoiceEditCommandSource::kExplicitCount
                                        : VoiceEditCommandSource::kDefaultCount,
                           had_quantity};
    }
    return std::nullopt;
}

// 动宾式：VERB [POS] [DEMON] [QTY] [个] UNIT（整句消费完）。
std::optional<VoiceEditCommand> MatchLeadingVerb(std::string_view text, int limit) {
    for (const auto verb : kVerbs) {
        std::size_t index = 0;
        if (!MatchToken(text, &index, verb)) continue;
        const auto object = ParseObject(text, &index, limit);
        if (!object) continue;
        if (index != text.size()) continue;
        // 单独“删”只在带数量时接受（方案 §5.1）。
        if (verb == "删" && !object->had_quantity) continue;
        return VoiceEditCommand{object->action, object->count, object->source};
    }
    return std::nullopt;
}

// 后置动词式：[把] [POS] [DEMON] [QTY] [个] UNIT VERB（整句消费完）。
std::optional<VoiceEditCommand> MatchTrailingVerb(std::string_view text, int limit) {
    std::size_t index = 0;
    MatchToken(text, &index, "把");
    const auto object = ParseObject(text, &index, limit);
    if (!object) return std::nullopt;
    std::optional<std::string_view> matched_verb;
    for (const auto verb : kVerbs) {
        std::size_t probe = index;
        if (MatchToken(text, &probe, verb)) {
            matched_verb = verb;
            index = probe;
            break;
        }
    }
    if (!matched_verb.has_value()) return std::nullopt;
    if (index != text.size()) return std::nullopt;
    if (*matched_verb == "删" && !object->had_quantity) return std::nullopt;
    return VoiceEditCommand{object->action, object->count, object->source};
}

}  // namespace

// ============================ 公开 API：UTF-8 ============================

bool IsValidUtf8(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[pos]);
        if (first < 0x80) {
            ++pos;
            continue;
        }
        std::size_t length = 0;
        char32_t value = 0;
        char32_t minimum = 0;
        if ((first & 0xE0) == 0xC0) {
            length = 2;
            value = first & 0x1F;
            minimum = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            length = 3;
            value = first & 0x0F;
            minimum = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            length = 4;
            value = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (pos + length > text.size()) return false;
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char cont = static_cast<unsigned char>(text[pos + i]);
            if ((cont & 0xC0) != 0x80) return false;
            value = (value << 6) | (cont & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
        pos += length;
    }
    return true;
}

std::size_t GraphemeCount(std::string_view text) {
    return SplitGraphemes(text).size();
}

std::string DropLastGraphemes(std::string_view text, std::size_t count) {
    if (count == 0) return std::string(text);
    const auto spans = SplitGraphemes(text);
    if (count >= spans.size()) return {};
    return std::string(text.substr(0, spans[spans.size() - count].offset));
}

std::string NormalizeVoiceEditLineBreaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::vector<std::size_t> VoiceEditSentenceGraphemes(std::string_view text) {
    const std::string normalized = NormalizeVoiceEditLineBreaks(text);
    const auto spans = SplitGraphemes(normalized);
    std::vector<std::size_t> sentences;
    std::size_t sentence_start = 0;
    std::size_t index = 0;
    while (index < spans.size()) {
        const std::string_view token =
            std::string_view(normalized).substr(spans[index].offset, spans[index].length);
        if (!IsSentenceDelimiter(token)) {
            ++index;
            continue;
        }
        ++index;
        // 连续句末标点（“！？”）归同一句。
        while (index < spans.size()) {
            const std::string_view next =
                std::string_view(normalized).substr(spans[index].offset, spans[index].length);
            if (!IsSentenceDelimiter(next)) break;
            ++index;
        }
        sentences.push_back(index - sentence_start);
        sentence_start = index;
    }
    if (sentence_start < spans.size()) {
        sentences.push_back(spans.size() - sentence_start);
    }
    return sentences;
}

// ============================ 公开 API：解析器 ============================

std::optional<VoiceEditCommand> ParseVoiceEditCommand(std::string_view text,
                                                     int default_chars,
                                                     int max_chars) {
    const int limit = std::max(1, max_chars);
    const int default_count = std::min(std::max(1, default_chars), limit);

    std::string command = NormalizeCommandText(text);
    if (command.empty()) return std::nullopt;
    // 长度上限：白名单词形都很短，超长文本不可能是整句指令（防长句偶然命中）。
    // 与 macOS 一致：在剥礼貌前缀/语气尾之前判定。
    if (GraphemeCount(command) > kMaxCommandGraphemes) return std::nullopt;

    // 礼貌前缀（“请/帮我/麻烦”，只剥一层，最长匹配优先 = 列表顺序）。
    for (const auto prefix : kPolitePrefixes) {
        if (StartsWith(command, prefix)) {
            command.erase(0, prefix.size());
            break;
        }
    }
    // 语气尾（“吧/啊/呀/哦/嘛”，可叠加，循环剥离）。
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto particle : kTrailingParticles) {
            if (StripSuffix(&command, particle)) {
                changed = true;
                break;
            }
        }
    }
    if (command.empty()) return std::nullopt;

    // 1) 单独“删除/删掉/去掉”（含“一下”变体）→ default_chars（D1：默认 1）。
    for (const auto form : kBareDeleteForms) {
        if (command == form) {
            return VoiceEditCommand{VoiceEditAction::kDeleteLastChars, default_count,
                                    VoiceEditCommandSource::kDefaultCount};
        }
    }
    // 2) 整段删除白名单（撤销/撤回/清空/删除本次输入…）。
    for (const auto form : kDeleteAllForms) {
        if (command == form) {
            return VoiceEditCommand{VoiceEditAction::kDeleteAll, 0,
                                    VoiceEditCommandSource::kAll};
        }
    }
    // 3) 动宾式：删除最后两个字 / 删除两个字 / 删掉最后一句 / 删除最后这句话。
    if (const auto parsed = MatchLeadingVerb(command, limit)) return parsed;
    // 4) 把字句/后置动词式：把最后两个字删掉 / 最后两个字删掉 / 把最后一句删掉。
    if (const auto parsed = MatchTrailingVerb(command, limit)) return parsed;
    return std::nullopt;
}

// ============================ 公开 API：校验模式 ============================

VoiceEditVerifyMode VoiceEditVerifyModeFromName(std::string_view name,
                                                VoiceEditVerifyMode fallback) {
    if (name == "strict") return VoiceEditVerifyMode::kStrict;
    if (name == "auto") return VoiceEditVerifyMode::kAuto;
    if (name == "off") return VoiceEditVerifyMode::kOff;
    return fallback;
}

std::string_view VoiceEditVerifyModeName(VoiceEditVerifyMode mode) {
    switch (mode) {
        case VoiceEditVerifyMode::kStrict: return "strict";
        case VoiceEditVerifyMode::kAuto: return "auto";
        case VoiceEditVerifyMode::kOff: return "off";
    }
    return "auto";
}

std::string_view VoiceEditRejectReasonName(VoiceEditRejectReason reason) {
    switch (reason) {
        case VoiceEditRejectReason::kNone: return "none";
        case VoiceEditRejectReason::kEmptyTarget: return "empty_target";
        case VoiceEditRejectReason::kFieldUnreadable: return "field_unreadable";
        case VoiceEditRejectReason::kPasswordField: return "password_field";
        case VoiceEditRejectReason::kSelectionActive: return "selection_active";
        case VoiceEditRejectReason::kCaretNotAtEnd: return "caret_not_at_end";
        case VoiceEditRejectReason::kTextMismatch: return "text_mismatch";
        case VoiceEditRejectReason::kUnverifiedNotAllowed: return "unverified_not_allowed";
        case VoiceEditRejectReason::kNothingToDelete: return "nothing_to_delete";
    }
    return "unknown";
}

// ============================ 公开 API：删除计划 ============================

namespace {

void RejectPlan(VoiceEditPlan* plan, VoiceEditRejectReason reason) {
    plan->ok = false;
    plan->reject_reason = reason;
}

struct DeleteAmount {
    int graphemes = 0;
    int sentences = 0;
};

// 删除量：chars = min(N, 可用 grapheme)；sentences = 最后 N 句的 grapheme 和；
// all = 全部（与 macOS VoiceEditPlanner.plan 的 targetChars 口径一致）。
DeleteAmount ComputeDeleteAmount(const VoiceEditCommand& command, std::string_view text) {
    const std::size_t total = GraphemeCount(text);
    if (command.action == VoiceEditAction::kDeleteAll) {
        return {static_cast<int>(total), 0};
    }
    if (command.action == VoiceEditAction::kDeleteLastChars) {
        const std::size_t want = command.count > 0 ? static_cast<std::size_t>(command.count) : 0;
        return {static_cast<int>(std::min(want, total)), 0};
    }
    const auto sentences = VoiceEditSentenceGraphemes(text);
    if (sentences.empty() || command.count <= 0) return {0, 0};
    const std::size_t take =
        std::min(static_cast<std::size_t>(command.count), sentences.size());
    std::size_t chars = 0;
    for (std::size_t i = sentences.size() - take; i < sentences.size(); ++i) {
        chars += sentences[i];
    }
    return {static_cast<int>(chars), static_cast<int>(take)};
}

// 字段尾部内容是否以 remainingText 的某个前缀结尾（取最长 k，单位=字节；两侧均为
// 合法 UTF-8 时 k 必然落在码点边界）。返回 0 = 不匹配。
std::size_t LongestRemainingPrefixSuffix(std::string_view field_body,
                                         std::string_view remaining) {
    const std::size_t max_k = std::min(field_body.size(), remaining.size());
    for (std::size_t k = max_k; k > 0; --k) {
        if (field_body.compare(field_body.size() - k, k, remaining.substr(0, k)) == 0) {
            return k;
        }
    }
    return 0;
}

}  // namespace

VoiceEditPlan PlanVoiceEdit(std::string_view remaining_text, bool pressed_enter,
                            const VoiceEditCommand& command,
                            const VoiceEditFieldState& field,
                            VoiceEditVerifyMode verify_mode,
                            bool unverified_allowed) {
    VoiceEditPlan plan;
    plan.action = command.action;
    // 行尾归一化：UIA TextPattern 用 \r 作段分隔，记录里的 auto_enter 换行是 \n。
    const std::string remaining = NormalizeVoiceEditLineBreaks(remaining_text);
    if (remaining.empty()) {
        RejectPlan(&plan, VoiceEditRejectReason::kEmptyTarget);
        return plan;
    }
    // 密码框：任何 verify_mode 都不执行（Doc §6.4 / §1.5；Windows UIA IsPassword 能力）。
    if (field.is_password) {
        RejectPlan(&plan, VoiceEditRejectReason::kPasswordField);
        return plan;
    }
    // off：不校验 L1，直接按 remainingText 估算（Doc §6.5）。
    if (verify_mode == VoiceEditVerifyMode::kOff) {
        const DeleteAmount amount = ComputeDeleteAmount(command, remaining);
        if (amount.graphemes <= 0) {
            RejectPlan(&plan, VoiceEditRejectReason::kEmptyTarget);
            return plan;
        }
        const int newline_count = pressed_enter ? 1 : 0;
        plan.ok = true;
        plan.delete_graphemes = amount.graphemes;
        plan.deleted_sentences = amount.sentences;
        plan.delete_newline = newline_count > 0;
        plan.backspace_count = amount.graphemes + newline_count;
        plan.new_remaining_text =
            DropLastGraphemes(remaining, static_cast<std::size_t>(amount.graphemes));
        plan.new_pressed_enter = false;
        plan.verified = false;
        return plan;
    }
    const bool verified_capable = field.readable;
    if (!verified_capable) {
        if (verify_mode == VoiceEditVerifyMode::kStrict) {
            RejectPlan(&plan, VoiceEditRejectReason::kFieldUnreadable);
            return plan;
        }
        // auto 不可验证：只在 unverified 时间窗内盲删（Doc §6.5）。
        if (!unverified_allowed) {
            RejectPlan(&plan, VoiceEditRejectReason::kUnverifiedNotAllowed);
            return plan;
        }
        const DeleteAmount amount = ComputeDeleteAmount(command, remaining);
        if (amount.graphemes <= 0) {
            RejectPlan(&plan, VoiceEditRejectReason::kEmptyTarget);
            return plan;
        }
        const int newline_count = pressed_enter ? 1 : 0;
        plan.ok = true;
        plan.delete_graphemes = amount.graphemes;
        plan.deleted_sentences = amount.sentences;
        plan.delete_newline = newline_count > 0;
        plan.backspace_count = amount.graphemes + newline_count;
        plan.new_remaining_text =
            DropLastGraphemes(remaining, static_cast<std::size_t>(amount.graphemes));
        plan.new_pressed_enter = false;
        plan.verified = false;
        return plan;
    }
    // ---- 可验证分支：字段尾部 + 光标位置必须通过 ----
    if (!field.selection_collapsed) {
        RejectPlan(&plan, VoiceEditRejectReason::kSelectionActive);
        return plan;
    }
    if (!field.caret_at_end) {
        RejectPlan(&plan, VoiceEditRejectReason::kCaretNotAtEnd);
        return plan;
    }
    std::string field_body = NormalizeVoiceEditLineBreaks(field.tail_text);
    bool newline_present = false;
    if (pressed_enter && !field_body.empty() && field_body.back() == '\n') {
        newline_present = true;
        field_body.pop_back();
    }
    const std::size_t effective_bytes = LongestRemainingPrefixSuffix(field_body, remaining);
    if (effective_bytes == 0) {
        RejectPlan(&plan, VoiceEditRejectReason::kTextMismatch);
        return plan;
    }
    const std::string_view effective(remaining.data(), effective_bytes);
    const DeleteAmount amount = ComputeDeleteAmount(command, effective);
    // 不越过注入边界：N 永远 clamp 到 effective 的 grapheme 数；无可删内容 → 拒绝
    //（与 macOS plan 的 deleteChars > 0 守卫一致，不单独为换行发退格）。
    if (amount.graphemes <= 0) {
        RejectPlan(&plan, VoiceEditRejectReason::kTextMismatch);
        return plan;
    }
    const int newline_count = newline_present ? 1 : 0;
    plan.ok = true;
    plan.delete_graphemes = amount.graphemes;
    plan.deleted_sentences = amount.sentences;
    plan.delete_newline = newline_count > 0;
    plan.backspace_count = amount.graphemes + newline_count;
    plan.new_remaining_text =
        DropLastGraphemes(effective, static_cast<std::size_t>(amount.graphemes));
    // 可验证分支：字段里没有尾随换行（或本次已删），记录一律置 false，防止之后降级
    // 到 unverified 时重复多发退格（比方案伪码更保守，与 macOS 一致）。
    plan.new_pressed_enter = false;
    plan.verified = true;
    return plan;
}

}  // namespace voicestick
