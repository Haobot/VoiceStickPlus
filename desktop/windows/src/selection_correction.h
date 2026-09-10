#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// 划词纠错候选链路纯逻辑（S1，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md）：
// 用户选中 ASR 错词（如「逾期次」）→ LLM 生成候选 → 近音过滤 → 对话框展示
// → 用户点选/手输正确词（如「语气词」）→ 入热词表（S2 锚点域即生效）。

// 候选生成的 system 提示（角色定位；全部输出要求在 user 提示里）。
std::string BuildCandidatesSystemPrompt();

// 候选生成的 user 提示文本。错词 + 可选上下文（跨轮历史最近几轮拼接）
// + 可选热词表（触类旁通：引导从热词出候选，覆盖错词漏字的变体场景）。
// 云端（OpenAI 兼容）与本地精修引擎共用同一 prompt。
std::string BuildCorrectionCandidatesPrompt(
    std::string_view wrong_text, std::string_view context,
    const std::vector<std::string>& hotwords = {});

// 解析 LLM 输出为候选列表：按行拆分、剥序号前缀（1. / 1、/ - 等）、
// 跳空行与「无」类直答、跳含标点行（候选是词不该有标点）、保序不去重。
std::vector<std::string> ParseCandidateLines(std::string_view llm_output);

// 近音过滤：候选与错词经 NearVariantText（等长逐字近音，或 ±1 字近音
// 子序列对齐——错词是热词漏字变体时候选仍保留）才展示；去与错词相同的
// 项与重复项，保序。安全性不依赖 LLM——不过校验的候选一律不展示，
// 用户选择/手输是最终裁决。
std::vector<std::string> FilterCandidates(
    std::string_view wrong_text,
    const std::vector<std::string>& candidates);

} // namespace voicestick
