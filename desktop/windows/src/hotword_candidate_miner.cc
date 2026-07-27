#include "hotword_candidate_miner.h"

#include "cJSON.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <memory>

namespace voicestick {

namespace {

bool IsTokenChar(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0 && c < 0x80 || ch == '.' || ch == '_' || ch == '-';
}

// 标识符样式判定：字母开头（排除 "2.0" 这类版本号/纯数字），且至少含一个
// 大写字母/数字/./_/-（排除普通小写英文单词）。
bool LooksLikeIdentifier(const std::string& token) {
    if (token.size() < 3) return false;
    if (std::isalpha(static_cast<unsigned char>(token.front())) == 0) return false;
    for (char ch : token) {
        const auto c = static_cast<unsigned char>(ch);
        if ((std::isupper(c) != 0) || (std::isdigit(c) != 0) || ch == '.' || ch == '_' || ch == '-') {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> ExtractIdentifierTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    auto flush = [&]() {
        while (!current.empty() &&
               (current.back() == '.' || current.back() == '_' || current.back() == '-')) {
            current.pop_back();
        }
        if (LooksLikeIdentifier(current)) tokens.push_back(current);
        current.clear();
    };
    for (char ch : text) {
        if (IsTokenChar(ch)) {
            current.push_back(ch);
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

std::vector<std::string> MineRefinementCandidates(
    const std::string& original,
    const std::string& refined,
    const std::vector<std::string>& existing_hotwords) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    for (const auto& token : ExtractIdentifierTokens(refined)) {
        if (!seen.insert(token).second) continue;
        if (original.find(token) != std::string::npos) continue;
        bool is_hotword = false;
        for (const auto& hotword : existing_hotwords) {
            if (hotword == token) {
                is_hotword = true;
                break;
            }
        }
        if (!is_hotword) candidates.push_back(token);
    }
    return candidates;
}

HotwordCandidateStore LoadHotwordCandidates(const std::filesystem::path& path) {
    HotwordCandidateStore store;
    std::ifstream f(path, std::ios::binary);
    if (!f) return store;
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* root = cJSON_Parse(json.c_str());
    if (!root) return store;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);

    auto* counts = cJSON_GetObjectItemCaseSensitive(root, "counts");
    if (cJSON_IsObject(counts)) {
        for (auto* item = counts->child; item != nullptr; item = item->next) {
            if (cJSON_IsNumber(item) && item->string != nullptr) {
                store.counts[item->string] = item->valueint;
            }
        }
    }
    auto load_set = [root](const char* key, std::set<std::string>* out) {
        auto* array = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsArray(array)) return;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, array) {
            if (cJSON_IsString(item) && item->valuestring != nullptr) out->insert(item->valuestring);
        }
    };
    load_set("dismissed", &store.dismissed);
    load_set("notified", &store.notified);
    return store;
}

void SaveHotwordCandidates(const std::filesystem::path& path, const HotwordCandidateStore& store) {
    auto* root = cJSON_CreateObject();
    if (!root) return;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* counts = cJSON_CreateObject();
    for (const auto& [word, count] : store.counts) {
        cJSON_AddNumberToObject(counts, word.c_str(), count);
    }
    cJSON_AddItemToObject(root, "counts", counts);
    auto add_set = [root](const char* key, const std::set<std::string>& values) {
        auto* array = cJSON_CreateArray();
        for (const auto& value : values) cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str()));
        cJSON_AddItemToObject(root, key, array);
    };
    add_set("dismissed", store.dismissed);
    add_set("notified", store.notified);

    char* json = cJSON_PrintUnformatted(root);
    if (!json) return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << json;
    cJSON_free(json);
}

std::vector<std::string> RecordHotwordCandidates(HotwordCandidateStore& store,
                                                 const std::vector<std::string>& words) {
    std::vector<std::string> newly_suggested;
    for (const auto& word : words) {
        if (store.dismissed.contains(word) || store.notified.contains(word)) continue;
        const int before = store.counts[word];
        const int after = before + 1;
        store.counts[word] = after;
        if (before < kHotwordCandidateThreshold && after >= kHotwordCandidateThreshold) {
            newly_suggested.push_back(word);
        }
    }
    return newly_suggested;
}

std::vector<std::string> PendingHotwordSuggestions(const HotwordCandidateStore& store) {
    std::vector<std::string> pending;
    for (const auto& [word, count] : store.counts) {
        if (count >= kHotwordCandidateThreshold && !store.dismissed.contains(word)) {
            pending.push_back(word);
        }
    }
    return pending;
}

} // namespace voicestick
