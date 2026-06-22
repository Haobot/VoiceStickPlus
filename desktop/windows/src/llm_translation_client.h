#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

// LLM 翻译客户端：复用 LLMChatClient 的网络层，仅提供翻译 system prompt。
// 翻译 prompt 已融合源文本精修要求（去停顿空格 / 修标点 / 去口头语）。
class LLMTranslationClient : public LLMChatClient {
public:
    // 基类构造函数为 protected（仅供子类复用网络层），继承构造会保持 protected 访问性，
    // 因此外部无法构造；这里显式提供 public 构造转发，使协调器能按值持有本类。
    explicit LLMTranslationClient(AppConfig config) : LLMChatClient(std::move(config)) {}

    void Translate(std::string text,
                   std::string target_language,
                   std::vector<std::string> hotwords,
                   std::function<void(bool, std::string)> completion) const;

    // 构造翻译 system prompt（已融合源文本精修要求：去停顿空格 / 修标点 / 去口头语）。
    static std::string SystemPrompt(const std::string& target_language,
                                    const std::vector<std::string>& hotwords);
};

} // namespace voicestick
