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
    // stream 为 true 时附加 "stream":true，用于 SSE 流式响应。
    static std::string BuildChatPayload(const std::string& model,
                                        const std::string& system_prompt,
                                        const std::string& user_text,
                                        bool stream = false);

    // SSE 流式回调：token 逐片到达；done 时 on_done(full_text)；出错时 on_error(msg)。
    struct StreamCallbacks {
        std::function<void(std::string token)> on_token;
        std::function<void(std::string full_text)> on_done;
        std::function<void(std::string error)> on_error;
    };

    // SSE 行解析（可单测）：从 "data: <json>" 行提取 delta.content token，返回空表示无 token。
    // 遇到 "[DONE]" 时通过 is_done 返回 true。
    static std::string ParseSseLine(const std::string& line, bool* is_done);

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
    // SSE 流式异步调用：detached thread 中通过 WinHTTP 增量读取 SSE 事件流，
    // 逐 token 回调 on_token，流结束回调 on_done，出错回调 on_error。
    // cancel 为可选的取消令牌，设为 true 可中断流式读取。
    void ChatStream(std::string system_prompt,
                    std::string user_text,
                    StreamCallbacks callbacks,
                    std::shared_ptr<std::atomic_bool> cancel = nullptr) const;

    const AppConfig& config() const { return config_; }

private:
    std::string ChatCompletionsPathAndQuery(std::wstring* host, INTERNET_PORT* port, bool* secure,
                                            std::string* error) const;
    static std::string JsonEscape(std::string_view text);
    static std::string LastErrorText();

    AppConfig config_;
};

} // namespace voicestick
