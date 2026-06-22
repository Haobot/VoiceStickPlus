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

std::string LLMRefinementClient::BuildRefinePrompt(const std::string& prompt_override) {
    const auto trimmed = Trim(prompt_override);
    if (!trimmed.empty()) return trimmed;
    return
        "You are a speech-recognition post-processor.\n"
        "The input is raw text from automatic speech recognition. Rewrite it into clean written text:\n"
        "- Remove stray spaces inserted at speech pauses, especially spaces between CJK characters; keep legitimate spaces between words and around inline Latin text or numbers.\n"
        "- Fix punctuation: add missing punctuation, correct misplaced ones, remove redundant punctuation, following the conventions of the text's language.\n"
        "- Remove filler words, false starts, stutters, and meaningless colloquial fragments (e.g. \"嗯\", \"啊\", \"那个\", \"就是\", \"uh\", \"um\", \"you know\") when they carry no meaning.\n"
        "- Preserve the original meaning, language, and tone. Do not translate or expand the content.\n"
        "- Keep proper nouns, numbers, code, and technical terms intact.\n"
        "Return only the cleaned text, with no explanations, quotes, prefixes, alternatives, or markdown.";
}

} // namespace voicestick
