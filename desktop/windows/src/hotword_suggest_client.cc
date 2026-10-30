#include "hotword_suggest_client.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <set>
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

void HotwordSuggestClient::Suggest(const std::string& wrong_text,
                                   const std::string& input_text,
                                   std::function<void(bool, std::string)> completion) const {
    ChatAsync(BuildSuggestPrompt(),
              "错误文本：" + wrong_text + "\n用户当前输入：" + input_text,
              std::move(completion));
}

std::string HotwordSuggestClient::BuildSuggestPrompt() {
    return
        "你是语音识别纠错助手。语音识别把用户的正确文本错误识别成了「错误文本」。\n"
        "用户正在修正，当前已输入「用户当前输入」。\n"
        "请给出最多 5 个与用户当前输入措辞相近的候选正确文本：优先考虑该输入的完整\n"
        "正确形态、同音/近音字的正确写法、常见专有名词（人名/品牌/产品/术语）的标准写法。\n"
        "\n"
        "• 只输出 JSON 字符串数组，如 [\"候选1\", \"候选2\"]。\n"
        "• 不要编号、解释或 Markdown 格式。\n"
        "• 每个候选不超过 20 个字。\n"
        "• 没有合适的候选时输出 []。";
}

std::vector<std::string> HotwordSuggestClient::ParseSuggestResponse(const std::string& response) {
    // 容错：LLM 可能在 JSON 数组前后包裹解释文字，截取首个 [...] 片段解析。
    const auto start = response.find('[');
    const auto end = response.rfind(']');
    if (start == std::string::npos || end == std::string::npos || end <= start) return {};
    auto* root = cJSON_Parse(response.substr(start, end - start + 1).c_str());
    if (!root) return {};
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    if (!cJSON_IsArray(root)) return {};

    std::vector<std::string> suggestions;
    std::set<std::string> seen_lower;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsString(item) || item->valuestring == nullptr) continue;
        const std::string word = Trim(item->valuestring);
        if (word.empty() || word.size() > kMaxSuggestionLen) continue;
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!seen_lower.insert(lower).second) continue;
        suggestions.push_back(word);
        if (suggestions.size() >= kMaxSuggestionCount) break;
    }
    return suggestions;
}

} // namespace voicestick
