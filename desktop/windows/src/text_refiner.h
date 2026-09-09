#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// 规则级文本精修（纯函数、保守策略：只做确定性变换，宁漏勿错）。
// 处理 ASR final 文本的显性噪声：句首语气词、CJK 叠字、中文字符间空格、
// 重复标点与句首孤立标点。上下文相关的口头语（那个/就是/然后）不在本层，
// 由本地 LLM 精修层负责（见 Doc/Plan/local-text-refinement.md）。
std::string RuleRefineText(std::string_view text);

// 精修结果守卫（纯函数）：判断 refined 相对 original 是否安全——
// 只允许「删除口水词片段 + 标点/空白规整 + ASCII 大小写纠正」三类变化；
// 任何新增/替换字符、删除实义片段均判不安全，调用方应回退上一层结果。
// hotwords 非空时叠加热词守卫（原文已正确出现的热词必须原样保留）。
bool RefineResultSafe(std::string_view original,
                      std::string_view refined,
                      const std::vector<std::string>& hotwords = {});

} // namespace voicestick
