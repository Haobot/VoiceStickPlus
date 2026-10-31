#pragma once

// Targeted 语音编辑纯逻辑（删除/替换最近一次语音输入中的指定词/短语）：
//   1) 中文整句白名单解析（ParseVoiceTargetedCommand）——支持显式作用域与自然简写；
//   2) remainingText 内目标词非重叠精确匹配（FindTargetedOccurrences）；
//   3) L1 目标校验 + 执行计划（PlanVoiceTargeted）。
//
// 本文件与 voice_targeted_edit.cc 不依赖任何 Win32/平台头，可在 macOS 上用
// clang++ -std=c++20 编译并链接本地 main 跑通全部测试向量。
// 双端同向量：语法表（scopeTokens/动词/分隔符/礼貌前缀/语气尾/位置词/上限常量）
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceTargetedEdit.swift 逐字一致。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// ---- 双端一致常量（独立于 [voice_edit].max_chars；与 macOS 常量同值）----
constexpr std::size_t kVoiceTargetedMaxTargetGraphemes = 32;
constexpr std::size_t kVoiceTargetedMaxReplacementGraphemes = 64;
constexpr std::size_t kVoiceTargetedMaxCommandGraphemes = 128;

enum class VoiceTargetedActionKind {
    kDelete,
    kReplace,
};

struct VoiceTargetedAction {
    VoiceTargetedActionKind kind = VoiceTargetedActionKind::kDelete;
    std::string replacement;  // kReplace 时非空（UTF-8）

    bool operator==(const VoiceTargetedAction& other) const = default;
};

enum class VoiceTargetedPosition {
    kFirst,
    kLast,
    kUnspecified,  // 仅 1 个精确匹配才允许
};

struct VoiceTargetedCommand {
    VoiceTargetedAction action;
    std::string target;  // UTF-8，非空，<= kVoiceTargetedMaxTargetGraphemes graphemes
    VoiceTargetedPosition position = VoiceTargetedPosition::kUnspecified;
    // 是否显式带「刚才的/刚刚」作用域。自然简写为 false，协调器仅在最近一次
    // 注入确实存在 target 时接管，否则按普通文本处理。
    bool has_explicit_scope = true;

    bool operator==(const VoiceTargetedCommand& other) const = default;
};

// 解析三态（对齐 macOS VoiceTargetedParseResult）：
//   nullopt = 非指令（走原链路，按普通文本上屏）；
//   result.invalid = 显式作用域下结构命中但操作数非法（空目标/空替换/多分隔符/否定
//                    ——吞掉并提示，不上屏、不修改；自然简写异常归 nullopt）；
//   result.command = 可执行指令。
struct VoiceTargetedParseResult {
    bool invalid = false;
    VoiceTargetedCommand command;
};

// 整句解析（归一化：去首尾空白/尾部标点、全角数字转半角、剥绝对开头礼貌前缀一层、
// 剥绝对结尾语气词一层；操作数内部有意义空白保留）。
std::optional<VoiceTargetedParseResult> ParseVoiceTargetedCommand(std::string_view text);

enum class VoiceTargetedFailure {
    kNone,
    kNotFound,           // 目标词不在 remainingText 中
    kAmbiguous,          // 多处匹配但未指定第一个/最后一个
    kInvalid,            // 结构命中但操作数非法（解析层已吞，协调器防御复用）
    kFieldMismatch,      // 字段尾部不再以完整 remainingText 为后缀（含手工改动）
    kCaretNotEnd,        // 已不再由 targeted planner 返回（2026-10-31 光标门槛放宽）；
                         // 保留枚举位与双端失败名表/日志码对齐
    kSelectionPresent,   // 同上（选区门槛一并放宽）
    kVerifyUnavailable,  // 读不到字段/光标（targeted 恒 verified，一律拒绝）
    kResultUnconfirmed,  // 修改已发出但终验不符（记录应失效）
    // L0 作用域层（与旧 VoiceEditRejectReason 同语义；toast 复用旧 StringId）。
    kNoInjection,
    kScopeExpired,
    kAppChanged,
    kAccessibilityDenied,
    kForegroundElevated,
};

std::string_view VoiceTargetedFailureName(VoiceTargetedFailure failure);

// remainingText 内的一次非重叠精确匹配（UTF-8 字节 + grapheme + UTF-16 三口径）。
struct VoiceTargetedOccurrence {
    std::size_t index = 0;           // 第几个（0-based）
    std::size_t total = 0;           // 非重叠匹配总数
    std::size_t grapheme_start = 0;  // [start, end) grapheme 区间（相对 remaining）
    std::size_t grapheme_end = 0;
    std::size_t utf16_start = 0;     // UTF-16 码元偏移（平台选区用）
    std::size_t utf16_length = 0;
};

// 非重叠精确匹配（target 为空返回空表；匹配按 grapheme 前进整个 needle）。
std::vector<VoiceTargetedOccurrence> FindTargetedOccurrences(std::string_view target,
                                                             std::string_view remaining);

// UTF-8 文本的 UTF-16 码元长度（协调器换算选区偏移用；转发 utf8_grapheme.h）。
std::size_t VoiceTargetedUtf16Length(std::string_view text);

// targeted 执行计划。安全模型（与旧尾部删除不同）：
//   * 任何 verify_mode（strict/auto/off）下都必须 verified——读不到字段/光标一律
//     拒绝，严禁复用 8s unverified 盲删/盲改；
//   * L1 要求**完整** remainingText 仍是字段尾部后缀（允许末尾一个**已记录**
//     auto_enter 换行），不做旧「最长前缀」容错——用户手工变化会使目标偏移不可靠；
//   * 光标位置/既有选区不拦截（2026-10-31 放宽：定位靠完整后缀校验 + 计划时刻
//     现算偏移，与光标位置无关；双击核对目标词是常态动作）。
struct VoiceTargetedPlan {
    bool ok = false;
    VoiceTargetedFailure failure = VoiceTargetedFailure::kNone;
    VoiceTargetedAction action;
    std::size_t target_graphemes = 0;
    VoiceTargetedOccurrence occurrence;
    std::string new_remaining_text;  // 执行后的 remainingText（LF 归一化）

    // ---- 后缀结构拆解（执行器换算平台选区/终验用；相对字段尾部窗口 tail_text）----
    // remaining 之前的前缀（UTF-8；即 tail_text 去掉 remaining/换行后的头部）。
    std::string tail_prefix;
    // 尾部是否存在已记录 auto_enter 换行（tail_text 以 \n 结尾且 pressedEnter）。
    bool has_recorded_newline = false;
    // remaining_text 的 UTF-16 码元长度（执行器换算 tail 内偏移用）。
    std::size_t remaining_utf16_length = 0;
};

// L1 校验 + 执行计划（纯逻辑）。
//   field_tail: 字段尾部文本（UTF-8，UIA 尾部窗口，\r 段分隔已归一为 \n）；
//   field_readable: 字段可读性（不可读传 false → verifyUnavailable）；
//   caret_at_end/selection_collapsed: 保留参数（2026-10-31 起 planner 不再按其拒绝）。
VoiceTargetedPlan PlanVoiceTargeted(const VoiceTargetedCommand& command,
                                    std::string_view remaining_text, bool pressed_enter,
                                    bool field_readable, std::string_view field_tail,
                                    bool caret_at_end, bool selection_collapsed);

}  // namespace voicestick
