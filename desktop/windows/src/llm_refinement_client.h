#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace voicestick {

// ASR 文本精修客户端：复用 LLMChatClient 的网络层，仅提供精修 system prompt
// （去停顿空格、修标点、去口头语）。精修是 best-effort：调用方应在失败时回退原文。
class LLMRefinementClient : public LLMChatClient {
public:
    // 基类构造函数为 protected（仅供子类复用网络层），继承构造会保持 protected 访问性，
    // 因此外部无法构造；这里显式提供 public 构造转发，使协调器能按值持有本类。
    explicit LLMRefinementClient(AppConfig config) : LLMChatClient(std::move(config)) {}

    void Refine(std::string text,
                std::string prompt_override,
                std::function<void(bool, std::string)> completion,
                std::vector<std::string> hotwords = {}) const;

    // 流式精修：逐 token 回调 on_token（后台线程），完成时回调 on_complete。
    // cancel 为可选的取消令牌，设为 true 可中断流式精修。
    // hotwords 非空时覆盖 config().asr_hotwords 作为精修 prompt 热词段
    // （调用方按高频优先评分裁剪后的 top-N，防大库稀释小模型注意力）。
    void RefineStream(std::string text,
                      std::string prompt_override,
                      std::function<void(std::string token)> on_token,
                      std::function<void(bool ok, std::string full_text)> on_complete,
                      std::shared_ptr<std::atomic_bool> cancel = nullptr,
                      std::vector<std::string> hotwords = {}) const;

    // 可单测纯函数：override 非空（去空白后）返回 override，否则返回内置默认精修 prompt。
    // hotwords 非空时在末尾追加热词替换指引：火山二遍 ASR 不吃 corpus 热词直传
    // （流式第一遍生效、nostream 第二遍覆盖后丢失），由 LLM 精修按热词表兜底纠正。
    static std::string BuildRefinePrompt(const std::string& prompt_override,
                                         const std::vector<std::string>& hotwords = {});

    // 精修结果守卫（纯函数可单测）：凡是在 ASR 原文中已正确出现的热词，精修后必须
    // 仍然存在；有任何一个被改丢则返回 false，调用方应回退原文。用于兜底小模型
    // 精修的不稳定（偶发把已正确的热词改坏，如 AGENTS.md -> CLDE.md）。
    static bool RefineResultKeepsHotwords(const std::string& original,
                                          const std::string& refined,
                                          const std::vector<std::string>& hotwords);

    // 热词候选提炼（异步 best-effort）：从最终文本中提炼可能是专有名词且不在热词表
    // 中的候选词，completion(true, words)；失败回调 (false, {})。覆盖 diff 挖掘够不到
    // 的场景（ASR 本来就识别对、或 LLM 不认识的全新词无法被纠错引入）。
    // log 为可选观测回调（后台线程触发），输出 LLM 错误码与解析统计（只计数不记文本）。
    // 模型 temp 0 并非完全确定（同一输入实测偶发返回 []），模型成功但 0 候选且文本
    // 含大写字母（目标候选典型形态）时自动重试一次；网络错误与纯中文文本不重试。
    void ExtractHotwordCandidates(
        std::string text,
        std::vector<std::string> hotwords,
        std::function<void(bool, std::vector<std::string>)> completion,
        std::function<void(const std::string&)> log = nullptr) const;

    // 提炼 prompt（纯函数可单测）：要求只输出 JSON 数组、保留原始大小写、附已知热词表。
    static std::string BuildHotwordExtractionPrompt(const std::vector<std::string>& hotwords);

    // 提炼解析统计（只计数不记文本，用于生产环境定位 candidates=0 的原因）。
    struct HotwordExtractionStats {
        bool bracket_found = false;  // 响应中找到了 [...] 片段
        bool json_ok = false;        // 该片段解析为 JSON 数组
        int items = 0;               // 数组中字符串条目数
        int rejected_len = 0;        // 长度超出 2..40
        int rejected_words = 0;      // 超过 3 个词
        int rejected_not_in_text = 0;  // 未在原文出现（防臆造）
        int rejected_hotword = 0;    // 已在热词表
        int rejected_dup = 0;        // 重复候选
    };

    // 提炼结果解析（纯函数可单测）：容错截取首个 JSON 数组，逐条过滤——
    // 长度 2..40、至多 3 个词、必须在原文中实际出现（防臆造；ASR 英文空格形态
    // 不稳定，比较时容忍空白差异：折叠连续空白并对全去空白形式再比一次）、
    // 不与已有热词重复（比较均忽略大小写）。stats 非空时回填拒绝计数。
    static std::vector<std::string> ParseHotwordExtractionResponse(
        const std::string& response,
        const std::string& source_text,
        const std::vector<std::string>& hotwords,
        HotwordExtractionStats* stats = nullptr);

private:
    // 单次提炼尝试（ExtractHotwordCandidates 的内部实现，attempt 用于重试标记与日志）。
    void ExtractHotwordCandidatesAttempt(
        std::string text,
        std::vector<std::string> hotwords,
        int attempt,
        std::function<void(bool, std::vector<std::string>)> completion,
        std::function<void(const std::string&)> log) const;
};

} // namespace voicestick
