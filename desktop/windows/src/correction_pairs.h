#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicestick {

// 纠错对资产化（用户数据飞轮 P0-1，对齐 Doc/Plan/user-data-flywheel-wetype-benchmark.md §2
// 与 Doc/Plan/doubaoime-user-data-flywheel.md §3.1 改写对表）：每次精修产出
// original→refined 时提取最小替换片段对（如「鱼器渍→语气词」），落地为可累积的
// 本地规则资产；精修 prompt 注入 top-N few-shot，让同一错误第二次不犯。
//
// 存储先 JSON（schema 预留 SQLite 迁移：before/after/count/last_seen/source/app
// 即 WeType 方案 §2 的 corrections 列），多写者沿用 hotword_candidate_miner B13
// 模式：唯一写入口 + 进程级互斥 + reload-merge-save。
//
// 只保留「替换」形态对（两侧均非空）：整句改写（超长侧）、删除/插入、纯标点
// 空白调整一律不入表——few-shot 需要明确的「错形→正形」锚点，删除/插入对留给
// 后续火山 correct_table 阶段（typeless 方案 P0-3-2）再评估。

// 单侧最大码点数：超过即视为整句改写，不入表（防垃圾进垃圾出）。
inline constexpr int kCorrectionPairMaxSideChars = 24;
// prompt few-shot 段上限对数（对齐 wetype 方案 §2「如 8 对」）。
inline constexpr int kCorrectionPromptMaxPairs = 8;
// 存储容量硬上限（P1-4 完整清洗策略落地前的防膨胀闸）。
inline constexpr std::size_t kCorrectionPairStoreCap = 1000;

struct CorrectionPair {
    std::string before;             // ASR/精修前错误形式
    std::string after;              // 精修后用户认可形式
    int count = 0;                  // 累计出现次数
    std::int64_t last_seen_ts = 0;  // 最近出现 epoch 秒
    std::string source = "refine";  // 产物来源（当前仅精修 diff）
    std::string app;                // 前台应用分桶（P1-2 接线，当前恒空=全局）
};

// UTF-8 码点级最小编辑对齐：提取 refined 相对 original 被替换的连续片段对。
// 任一侧超过 512 码点（整句改写量级）返回空；同一对在一次提取内去重。
std::vector<std::pair<std::string, std::string>> ExtractReplacementPairs(
    std::string_view original, std::string_view refined);

// 内存合并：同 (before, after, app) 计数 +1 并刷新时间；新对追加；超容量按
// （count 升序、last_seen 升序、字典序）逐出。
void RecordCorrectionPairs(std::vector<CorrectionPair>& store,
                           const std::vector<std::pair<std::string, std::string>>& pairs,
                           std::int64_t now_ts);

// JSON 读写（文件缺失/损坏返回空 store，写失败静默 best-effort；保存前按
// (before, after, app) 字典序排序，输出确定性）。
std::vector<CorrectionPair> LoadCorrectionPairs(const std::filesystem::path& path);
void SaveCorrectionPairs(const std::filesystem::path& path,
                         const std::vector<CorrectionPair>& store);

// B13 单写入口：进程级互斥 + 每次从磁盘重读后合并落盘（绝不使用陈旧快照写回）。
void RecordCorrectionPairsToDisk(const std::filesystem::path& path,
                                 const std::vector<std::pair<std::string, std::string>>& pairs,
                                 std::int64_t now_ts);

// prompt few-shot 候选：按 count 降序、last_seen 降序、(before, after, app) 字典序
// 取前 max_pairs；count<=0 剔除。
std::vector<CorrectionPair> TrimCorrectionPairsForPrompt(const std::vector<CorrectionPair>& store,
                                                         int max_pairs);

// few-shot 段构造（两端逐字一致，双端单测钉死）。空集或全部被格式安全过滤
// （含引号/换行的对）时返回空串。
std::string BuildCorrectionFewshotSection(const std::vector<CorrectionPair>& pairs);

// 存储路径单一出处（与 config.toml 同目录）。
std::filesystem::path CorrectionPairsPath(const std::filesystem::path& config_path);

} // namespace voicestick
