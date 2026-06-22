#include "llm_translation_client.h"

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

void LLMTranslationClient::Translate(std::string text,
                                     std::string target_language,
                                     std::vector<std::string> hotwords,
                                     std::function<void(bool, std::string)> completion) const {
    ChatAsync(SystemPrompt(target_language, hotwords), std::move(text), std::move(completion));
}

std::string LLMTranslationClient::SystemPrompt(const std::string& target_language,
                                               const std::vector<std::string>& hotwords) {
    std::string prompt =
        "You are a real-time speech translator.\n"
        "Translate the user's text into " + target_language + ".\n"
        "Detect the source language automatically.\n"
        "Return only the translated text, with no explanations, quotes, prefixes, alternatives, or markdown.\n"
        "The text may come from live speech recognition and may contain minor recognition errors; infer the intended meaning when it is clear.\n"
        "Before translating, clean the source text: remove stray pause spaces (especially between CJK characters), fix punctuation, and drop filler words/false starts that add no meaning. Preserve proper nouns and numbers.";
    std::vector<std::string> terms;
    for (auto term : hotwords) {
        term = Trim(std::move(term));
        if (!term.empty()) terms.push_back(std::move(term));
    }
    if (!terms.empty()) {
        prompt += "\n\nImportant terms that may appear:\n";
        for (const auto& term : terms) {
            prompt += "- " + term + "\n";
        }
    }
    return prompt;
}

} // namespace voicestick
