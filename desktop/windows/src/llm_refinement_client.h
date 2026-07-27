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
                std::function<void(bool, std::string)> completion) const;

    // 流式精修：逐 token 回调 on_token（后台线程），完成时回调 on_complete。
    // cancel 为可选的取消令牌，设为 true 可中断流式精修。
    void RefineStream(std::string text,
                      std::string prompt_override,
                      std::function<void(std::string token)> on_token,
                      std::function<void(bool ok, std::string full_text)> on_complete,
                      std::shared_ptr<std::atomic_bool> cancel = nullptr) const;

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
};

} // namespace voicestick
