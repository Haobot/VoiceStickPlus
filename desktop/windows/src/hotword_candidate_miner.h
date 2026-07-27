#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace voicestick {

// 纠错对挖掘（热词候选）：精修把 ASR 错词纠回正确形式时，正确形式若是「标识符
// 样式」且不在热词表里，就是候选新热词。累计出现 kHotwordCandidateThreshold 次后
// 向用户建议，由用户确认后才入表。
//
// 为什么不做全自动入表：LLM 可能把错误变体（如 clouddmd）当热词加进去，之后
// ASR 第一遍反而被错误热词带偏，形成负反馈；且热词表会无限膨胀，稀释直传
// （双向流式 ~100 tokens）与精修 prompt 的效果。挖掘只产候选，入表必须经用户确认。
inline constexpr int kHotwordCandidateThreshold = 3;

// 提取文本中标识符样式的拉丁词：字母开头，字符集 [A-Za-z0-9_.\-]，长度 >= 3，
// 且至少含一个大写字母/数字/./_/-（纯小写普通英文单词不算，CJK 暂不挖掘）。
// 尾部的句号/下划线/连字符会被剥掉（如句末 "hello." 的 "."）。
std::vector<std::string> ExtractIdentifierTokens(const std::string& text);

// 挖掘候选：出现在 refined 但不在 original、不在 existing_hotwords 中的标识符，去重。
std::vector<std::string> MineRefinementCandidates(
    const std::string& original,
    const std::string& refined,
    const std::vector<std::string>& existing_hotwords);

struct HotwordCandidateStore {
    std::map<std::string, int> counts;  // 候选词 -> 累计出现次数
    std::set<std::string> dismissed;    // 用户在设置里忽略的词，永不建议
    std::set<std::string> notified;     // 已弹过托盘通知的词，避免重复打扰（设置里仍可见）
};

// JSON 读写（文件缺失/损坏返回空 store，写失败静默，best-effort）。
HotwordCandidateStore LoadHotwordCandidates(const std::filesystem::path& path);
void SaveHotwordCandidates(const std::filesystem::path& path, const HotwordCandidateStore& store);

// 记录一批候选词，返回本次新晋达到阈值、且未忽略/未通知的词（调用方据此弹通知，
// 通知后自行把词加入 store.notified 并保存）。
std::vector<std::string> RecordHotwordCandidates(HotwordCandidateStore& store,
                                                 const std::vector<std::string>& words);

// 当前待确认建议（达到阈值且未忽略），供设置界面展示。
std::vector<std::string> PendingHotwordSuggestions(const HotwordCandidateStore& store);

} // namespace voicestick
