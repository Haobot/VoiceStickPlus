#pragma once

// 语音编辑指令纯逻辑（Doc/Plan/voice-edit-commands.md）：
//   1) 中文整句白名单解析（ParseVoiceEditCommand）；
//   2) UTF-8 grapheme 计数/截断辅助（GraphemeCount/DropLastGraphemes）；
//   3) L1 目标校验 + 退格删除计划（PlanVoiceEdit）。
//
// 本文件与 voice_edit_command.cc 不依赖任何 Win32/平台头，可在 macOS 上用
// clang++ -std=c++20 编译并链接本地 main 跑通全部测试向量。
// 双端同向量：解析器白名单（verbs / bareDeleteForms / deleteAllForms / 礼貌前缀 /
// 语气尾 / 单位表 / 句末分隔符）与 macOS
// desktop/macos/Sources/VoiceStickCore/VoiceEditCommand.swift 逐字一致——任一侧
// 增删词条必须同步另一侧与两端的测试表（延续 correction_pairs 约定）。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

enum class VoiceEditAction {
    kDeleteLastChars,
    kDeleteLastSentences,
    kDeleteAll,
};

enum class VoiceEditCommandSource {
    kExplicitCount,  // “删掉最后两个字”：显式数量
    kDefaultCount,   // “删除”：使用 default_chars（或句式单位默认 1）
    kAll,            // “撤销/清空/删除刚才的”：整段
};

struct VoiceEditCommand {
    VoiceEditAction action = VoiceEditAction::kDeleteLastChars;
    // kDeleteLastChars/kDeleteLastSentences 的 N（1..max_chars）；kDeleteAll 恒为 0。
    int count = 1;
    VoiceEditCommandSource source = VoiceEditCommandSource::kDefaultCount;

    bool operator==(const VoiceEditCommand& other) const = default;
};

// 解析中文语音编辑指令。归一化（去首尾/内部空白、全角数字转半角、去尾部标点与
// 语气词、去礼貌前缀）后必须**整句**匹配白名单语法；未命中返回 nullopt（按普通
// 文本上屏）。
//   default_chars: 单独说“删除”时删几个字（钳位 1..max_chars）
//   max_chars:     单条指令允许删除的最大字数（钳位 >=1；配置层再限 1..50）；
//                  显式数量超限 → 非指令
std::optional<VoiceEditCommand> ParseVoiceEditCommand(std::string_view text,
                                                     int default_chars = 1,
                                                     int max_chars = 20);

enum class VoiceEditVerifyMode {
    kStrict,  // 读不到字段/光标一律拒绝
    kAuto,    // 可读→必须校验通过；不可读→仅 unverified 时间窗内执行
    kOff,     // 不校验（仅 L0，排障用）
};

VoiceEditVerifyMode VoiceEditVerifyModeFromName(std::string_view name,
                                                VoiceEditVerifyMode fallback);
std::string_view VoiceEditVerifyModeName(VoiceEditVerifyMode mode);

// ---- UTF-8 grapheme（用户可见字符）辅助 ----
// 覆盖 CJK / emoji / 组合字符 / ZWJ 连字 / 国旗 / 变体选择符与 emoji 肤色修饰符。
// 说明：按 Unicode 扩展 grapheme cluster 的常见子集实现，不含完整 UAX #29
//（复杂 Indic 组合可能多计），足以支撑“删 N 个字”的计数与截断（Doc §7.2）。
bool IsValidUtf8(std::string_view text);
std::size_t GraphemeCount(std::string_view text);
// 删除末尾 count 个 grapheme（count >= 计数时返回空串）。
std::string DropLastGraphemes(std::string_view text, std::size_t count);
// 行尾归一化：CRLF / CR -> LF（UIA TextPattern 以 \r 作段分隔，注入文本可能含 \n）。
std::string NormalizeVoiceEditLineBreaks(std::string_view text);
// 句子切分（句末标点 。！？； + ASCII !?; 与换行，标点归前句；末尾残句算一句）。
// 返回每句的 grapheme 数，长度和恒等于 GraphemeCount(归一化后的 text)。
std::vector<std::size_t> VoiceEditSentenceGraphemes(std::string_view text);

// ---- L1 目标校验与删除计划 ----

