#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace voicestick {

// 热词评分与预算裁剪（高频优先策略的 C++ 侧实现，对应
// scripts/e2e_test/asr_bench/hotword_select.py 与
// Doc/Plan/hotword-eval-and-prioritization.md §3）。
//
// 背景：热词库持续增长，火山双向流式 corpus.context 直传预算有限
// （kHotwordCorpusTokenBudget = 80 tokens）。超预算时按
//   score = 1.0 * log1p(count) + 0.5 * exp(-age/30d) + 2.0 * manual
// 优先保留高频/新近/手动词，替代旧的按插入顺序贪心截断
// （新加词 append 在列表尾部，旧贪心从前往后保留，最先被裁的恰是最新加的词）。

// 评分权重（与 hotword_select.py 保持一致）。
inline constexpr double kHotwordWCount = 1.0;
inline constexpr double kHotwordWRecency = 0.5;
inline constexpr double kHotwordWManual = 2.0;
inline constexpr std::int64_t kHotwordRecencyTauS = 30LL * 24 * 3600;

// 精修/翻译 prompt 热词段上限（防大库稀释小模型注意力，见
// Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md）。
inline constexpr int kHotwordPromptMaxWords = 50;

struct HotwordUsage {
    int count = 0;                 // 出现在最终文本中的累计次数
    std::int64_t last_used_ts = 0; // 最近一次使用的 epoch 秒（0 = 未知，按最旧处理）
    std::string source = "mined";  // "manual"（用户手动加词）或 "mined"（候选挖掘）
};

using HotwordUsageStore = std::map<std::string, HotwordUsage>;

// 两平台共同的硬约束：不含空白，≤10 汉字 / ≤30 英文字符
// （与 hotword_select.py 的 is_valid_word 一致）。
bool IsValidHotword(const std::string& word);

// 评分；last_used_ts 未知（0）时新近度记 0。
double HotwordScore(const HotwordUsage& usage, std::int64_t now_s);

// 按评分降序返回 hotwords：缺统计记录的词按 manual 处理（现状所有热词均由用户
// 动作入表——划词/LLM 提炼/候选确认，无自动入表，避免新加的词最先被裁）；
// 非法词过滤；同分按字典序保证确定性。
std::vector<std::string> RankHotwords(const HotwordUsageStore& store,
                                      const std::vector<std::string>& hotwords,
                                      std::int64_t now_s);

// 取评分最高的前 max_words 个（用于精修/翻译 prompt 热词段）。
std::vector<std::string> TrimHotwordsForPrompt(const HotwordUsageStore& store,
                                               const std::vector<std::string>& hotwords,
                                               int max_words,
                                               std::int64_t now_s);

// 记录文本中出现的热词：计数 + 刷新最近使用时间戳（大小写不敏感子串匹配）。
// 只更新内存 store，落盘由调用方负责。
void RecordHotwordUsageInText(HotwordUsageStore& store,
                              const std::string& text,
                              const std::vector<std::string>& hotwords,
                              std::int64_t now_s);

// JSON 读写（数组形态 [{word,count,last_used_ts,source}, ...]，与
// hotword_select.py 的 load_stats_json 列表形态兼容）；文件缺失/损坏返回空
// store，写失败静默，best-effort。
HotwordUsageStore LoadHotwordUsage(const std::filesystem::path& path);
void SaveHotwordUsage(const std::filesystem::path& path, const HotwordUsageStore& store);

} // namespace voicestick
