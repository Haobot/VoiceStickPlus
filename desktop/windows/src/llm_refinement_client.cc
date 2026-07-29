#include "llm_refinement_client.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <memory>
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

// ASCII 忽略大小写比较（热词/候选均为 ASCII 为主，足够用）。
bool EqualsCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty() || needle.size() > text.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        if (EqualsCaseInsensitive(text.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

std::string StripSpaces(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch != ' ') out.push_back(ch);
    }
    return out;
}

// 防臆造匹配：候选必须在原文中实际出现。ASR 英文空格形态不稳定
// （"Stack Chain" / "Stack  Chain" / "StackChain" 都可能是同一口述），
// 除精确子串外，再对全去空白形式比较一次；均忽略大小写。
bool AppearsInSourceText(const std::string& source_text, const std::string& word) {
    if (ContainsCaseInsensitive(source_text, word)) return true;
    if (word.find(' ') == std::string::npos) return false;
    return ContainsCaseInsensitive(StripSpaces(source_text), StripSpaces(word));
}

} // namespace

void LLMRefinementClient::Refine(std::string text,
                                 std::string prompt_override,
                                 std::function<void(bool, std::string)> completion) const {
    ChatAsync(BuildRefinePrompt(prompt_override, config().asr_hotwords), std::move(text),
              std::move(completion));
}

void LLMRefinementClient::RefineStream(std::string text,
                                       std::string prompt_override,
                                       std::function<void(std::string token)> on_token,
                                       std::function<void(bool ok, std::string full_text)> on_complete,
                                       std::shared_ptr<std::atomic_bool> cancel) const {
    const auto system_prompt = BuildRefinePrompt(prompt_override, config().asr_hotwords);
    StreamCallbacks cbs;
    cbs.on_token = std::move(on_token);
    cbs.on_done = [on_complete](std::string full_text) {
        if (on_complete) on_complete(true, std::move(full_text));
    };
    cbs.on_error = [this, text, prompt = std::move(prompt_override),
                    on_complete](std::string error) mutable {
        // SSE 流式失败（服务端不支持 stream:true 或网络超时等）：
        // 回退到非流式 ChatAsync → ChatSync 精修，不丢精修能力。
        (void)error;
        Refine(std::move(text), std::move(prompt), std::move(on_complete));
    };
    ChatStream(system_prompt, text, std::move(cbs), std::move(cancel));
}

std::string LLMRefinementClient::BuildRefinePrompt(const std::string& prompt_override,
                                                   const std::vector<std::string>& hotwords) {
    const auto trimmed = Trim(prompt_override);
    std::string prompt;
    if (!trimmed.empty()) {
        prompt = trimmed;
    } else {
        prompt =
            "你是一个语音识别后处理器。\n"
            "输入为自动语音识别生成的原始文本。请将其改写为规范的书面文本：\n"
            "\n"
            "• 去除因语音停顿产生的多余空格，尤其中文、日文、韩文字符间的空格；保留单词间及行内拉丁字母、数字周围的合法空格。\n"
            "\n"
            "• 修正标点：按文本语言规范补全缺失标点、调整错位标点、删除冗余标点。若输入仅为短语或短词，末尾请勿添加句号。\n"
            "\n"
            "• 剔除无实际语义的填充词、半截话、口吃及无意义口语碎片（如“嗯”“啊”“那个”“就是”“uh”“um”“you know”）。\n"
            "\n"
            "• 保留原意、语种与语气，不翻译或扩写内容。\n"
            "\n"
            "• 专有名词、数字、代码及专业术语保持不变。\n"
            "\n"
            "仅返回清理后的文本，无需解释、引号、前缀、备选方案或 Markdown 格式。";
    }
    if (hotwords.empty()) return prompt;

    prompt += "\n\n用户常用术语热词表：";
    for (std::size_t i = 0; i < hotwords.size(); ++i) {
        if (i != 0) prompt += ", ";
        prompt += hotwords[i];
    }
    prompt += "。若识别文本中出现与热词发音相近的写法（如拼读、同音、分写或连写变形），"
              "请替换为热词原形；没有相近发音的词时不要凭空插入热词。"
              "示例：热词表含「AGENTS.md」时，识别文本「编辑 agentsdmd 这个文件」"
              "应改为「编辑 AGENTS.md 这个文件」。"
              "原文中已经正确出现的热词必须原样保留，不得改写。";
    return prompt;
}

bool LLMRefinementClient::RefineResultKeepsHotwords(const std::string& original,
                                                    const std::string& refined,
                                                    const std::vector<std::string>& hotwords) {
    for (const auto& hotword : hotwords) {
        if (hotword.empty()) continue;
        if (original.find(hotword) == std::string::npos) continue;
        if (refined.find(hotword) == std::string::npos) return false;
    }
    return true;
}

void LLMRefinementClient::ExtractHotwordCandidates(
    std::string text,
    std::vector<std::string> hotwords,
    std::function<void(bool, std::vector<std::string>)> completion,
    std::function<void(const std::string&)> log) const {
    ExtractHotwordCandidatesAttempt(std::move(text), std::move(hotwords), 1,
                                    std::move(completion), std::move(log));
}

