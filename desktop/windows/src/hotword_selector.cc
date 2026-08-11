#include "hotword_selector.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

namespace voicestick {

namespace {

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty() || text.size() < needle.size()) return false;
    auto to_lower = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(text[i + j]) != to_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

bool IsValidHotword(const std::string& word) {
    if (word.empty()) return false;
    int cjk = 0;
    int ascii = 0;
    for (std::size_t i = 0; i < word.size();) {
        const auto lead = static_cast<unsigned char>(word[i]);
        if (lead < 0x80) {
            if (std::isspace(lead) != 0) return false;
            ++ascii;
            ++i;
        } else {
            std::size_t seq_len = 1;
            if ((lead & 0xe0) == 0xc0) seq_len = 2;
            else if ((lead & 0xf0) == 0xe0) seq_len = 3;
            else if ((lead & 0xf8) == 0xf0) seq_len = 4;
            ++cjk;
            i += seq_len;
        }
    }
    return cjk <= 10 && ascii <= 30;
}

double HotwordScore(const HotwordUsage& usage, std::int64_t now_s) {
    const double count_term =
        kHotwordWCount * std::log1p(static_cast<double>(std::max(0, usage.count)));
    double recency = 0.0;
    if (usage.last_used_ts > 0) {
        const double age = std::max(0.0, static_cast<double>(now_s - usage.last_used_ts));
        recency = kHotwordWRecency * std::exp(-age / static_cast<double>(kHotwordRecencyTauS));
    }
    const double manual = usage.source == "manual" ? kHotwordWManual : 0.0;
    return count_term + recency + manual;
}

std::vector<std::string> RankHotwords(const HotwordUsageStore& store,
                                      const std::vector<std::string>& hotwords,
                                      std::int64_t now_s) {
    struct Entry {
        std::string word;
        double score;
    };
    std::vector<Entry> entries;
    entries.reserve(hotwords.size());
    for (const auto& word : hotwords) {
        if (!IsValidHotword(word)) continue;
        HotwordUsage usage;
        if (const auto it = store.find(word); it != store.end()) {
            usage = it->second;
        } else {
            usage.source = "manual";  // 库外词按用户手动加词处理，避免新词最先被裁
        }
        entries.push_back({word, HotwordScore(usage, now_s)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.word < b.word;
    });
    std::vector<std::string> ranked;
    ranked.reserve(entries.size());
    for (const auto& entry : entries) ranked.push_back(entry.word);
    return ranked;
}

std::vector<std::string> TrimHotwordsForPrompt(const HotwordUsageStore& store,
                                               const std::vector<std::string>& hotwords,
                                               int max_words,
                                               std::int64_t now_s) {
    auto ranked = RankHotwords(store, hotwords, now_s);
    if (max_words <= 0) return {};
    if (static_cast<int>(ranked.size()) <= max_words) return ranked;
    ranked.resize(static_cast<std::size_t>(max_words));
    return ranked;
}

void RecordHotwordUsageInText(HotwordUsageStore& store,
                              const std::string& text,
                              const std::vector<std::string>& hotwords,
                              std::int64_t now_s) {
    for (const auto& word : hotwords) {
        if (word.empty() || !ContainsCaseInsensitive(text, word)) continue;
        auto it = store.find(word);
        if (it == store.end()) {
            it = store.emplace(word, HotwordUsage{}).first;
            it->second.source = "manual";  // 热词表内所有词均为用户动作加入
        }
        auto& usage = it->second;
        ++usage.count;
        usage.last_used_ts = now_s;
    }
}

HotwordUsageStore LoadHotwordUsage(const std::filesystem::path& path) {
    HotwordUsageStore store;
    std::ifstream f(path, std::ios::binary);
    if (!f) return store;
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* root = cJSON_Parse(json.c_str());
    if (!root) return store;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) continue;
        auto* word = cJSON_GetObjectItemCaseSensitive(item, "word");
        if (!cJSON_IsString(word) || word->valuestring == nullptr) continue;
        HotwordUsage usage;
        if (auto* count = cJSON_GetObjectItemCaseSensitive(item, "count");
            cJSON_IsNumber(count)) {
            usage.count = count->valueint;
        }
        if (auto* ts = cJSON_GetObjectItemCaseSensitive(item, "last_used_ts");
            cJSON_IsNumber(ts)) {
            usage.last_used_ts = static_cast<std::int64_t>(ts->valuedouble);
        }
        if (auto* source = cJSON_GetObjectItemCaseSensitive(item, "source");
            cJSON_IsString(source) && source->valuestring != nullptr) {
            usage.source = source->valuestring;
        }
        store[word->valuestring] = std::move(usage);
    }
    return store;
}

void SaveHotwordUsage(const std::filesystem::path& path, const HotwordUsageStore& store) {
    auto* root = cJSON_CreateArray();
    if (!root) return;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    for (const auto& [word, usage] : store) {
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "word", word.c_str());
        cJSON_AddNumberToObject(item, "count", usage.count);
        cJSON_AddNumberToObject(item, "last_used_ts", static_cast<double>(usage.last_used_ts));
        cJSON_AddStringToObject(item, "source", usage.source.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    if (!json) return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << json;
    cJSON_free(json);
}

} // namespace voicestick
