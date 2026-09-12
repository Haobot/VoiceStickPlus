#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 热词纠错 AI 建议客户端：复用 LLMChatClient 网络层。用户在纠错对话框输入正确
// 文本时，异步请求与当前输入措辞相近的候选建议（同音/近音字、专有名词正确写法），
// 点击建议即可保存为纠错规则。
class HotwordSuggestClient : public LLMChatClient {
public:
    // 基类构造为 protected（见 LLMRefinementClient 同款注释），显式 public 转发。
    explicit HotwordSuggestClient(AppConfig config) : LLMChatClient(std::move(config)) {}

    // 异步建议：detached 线程完成后回调 completion(ok, 原始响应文本或错误信息)。
    // 调用方负责解析响应与回切 UI 线程。
    void Suggest(const std::string& wrong_text,
                 const std::string& input_text,
                 std::function<void(bool, std::string)> completion) const;

    // 可单测纯函数：override 非空（去空白后）返回 override，否则返回内置默认建议 prompt。
    static std::string BuildSuggestPrompt();

    // 可单测纯函数：解析 LLM 响应为建议列表。容错截取首个 JSON 数组，逐条过滤——
    // Trim、非空、长度 1..kMaxSuggestionLen、去重（忽略大小写），总量截断到
    // kMaxSuggestionCount。
    static std::vector<std::string> ParseSuggestResponse(const std::string& response);

    static constexpr std::size_t kMaxSuggestionLen = 64;    // 单条限长（UTF-8 字节）
    static constexpr std::size_t kMaxSuggestionCount = 5;   // 单次建议限量
};

} // namespace voicestick
