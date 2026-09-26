#pragma once

#include "app_config.h"
#include "hotword_selector.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// ===== 智能热词管理：活跃度评估 / 逐出 / 回收池 =====
//
// 活跃度直接复用 hotword_selector.h 的 HotwordScore（次数 × 30 天时间衰减 +
// manual 加成）：使用次数越多、越新近，活跃度越高；长期不用自然衰减为"冷淡"。
//
// 逐出（清理机制）：活跃分类达到 kMaxWords=25 上限时，优先逐出活跃度最低的
// 非 manual 词（用户手动加词最后逐出；全 manual 时降级逐出最低分 manual 词）。
// 逐出的词连同其使用统计进入回收池（hotword_evicted.json，独立于
// hotword_usage.json——后者是与 scripts/e2e_test/asr_bench/hotword_select.py
// 兼容的数组契约，不掺入回收池语义）。
//
// 回收（活跃度回升→重新加入）：回收池中的词再次出现在最终文本中，视为活跃度
// 回升信号，自动回归当前活跃分类（池内记录的原分类仅供展示，不作回归目标——
// 词可能被加进与语义无关的分类，按原分类回归会错位；活跃分类已满时先逐出
// 更冷的词再回归）。

// 冷淡判定线：活跃度低于该值的词在逐出时优先被清理。
inline constexpr double kHotwordActivityColdThreshold = 0.3;
// 时间衰减常数（与 hotword_selector.h 的 kHotwordRecencyTauS 一致：30 天）。
inline constexpr std::int64_t kHotwordActivityTauS = 30LL * 24 * 3600;

// 回收池条目：word + 被逐出时所在分类（仅展示记录，回收回归目标为当前活跃
// 分类）+ 逐出时间 + 逐出时的使用统计快照。
struct EvictedWord {
    std::string word;
    std::string category;
    std::int64_t evicted_ts = 0;
    HotwordUsage usage;
};

using EvictedPool = std::vector<EvictedWord>;

// 冷淡判定：活跃度（HotwordScore）低于 kHotwordActivityColdThreshold。
bool IsCold(const HotwordUsage& usage, std::int64_t now_s);

// 逐出候选：words 中活跃度最低的词。protect_manual=true 时只在非 manual 词中
// 选（全 manual 时降级选最低分 manual 词）；返回 nullopt 表示 words 为空。
// 评分缺失的词按 HotwordScore 默认（count=0）处理。同分按字典序保证确定性。
std::optional<std::string> PickEvictionWord(const std::vector<std::string>& words,
                                            const HotwordUsageStore& store,
                                            bool protect_manual,
                                            std::int64_t now_s);

// 回收池 JSON 读写（数组形态 [{word,category,evicted_ts,count,last_used_ts,source}]，
// best-effort：文件缺失/损坏返回空池，写失败静默）。
EvictedPool LoadEvictedPool(const std::filesystem::path& path);
void SaveEvictedPool(const std::filesystem::path& path, const EvictedPool& pool);

// 回收判定：池中出现在 text 里的词（大小写不敏感子串匹配），视为活跃度回升。
// 返回第一个命中条目的拷贝（调用方需 category 判断回归目标分类）；无命中 nullopt。
std::optional<EvictedWord> PickReclaimWord(const EvictedPool& pool, const std::string& text);

// 活跃度快照（可视化对话框展示用）：配置（分类/活跃分类）+ 使用统计 + 回收池。
// 协调器在锁内取拷贝，调用方可安全离线使用。
struct HotwordActivitySnapshot {
    AppConfig config;
    HotwordUsageStore usage;
    EvictedPool evicted;
};

} // namespace voicestick
