#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// 拼音同音/近音判定（跨轮纠错守卫的字符级口径）。
//
// 语义以 M0 spike 实测验证口径为准（m0/refine/run_cross_turn_spike.py
// same_or_near，报告 report_cross_turn_4b.md GO 数据）：
//   1. 码点完全相同 → 近音；
//   2. 任一方非 CJK 基本区汉字 → 按字节忽略 ASCII 大小写比较；
//   3. 双方汉字且韵母集合有交集 → 同音（声母不参与——spike 实测口径，
//      「渍 zì/词 cí」声母不同仍须放行，靠本条通过）；
//   4. 韵母命中模糊音对表（an/ang 等鼻音混淆、e/i 卷舌弱化）且声母集合
//      有交集 → 近音。
// 拼音数据查不到的字按不同音处理（保守拒绝）。声母模糊对表（n/l 等）在
// 该口径下为死分支（韵母有交集时第 3 条已命中），不移植。
bool PinyinSameOrNear(std::uint32_t a, std::uint32_t b);

// 等长（码点数）文本逐字近音判定：任一位置非近音即 false；码点数不等 false；
// 两个空串 true。守卫与候选过滤的基线口径。
bool SameOrNearText(std::string_view a, std::string_view b);

// SameOrNearText 的变体容忍版（触类旁通）：等长逐字近音，或一方恰好多 1 字
// 且短方与长方删任一字后的连续序列逐字近音（热词少字变体：「口水」「水池」
// 对齐「口水词」）。对称；长度差 ≥2 仍 false。划词候选过滤用——错词是
// 热词漏字时候选仍可展示，用户点选是最终裁决。
bool NearVariantText(std::string_view a, std::string_view b);

struct CorrectionOutcome {
    std::string text;                   // 执行后文本（被拒指令不执行，其余生效）
    std::vector<std::string> rejected;  // 被守卫拒绝的指令行（诊断归因）
};

// 受限纠正指令执行器（M0 spike apply_corrections 的 C++ 移植，生产规格基线）。
//
// instructions 是本地 LLM 输出的多行指令，每行二选一：
//   「错词→纠正词」：替换。守卫全在代码层——src 须在当前文本、dst 须在
//     context（跨轮上文）或 hotwords（热词表整词）中出现过、逐字
//     PinyinSameOrNear（等长），或 dst 恰好多 1 字且近音子序列对齐
//     （热词少字变体：「口水」「水池」→「口水词」；dst 更短或长度差 ≥2
//     一律拒——只放宽「错词漏字」方向）。热词锚点（S2）：ASR 连续误识别
//     某关键词时上文永远无正确写法，用户划词确认的正确词入热词表即建立
//     锚点，后续变体自愈；
//   待删片段原样摘录：不得含字母数字、须在当前文本中。
// 「无」/空指令直通原文。执行后总长低于原文 40% 时整体回退原文
// （rejected 追加 "total-ratio"）。安全性不依赖模型：任何越界指令均被拒。
CorrectionOutcome ApplyPinyinCorrections(
    std::string_view asr, std::string_view instructions, std::string_view context,
    const std::vector<std::string>& hotwords = {});

} // namespace voicestick
