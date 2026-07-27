#include "hotword_extractor.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace voicestick {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

} // namespace

void HotwordExtractor::Extract(std::string text,
                               std::string prompt_override,
                               std::function<void(bool, std::string)> completion) const {
    ChatAsync(BuildExtractPrompt(prompt_override), std::move(text), std::move(completion));
}

std::string HotwordExtractor::BuildExtractPrompt(const std::string& prompt_override) {
    const auto trimmed = Trim(prompt_override);
    if (!trimmed.empty()) return trimmed;
    return
        "你是一个热词提取器。\n"
        "从用户给出的文本中提取适合作为语音识别热词的词汇：专有名词、人名、地名、组织名、品牌名、产品名、专业术语等。\n"
        "\n"
        "• 每行输出一个词，只输出词本身。\n"
        "• 不要编号、解释、标点或 Markdown 格式。\n"
        "• 普通常用词不要提取。\n"
        "• 若没有值得提取的词，直接输出空。";
}

std::vector<std::string> HotwordExtractor::ParseExtractResult(const std::string& response) {
    auto words = ParseHotwordList(response);  // 已处理换行/逗号切分、Trim、去重
    std::vector<std::string> result;
    for (auto& w : words) {
        if (w.size() > kMaxWordLen) continue;
        result.push_back(std::move(w));
        if (result.size() >= kMaxWordCount) break;
    }
    return result;
}

std::vector<std::string> HotwordExtractor::DiffNewHotwords(
    const std::vector<std::string>& extracted,
    const std::vector<std::string>& existing) {
    std::vector<std::string> result;
    for (const auto& w : extracted) {
        if (std::find(existing.begin(), existing.end(), w) == existing.end() &&
            std::find(result.begin(), result.end(), w) == result.end()) {
            result.push_back(w);
        }
    }
    return result;
}

} // namespace voicestick
