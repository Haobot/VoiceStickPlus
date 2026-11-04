#pragma once

// 语音编辑批次 2（追问「第几个」+ 整个输入框可编辑）纯逻辑层：
//   1) ParseVoiceEditOrdinalNumber —— 中文/阿拉伯序数解析（targeted 指令位置限定
//      与追问序数应答共用；与 macOS VoiceEditNumberParsing.consumeOrdinal 同口径）；
//   2) ResolveVoiceTargetedDomain   —— targeted 搜索域解析规则表（Tier 1 最近一次
//      语音输入优先 → 整框；显式作用域；位置限定；field_too_long）；
//   3) VoiceTargetedRestoredCaret   —— 编辑后光标恢复「原位置 + 平移」；
//   4) MapWholeFieldEditToRemaining —— 整框域编辑完全落在 remaining 内时的记录同步映射；
//   5) ParseVoiceEditOrdinalReply   —— 追问「第几个」的序数应答解析。
//
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceTargetedDomain.swift /
// VoiceEditOrdinalReply.swift 同构：同一张规则表与同一份测试向量（UTF-8 输入、
// grapheme/UTF-16 口径与 voice_targeted_edit 一致）。本文件与 .cc 不依赖任何
// Win32/平台头，可在 macOS 上 clang++ -std=c++20 编译并链接本地 main 跑通向量。
//
// 域解析只负责「在哪个 haystack 里找、找第几个」；锚定（最近一次语音输入可定位）
// 由协调器完成 LocateVoiceEditAnchor 后以 recent 参数传入——recent == nullptr 即
// 「不可定位」（无记录/过期/换应用/锚定失败），规则表按整框处理。field_scope 关闭
// 时协调器走批次 1 原路径，不进入本解析器（回归开关语义）。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voice_edit_anchor.h"
#include "voice_targeted_edit.h"

namespace voicestick {

// 整框域字段长度上限（UTF-16 码元）：超过即 field_too_long（与 macOS 同值）。
inline constexpr std::size_t kVoiceTargetedFieldScopeMaxUtf16 = 200000;

// ============================ 序数数字解析（共用） ============================

// 在 index 处消费一个 1..max 的序数（中文 十/十Y/X/X十/X十Y（两→2）或阿拉伯数字，
// 全角数字由调用方先转半角）；命中返回值并推进 *index，未命中返回 nullopt（*index
// 不动）。阿拉伯越界整串不识别（不部分消费）。
std::optional<int> ParseVoiceEditOrdinalNumber(std::string_view text,
                                               std::size_t* index, int max);

// ============================ 搜索域解析（交付 A 规则表） ============================

enum class VoiceTargetedDomainKind {
    kLastInjection,
    kWholeField,
};

// 计划时刻可用的「最近一次语音输入」域：L0 批次 1 闸（记录/时间窗/同前台应用）
// 全部通过且 LocateVoiceEditAnchor 锚定成功时由协调器构造。
struct VoiceTargetedRecentDomain {
    std::string remaining_text;  // LastInjection.remaining_text（UTF-8）
    VoiceEditAnchor anchor;      // 锚定结果（偏移与传入 field_text 同基准；
                                 // VoiceEditAnchor 无 operator==，按字段比较）
};

// 域解析结果（对齐 macOS VoiceTargetedDomainOutcome）。
struct VoiceTargetedDomainOutcome {
    enum class Kind {
        kExecute,      // 域内选中一处（occurrence 偏移相对域文本）
        kClarify,      // 未指定位置且域内多处 → 追问「第几个」
        kOutOfRange,   // 位置序号越界（count=域内出现次数）
        kFailure,      // not_found / field_too_long
    };
    Kind kind = Kind::kFailure;
    VoiceTargetedDomainKind domain = VoiceTargetedDomainKind::kLastInjection;
    VoiceTargetedOccurrence selected;         // kExecute 有效
    // 批量（kAll / kRange 命中 ≥1 处）：按文档顺序的全部目标区间。
    // is_batch=true 时以 batch 为准（selected 未使用）。
    std::vector<VoiceTargetedOccurrence> batch;
    bool is_batch = false;
    int count = 0;                            // kClarify / kOutOfRange 有效
    VoiceTargetedFailure failure = VoiceTargetedFailure::kNone;  // kFailure 有效
};

// 域解析纯函数（规则表见 Doc/Plan/voice-edit-commands.md §2.1；双端同构）：
//   无作用域无位置  ：Tier 1（recent 可用且 remaining 内恰 1 处）→ lastInjection；
//                     否则整框；
//   显式作用域      ：recent 可用 → lastInjection（目标不在其中 → not_found）；
//                     不可用 → 整框；
//   无作用域 + 位置 ：整框按文档顺序；
//   显式作用域+位置 ：recent 可用 → lastInjection；不可用 → 整框。
// 唯一 → execute；多处 → clarify（该域内编号）；越界 → out_of_range；整框超长
//（> kVoiceTargetedFieldScopeMaxUtf16）→ field_too_long。
VoiceTargetedDomainOutcome ResolveVoiceTargetedDomain(
    const VoiceTargetedCommand& command,
    const VoiceTargetedRecentDomain* recent,  // 可为 nullptr（不可定位）
    std::string_view field_text);

// ============================ 光标恢复平移（交付 C） ============================

// 编辑后光标 = 原光标 + 平移：原光标 ≤ 起点 → 不变；≥ 终点 → 加长度差；
// 区间内 → 编辑后区间末尾。末尾追加情形与批次 1「恢复段末/字段末」结果一致。
std::size_t VoiceTargetedRestoredCaret(std::size_t original_caret_utf16,
                                       std::size_t range_start_utf16,
                                       std::size_t range_length_utf16,
                                       std::size_t inserted_length_utf16);

// 整框域编辑完全落在 remaining 内时，把同一编辑映射到 remaining（UTF-16 拼接，
// grapheme 对齐）：rel_start/length 为编辑区间相对 remaining 的 UTF-16 偏移。
std::string MapWholeFieldEditToRemaining(std::string_view remaining_text,
                                         std::size_t rel_start_utf16,
                                         std::size_t length_utf16,
                                         std::string_view replacement_utf8);

// ============================ 序数应答解析（交付 B） ============================

struct VoiceEditOrdinalReply {
    enum class Kind {
        kOrdinal,  // 第 N 个（1 基）
        kFromEnd,  // 倒数第 N 个（1 基）
        kCancel,
        kAll,    // 批量：全部（Doc/Plan/voice-edit-batch-replace.md）
        kRange,  // 批量：范围（range_start/range_end 有效）
    };
    Kind kind = Kind::kOrdinal;
    int n = 0;
    int range_start = 0;  // kRange：>0 正序 1 基，<0 倒数
    int range_end = 0;

    bool operator==(const VoiceEditOrdinalReply& other) const = default;
};

// 仅在协调器存在 pending（20 秒有效）时调用（裸数字误触面因此极小）。
// 接受：第N个/第N处/第N/第N个X（X==target）、倒数第N个/处/N、最后一个/那个/一处
//（尾缀剥「那个」后的裸「最后」同收）、裸数字（中文一…二十/阿拉伯 1–20）；
// 前缀（就/就是/选择/选/是/要/改）与尾缀（吧/啊/呀/的/就行/那个）各剥一层；
// 剥尾部标点；取消：取消/算了/不改了/不用了/不要了。其余 nullopt（非应答，
// 按正常链路放行——绝不吞用户的正常说话）。
std::optional<VoiceEditOrdinalReply> ParseVoiceEditOrdinalReply(std::string_view text,
                                                                std::string_view target);

}  // namespace voicestick
