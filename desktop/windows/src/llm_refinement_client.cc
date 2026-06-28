#include "llm_refinement_client.h"

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

void LLMRefinementClient::Refine(std::string text,
                                 std::string prompt_override,
                                 std::function<void(bool, std::string)> completion) const {
    ChatAsync(BuildRefinePrompt(prompt_override), std::move(text), std::move(completion));
}

void LLMRefinementClient::RefineStream(std::string text,
                                       std::string prompt_override,
                                       std::function<void(std::string token)> on_token,
                                       std::function<void(bool ok, std::string full_text)> on_complete,
                                       std::shared_ptr<std::atomic_bool> cancel) const {
    const auto system_prompt = BuildRefinePrompt(prompt_override);
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

std::string LLMRefinementClient::BuildRefinePrompt(const std::string& prompt_override) {
    const auto trimmed = Trim(prompt_override);
    if (!trimmed.empty()) return trimmed;
    return
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

} // namespace voicestick
