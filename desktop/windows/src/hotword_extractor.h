#pragma once

#include "app_config.h"
#include "llm_chat_client.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 热词提炼器：复用 LLMChatClient 网络层，从划词长文中提取适合作为语音识别热词的
// 词汇（专有名词/人名/术语等）。提示词构建、结果解析、去重 diff 均为可单测静态函数。
class HotwordExtractor : public LLMChatClient {
public:
    // 基类构造为 protected（见 LLMRefinementClient 同款注释），显式 public 转发。
    explicit HotwordExtractor(AppConfig config) : LLMChatClient(std::move(config)) {}

    // 异步提炼：detached 线程完成后回调 completion(ok, 原始响应文本或错误信息)。
    // 调用方负责解析响应与回切 UI 线程。
    void Extract(std::string text,
                 std::string prompt_override,
                 std::function<void(bool, std::string)> completion) const;

    // 可单测纯函数：override 非空（去空白后）返回 override，否则返回内置默认提炼 prompt。
    static std::string BuildExtractPrompt(const std::string& prompt_override);

    // 可单测纯函数：解析 LLM 响应为词列表。按换行/逗号切分、Trim、去重
    // （复用 ParseHotwordList），过滤超长词（>kMaxWordLen），总量截断到 kMaxWordCount。
    static std::vector<std::string> ParseExtractResult(const std::string& response);

    // 可单测纯函数：返回 extracted 中不在 existing 里的新词（保序、结果内去重）。
    static std::vector<std::string> DiffNewHotwords(const std::vector<std::string>& extracted,
                                                    const std::vector<std::string>& existing);

    static constexpr std::size_t kMaxWordLen = 64;    // 单词限长（字符）
    static constexpr std::size_t kMaxWordCount = 20;  // 单次提炼限量
};

} // namespace voicestick
