#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// ===== 智能热词管理：上下文识别（关键词加权规则引擎） =====
//
// 对 ASR 最终文本做轻量语义分析：各内置领域（20 个，见 DefaultRules）持有关键词表，
// 命中即按权重得分。切换判定带防抖与保护（见 DecideCategory）：
//   - 得分阈值 + 最少命中词数：偶发单个技术词不触发（"程序内容不误识别"保障）；
//   - 当前分类热词命中保护：用户正在使用当前热词库时不切走；
//   - 滞后加成：当前分类得分加成，避免边界文本来回抖动；
//   - 连续 N 次决策一致才切换 + 切换冷却，防止频繁切换。
// 纯决策模块：不持有配置，不触碰文本纠错/注入路径——自动切换只改热词分类。
//
// 性能：纯子串匹配（分类数 × 关键词数 × 文本长度），500 字文本 <1ms，
// 满足切换响应 ≤300ms 指标。

// 软限：活跃分类词数超过该值即进入"监控"（活跃度统计呈现 + 为清理做准备）。
inline constexpr int kHotwordCtxSoftLimit = 5;
// 防误触发：目标分类最少命中关键词数。
inline constexpr int kHotwordCtxMinHits = 3;
// 切换得分阈值（加权命中和需 ≥ 该值）。
inline constexpr double kHotwordCtxScoreThreshold = 2.5;
// 连续 N 次决策指向同一分类才切换。
inline constexpr int kHotwordCtxConsecutive = 2;
// 切换冷却（秒）：两次自动切换的最小间隔。
inline constexpr std::int64_t kHotwordCtxCooldownS = 300;
// 当前分类滞后加成：候选分类得分需超过 当前分类得分+该值。
inline constexpr double kHotwordCtxHysteresisBonus = 1.0;

// 一条分类规则：category 下的关键词均按 weight 计分。
struct HotwordContextRule {
    std::string category;
    std::vector<std::string> keywords;
    double weight = 1.0;
};

// 文本评分结果：各分类的加权得分与命中关键词数（同词多次出现只计一次）。
struct HotwordContextScore {
    std::map<std::string, double> scores;
    std::map<std::string, int> hits;
};

// ===== 多级内容深度适配（方案 §4.3） =====
//
// 按文本长度与领域词命中密度分三档，决定精修 prompt 是否注入领域语境提示：
//VoiceStick 的"内容适配"限于 ASR/精修链路（术语保护、领域语境纠错），
// 不新增内容生成式助手能力，不触碰文本注入路径。
enum class ContentDepth {
    kLight,         // 短文本或零领域命中：默认精修 prompt，不注入
    kStandard,      // 中等长度且有领域命中：注入一行领域语境提示
    kProfessional,  // 长文本或高领域词密度：语境提示 + 术语保护强约束
};

// 深度档位阈值（UTF-8 字符数 / 命中关键词总数）。
inline constexpr int kDepthLightMaxChars = 30;
inline constexpr int kDepthProMinChars = 120;
inline constexpr int kDepthProMinHits = 5;

// 深度档位判定（纯函数可单测）：≤30 字符或零领域命中 → kLight；
// >120 字符或命中 ≥5 个领域关键词 → kProfessional；其余 kStandard。
ContentDepth ClassifyContentDepth(const std::string& text,
                                  const HotwordContextScore& score);

// 得分最高领域（同分按字典序稳序）；无命中返回空串。
std::string TopHotwordDomain(const HotwordContextScore& score);

// 领域语境提示（精修 prompt 注入段，纯函数可单测）：标准/专业档按领域返回
// 一句话语境指引，专业档追加术语保护强约束；未知领域（含用户自定义分类名）
// 返回空串。提示语初版由开发者起草，按方案 §8 决议需逐领域人工评审后定稿。
std::string BuildDomainHint(const std::string& domain, ContentDepth depth);

class HotwordContextClassifier {
public:
    explicit HotwordContextClassifier(std::vector<HotwordContextRule> rules);

    // 对文本评分：逐规则做大小写不敏感子串匹配，统计各分类得分与命中词数。
    HotwordContextScore Score(const std::string& text) const;

    // 切换判定状态机（有状态：连续计数 + 冷却时间戳）。返回目标分类名；无切换
    // 返回 nullopt。全部条件满足才切换（见文件头注释）；任一硬条件不满足时清零
    // 连续计数。调用方需自行确认目标分类在配置中存在（规则只覆盖内置分类名）。
    std::optional<std::string> DecideCategory(const std::string& text,
                                              const std::string& current_category,
                                              const std::vector<std::string>& current_words,
                                              std::int64_t now_s);

    // 内置 20 领域的关键词表（程序/AI/数学/语文 + 16 个扩展领域，方案 §4.1）；
    // 自定义分类不参与自动切换，内置领域名可经 [hotword_domain_map] 映射到
    // 用户分类（解析在协调器侧）。
    static std::vector<HotwordContextRule> DefaultRules();

    // 重置决策状态（配置变更/开关重新启用时由调用方调用）。
    void Reset();

private:
    std::vector<HotwordContextRule> rules_;
    std::int64_t last_switch_ts_ = 0;
    std::string pending_category_;
    int pending_count_ = 0;
};

} // namespace voicestick
