#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <functional>
#include <string>

namespace voicestick {

// ASR 文本精修客户端：复用 LLMChatClient 的网络层，仅提供精修 system prompt
// （去停顿空格、修标点、去口头语）。精修是 best-effort：调用方应在失败时回退原文。
class LLMRefinementClient : public LLMChatClient {
public:
    using LLMChatClient::LLMChatClient;

    void Refine(std::string text,
                std::string prompt_override,
                std::function<void(bool, std::string)> completion) const;

    // 可单测纯函数：override 非空（去空白后）返回 override，否则返回内置默认精修 prompt。
    static std::string BuildRefinePrompt(const std::string& prompt_override);
};

} // namespace voicestick
