#pragma once

#include "app_config.h"

#include <Windows.h>
#include <Winhttp.h>

#include <functional>
#include <string>
#include <string_view>

namespace voicestick {

// OpenAI 兼容 /chat/completions 客户端基类：封装 WinHTTP 同步调用与 detached-thread 异步包装。
// 子类（翻译 / 精修）只需提供各自的 system prompt，复用全部网络与解析逻辑。
class LLMChatClient {
public:
    static std::wstring Utf16FromUtf8(std::string_view text);
    static std::string Utf8FromUtf16(std::wstring_view text);
    // 构造 OpenAI Chat Completions 请求体 JSON（temperature:0，system+user 两条消息）。
    static std::string BuildChatPayload(const std::string& model,
                                        const std::string& system_prompt,
                                        const std::string& user_text);

protected:
    explicit LLMChatClient(AppConfig config) : config_(std::move(config)) {}

    // 同步调用，返回 assistant 文本；失败时写 *error 并返回空。
    std::string ChatSync(const std::string& system_prompt,
                         const std::string& user_text,
                         std::string* error) const;
    // 异步包装：detached thread 调 ChatSync，completion(true,result) 或 (false,error)。
    void ChatAsync(std::string system_prompt,
                   std::string user_text,
                   std::function<void(bool, std::string)> completion) const;

    const AppConfig& config() const { return config_; }

private:
    std::string ChatCompletionsPathAndQuery(std::wstring* host, INTERNET_PORT* port, bool* secure,
                                            std::string* error) const;
    static std::string JsonEscape(std::string_view text);
    static std::string LastErrorText();

    AppConfig config_;
};

} // namespace voicestick