void LLMRefinementClient::ExtractHotwordCandidatesAttempt(
    std::string text,
    std::vector<std::string> hotwords,
    int attempt,
    std::function<void(bool, std::vector<std::string>)> completion,
    std::function<void(const std::string&)> log) const {
    ChatAsync(BuildHotwordExtractionPrompt(hotwords), text,
              [this, text = std::move(text), hotwords = std::move(hotwords), attempt,
               completion = std::move(completion),
               log = std::move(log)](bool ok, std::string response) mutable {
                  if (!ok) {
                      // response 此时是 WinHTTP 错误码文本，不含用户内容。
                      if (log) log("hotword extraction llm_error: " + response);
                      completion(false, {});
                      return;
                  }
                  HotwordExtractionStats stats;
                  auto words = ParseHotwordExtractionResponse(response, text, hotwords, &stats);
                  if (log) {
                      // 响应只含模型给出的候选词（与托盘通知/「suggested」日志同级，
                      // 不含用户原文）；压掉换行、截断到 160 字符保证单行。
                      std::string raw = response;
                      std::replace(raw.begin(), raw.end(), '\n', ' ');
                      std::replace(raw.begin(), raw.end(), '\r', ' ');
                      if (raw.size() > 160) raw = raw.substr(0, 160) + "...";
                      log("hotword extraction detail: attempt=" + std::to_string(attempt) +
                          " resp_len=" + std::to_string(response.size()) +
                          " bracket=" + std::to_string(stats.bracket_found) +
                          " json=" + std::to_string(stats.json_ok) +
                          " items=" + std::to_string(stats.items) +
                          " kept=" + std::to_string(words.size()) +
                          " rej_len=" + std::to_string(stats.rejected_len) +
                          " rej_words=" + std::to_string(stats.rejected_words) +
                          " rej_not_in_text=" + std::to_string(stats.rejected_not_in_text) +
                          " rej_hotword=" + std::to_string(stats.rejected_hotword) +
                          " rej_dup=" + std::to_string(stats.rejected_dup) +
                          " raw=" + raw);
                  }
                  // 模型 temp 0 非完全确定：同一输入偶发返回 []（实测同一句离线 6/6 中、
                  // 线上 2/2 空）。成功但 0 候选且文本含大写字母（目标候选的典型形态）时
                  // 重试一次；纯中文等无候选文本不重试，避免每次会话白调一次 LLM。
                  const bool has_capital = std::any_of(text.begin(), text.end(), [](char ch) {
                      return ch >= 'A' && ch <= 'Z';
                  });
                  if (words.empty() && attempt == 1 && has_capital) {
                      if (log) log("hotword extraction retry: attempt 1 returned no candidates");
                      ExtractHotwordCandidatesAttempt(std::move(text), std::move(hotwords), 2,
                                                      std::move(completion), std::move(log));
                      return;
                  }
                  completion(true, std::move(words));
              });
}

std::string LLMRefinementClient::BuildHotwordExtractionPrompt(
    const std::vector<std::string>& hotwords) {
    std::string prompt =
        "你是热词候选提取器。从语音识别结果文本中提取可能是专有名词的词或短语："
        "产品名、项目名、公司名、技术术语、文件名、代号等。\n"
        "规则：\n"
        "• 只提取文本中实际出现的内容，严格保留原始拼写与大小写，不臆造、不翻译、不纠错。\n"
        "• 普通词汇、常见英文单词、人称代词与完整句子不要提取。\n"
        "• 文本中非句首的首字母大写词或词组，即使看起来像普通英文单词的组合，也应作为候选输出。\n"
        "• 已知热词表中的条目不要重复提取。\n"
        "• 最多输出 5 个。\n"
        "• 只输出 JSON 数组（如 [\"DeepSeek\", \"Stack Chain\"]），没有候选时输出 []；"
        "不要输出解释或其他任何内容。\n"
        "示例：输入「我刚才讲了 Stack Chain 这个新词」→ 输出 [\"Stack Chain\"]；"
        "输入「今天天气不错，我们出去走走吧」→ 输出 []。";
    if (hotwords.empty()) return prompt;
    prompt += "\n已知热词表：";
    for (std::size_t i = 0; i < hotwords.size(); ++i) {
        if (i != 0) prompt += ", ";
        prompt += hotwords[i];
    }
    prompt += "。";
    return prompt;
}

std::vector<std::string> LLMRefinementClient::ParseHotwordExtractionResponse(
    const std::string& response,
    const std::string& source_text,
    const std::vector<std::string>& hotwords,
    HotwordExtractionStats* stats) {
    // 容错：LLM 可能在 JSON 数组前后包裹解释文字，截取首个 [...] 片段解析。
    const auto start = response.find('[');
    const auto end = response.rfind(']');
    if (start == std::string::npos || end == std::string::npos || end <= start) return {};
    if (stats) stats->bracket_found = true;
    auto* root = cJSON_Parse(response.substr(start, end - start + 1).c_str());
    if (!root) return {};
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    if (!cJSON_IsArray(root)) return {};
    if (stats) stats->json_ok = true;

    std::vector<std::string> candidates;
    std::set<std::string> seen_lower;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsString(item) || item->valuestring == nullptr) continue;
        if (stats) ++stats->items;
        const std::string word = Trim(item->valuestring);
        if (word.size() < 2 || word.size() > 40) {
            if (stats) ++stats->rejected_len;
            continue;
        }
        // 至多 3 个词，避免整句被当成候选。
        int word_count = 1;
        for (char ch : word) {
            if (ch == ' ') ++word_count;
        }
        if (word_count > 3) {
            if (stats) ++stats->rejected_words;
            continue;
        }
        // 防臆造：候选必须在原文中实际出现（忽略大小写与空白差异）。
        if (!AppearsInSourceText(source_text, word)) {
            if (stats) ++stats->rejected_not_in_text;
            continue;
        }
        bool is_hotword = false;
        for (const auto& hotword : hotwords) {
            if (EqualsCaseInsensitive(hotword, word)) {
                is_hotword = true;
                break;
            }
        }
        if (is_hotword) {
            if (stats) ++stats->rejected_hotword;
            continue;
        }
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!seen_lower.insert(lower).second) {
            if (stats) ++stats->rejected_dup;
            continue;
        }
        candidates.push_back(word);
    }
    return candidates;
}

} // namespace voicestick