// 焦点字段读取结果（UIA 层填充；readable=false 表示不可验证）。
struct VoiceEditFieldState {
    bool readable = false;
    bool is_password = false;
    // GetSelection 恰好返回 1 个选区且首尾折叠（CompareEndpoints==0）才为 true。
    bool selection_collapsed = true;
    // 该折叠选区末端 == 文档 range 末端（CompareEndpoints，非字符偏移）。
    bool caret_at_end = false;
    // 字段尾部文本（UTF-8，约最后 512 个 UTF-16 码元）；仅用于尾部后缀匹配。
    std::string tail_text;
    // tail_text 的 UTF-16 码元长度（targeted 编辑换算 tail 内选区偏移用）。
    std::size_t tail_utf16_length = 0;
    // 尾部读取被截断（字段内容比 tail_text 长），仅供日志/排障。
    bool truncated = false;
    // ---- 批次 1（字段内定位）：光标在 tail_text 内的 UTF-16 相对偏移 ----
    // 旧布尔（caret_at_end/selection_collapsed）保留（派生诊断）；P1a/P1b 锚定
    // 与句中句号判定用本偏移。窗口外/不可得 = kCaretOutsideTail。
    std::size_t caret_tail_utf16 = static_cast<std::size_t>(-1);
};

// caret_tail_utf16 的「不可得」哨兵（窗口外/平台未实现偏移读取）。
inline constexpr std::size_t kCaretOutsideTail = static_cast<std::size_t>(-1);

enum class VoiceEditRejectReason {
    kNone,
    kEmptyTarget,           // 没有可删除的注入文本
    kFieldUnreadable,       // strict：字段/光标不可读（或未注入 reader）
    kPasswordField,         // 密码框：永不执行
    kSelectionActive,       // 选区非空（要求用户先取消选中）
    kCaretNotAtEnd,         // 光标不在字段末尾（批次 1 起 planner 不再返回；枚举位
                            // 与失败名表保留，双端对齐）
    kTextMismatch,          // 字段尾部与注入记录对不上（已发送/被改动/继续手打）
    kUnverifiedNotAllowed,  // auto：不可验证且超出 unverified 时间窗
    kNothingToDelete,       // 计算后无可删除内容
    // ---- 批次 1（字段内定位）尾部追加（双端同构，别插中间）----
    kAnchorAmbiguous,       // anchored 段在字段中出现 ≥2 次且 P1 全 miss（零容错拒绝）
    kCaretMoveFailed,       // 尾删前置光标移动失败：零退格、记录保留（协调器产生）
};

std::string_view VoiceEditRejectReasonName(VoiceEditRejectReason reason);

struct VoiceEditPlan {
    bool ok = false;
    VoiceEditRejectReason reject_reason = VoiceEditRejectReason::kNone;
    VoiceEditAction action = VoiceEditAction::kDeleteLastChars;
    // 本次删除的文本内容 grapheme 数（不含 auto_enter 换行）。
    int delete_graphemes = 0;
    // sentence 动作实际删除的句数（被剩余句数 clamp；其它动作为 0）。
    int deleted_sentences = 0;
    // 是否额外删除 1 个尾部换行（auto_enter）。
    bool delete_newline = false;
    // 实际注入的退格次数 = delete_graphemes + (delete_newline ? 1 : 0)。
    int backspace_count = 0;
    // 执行后仍留在输入框中的剩余注入文本（LF 归一化）。
    std::string new_remaining_text;
    // 执行后记录中的 pressedEnter（换行已消费或已确认不存在则 false）。
    bool new_pressed_enter = false;
    // true = 经过 L1 校验；false = auto 不可验证分支 / off。
    bool verified = false;
    // ---- 批次 1（字段内定位）：anchored 段终点（tail 窗口内 UTF-16 偏移）与
    // 光标是否需先移过去。unverified 计划读不到字段无从定位 → kCaretOutsideTail
    // 且 needs_caret_move=false（盲退格窗口语义不变）。----
    std::size_t anchor_end_utf16 = kCaretOutsideTail;
    bool needs_caret_move = false;
};

// L1 校验 + 删除计划（纯逻辑，算法见 Doc/Plan/voice-edit-commands.md §6.3 与
// §17 批次 1 实施记录）：
//  - 可验证（readable && !password）：选区非空先拒绝；字段内多级锚定
//    （voice_edit_anchor.h：P1a 光标前缀/P1b 光标后缀/P1c 字段末尾/P2 唯一出现，
//    零容错——旧 caret_at_end 门槛与最长前缀容错已移除），产出
//    anchor_end_utf16 + needs_caret_move（协调器先移光标再退格）；
//  - strict：不可验证一律拒绝；
//  - auto：不可验证时仅当 unverified_allowed（协调器按 unverified_seconds 判定）
//    才按不可验证分支执行；
//  - off：不校验，按不可验证分支执行（密码框仍拒绝）。
//  - caret_tail_utf16 == kCaretOutsideTail 时 P1a/P1b 不可判，仅 P1c/P2 可命中。
VoiceEditPlan PlanVoiceEdit(std::string_view remaining_text, bool pressed_enter,
                            const VoiceEditCommand& command,
                            const VoiceEditFieldState& field,
                            VoiceEditVerifyMode verify_mode,
                            bool unverified_allowed);

} // namespace voicestick
