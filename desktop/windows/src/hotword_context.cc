#include "hotword_context.h"

#include <algorithm>
#include <cctype>

namespace voicestick {

namespace {

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    auto to_lower = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(text[i + j]) != to_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

HotwordContextClassifier::HotwordContextClassifier(std::vector<HotwordContextRule> rules)
    : rules_(std::move(rules)) {}

HotwordContextScore HotwordContextClassifier::Score(const std::string& text) const {
    HotwordContextScore result;
    if (text.empty()) return result;
    for (const auto& rule : rules_) {
        if (rule.category.empty() || rule.keywords.empty()) continue;
        int hits = 0;
        for (const auto& keyword : rule.keywords) {
            if (ContainsCaseInsensitive(text, keyword)) ++hits;
        }
        if (hits > 0) {
            result.scores[rule.category] += rule.weight * hits;
            result.hits[rule.category] += hits;
        }
    }
    return result;
}

std::optional<std::string> HotwordContextClassifier::DecideCategory(
    const std::string& text,
    const std::string& current_category,
    const std::vector<std::string>& current_words,
    std::int64_t now_s) {
    if (text.empty()) {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    }
    // 当前分类热词命中保护：用户正在使用当前热词库（最终文本中出现当前分类的
    // 热词），是强停留信号，直接放弃本次决策。
    for (const auto& word : current_words) {
        if (!word.empty() && ContainsCaseInsensitive(text, word)) {
            pending_category_.clear();
            pending_count_ = 0;
            return std::nullopt;
        }
    }
    const auto scored = Score(text);
    if (scored.scores.empty()) {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    }
    // 最高分候选（排除当前分类）。
    const std::string* best = nullptr;
    double best_score = 0.0;
    for (const auto& [category, score] : scored.scores) {
        if (category == current_category) continue;
        if (!best || score > best_score) {
            best = &category;
            best_score = score;
        }
    }
    const auto reset_and_reject = [this]() {
        pending_category_.clear();
        pending_count_ = 0;
        return std::nullopt;
    };
    if (best == nullptr) return reset_and_reject();
    // ① 得分阈值；② 最少命中词数；③ 滞后：超过当前分类得分 + 加成。
    const double current_score =
        (scored.scores.count(current_category) > 0 ? scored.scores.at(current_category) : 0.0) +
        kHotwordCtxHysteresisBonus;
    const int best_hits = scored.hits.count(*best) > 0 ? scored.hits.at(*best) : 0;
    if (best_score < kHotwordCtxScoreThreshold ||
        best_hits < kHotwordCtxMinHits ||
        best_score <= current_score) {
        return reset_and_reject();
    }
    // ④ 连续性：连续 N 次决策指向同一分类。
    if (pending_category_ == *best) {
        ++pending_count_;
    } else {
        pending_category_ = *best;
        pending_count_ = 1;
    }
    // ⑤ 冷却：两次自动切换的最小间隔（冷却期内保留连续计数，到期即切）。
    if (last_switch_ts_ > 0 && now_s - last_switch_ts_ < kHotwordCtxCooldownS) {
        return std::nullopt;
    }
    if (pending_count_ < kHotwordCtxConsecutive) return std::nullopt;
    // ⑥ 目标非当前分类（候选选取时已排除），执行切换。
    last_switch_ts_ = now_s;
    pending_category_.clear();
    pending_count_ = 0;
    return *best;
}

void HotwordContextClassifier::Reset() {
    last_switch_ts_ = 0;
    pending_category_.clear();
    pending_count_ = 0;
}

std::vector<HotwordContextRule> HotwordContextClassifier::DefaultRules() {
    std::vector<HotwordContextRule> rules;
    // 程序：编码/工程/系统类关键词（含常见语言与工具名，ASCII 大小写不敏感）。
    rules.push_back({"程序",
                     {"代码", "编程", "程序", "函数", "变量", "编译", "调试", "运行",
                      "部署", "接口", "算法", "数据结构", "报错", "异常", "bug",
                      "缺陷", "重构", "提交", "分支", "合并", "回滚", "服务器",
                      "数据库", "前端", "后端", "客户端", "脚本", "框架", "类",
                      "对象", "继承", "多态", "线程", "进程", "内存", "指针",
                      "数组", "字符串", "语法", "断点", "单元测试", "集成测试",
                      "版本", "开发", "程序员", "编码", "python", "java",
                      "javascript", "typescript", "golang", "rust", "html",
                      "css", "sql", "linux", "git", "github", "api", "sdk",
                      "http", "tcp", "json", "yaml", "docker", "软件工程"},
                     1.0});
    // AI：模型/训练/智能类关键词。
    rules.push_back({"AI",
                     {"人工智能", "大模型", "大语言模型", "神经网络", "机器学习",
                      "深度学习", "模型", "训练", "推理", "微调", "提示词",
                      "智能体", "算力", "显卡", "张量", "数据集", "标注",
                      "过拟合", "梯度", "参数量", "token", "gpt", "多模态",
                      "语音识别", "图像识别", "自然语言", "知识图谱", "强化学习",
                      "生成式", "智能"},
                     1.0});
    // 数学：运算/几何/统计类关键词。
    rules.push_back({"数学",
                     {"数学", "方程", "微积分", "导数", "积分", "概率", "统计",
                      "几何", "代数", "定理", "证明", "公式", "计算", "矩阵",
                      "向量", "函数", "数列", "不等式", "三角形", "面积", "体积",
                      "坐标", "斜率", "极限", "收敛", "级数", "线性", "拟合",
                      "期望值", "方差", "分数", "小数", "整数", "质数", "开方",
                      "平方", "立方", "算术"},
                     1.0});
    // 语文：文学/语言学习类关键词。
    rules.push_back({"语文",
                     {"语文", "作文", "阅读理解", "古诗", "诗词", "文言文", "散文",
                      "小说", "段落", "文章", "修辞", "比喻", "拟人", "排比",
                      "拼音", "生字", "词语", "成语", "谚语", "默写", "背诵",
                      "课文", "作者", "中心思想", "主旨", "标点", "笔画", "偏旁",
                      "部首", "造句", "范文", "日记", "题材", "散文诗"},
                     1.0});
    return rules;
}

} // namespace voicestick
