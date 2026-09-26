#include "hotword_activity.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
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

bool IsCold(const HotwordUsage& usage, std::int64_t now_s) {
    return HotwordScore(usage, now_s) < kHotwordActivityColdThreshold;
}

std::optional<std::string> PickEvictionWord(const std::vector<std::string>& words,
                                            const HotwordUsageStore& store,
                                            bool protect_manual,
                                            std::int64_t now_s) {
    if (words.empty()) return std::nullopt;
    struct Candidate {
        const std::string* word;
        double score;
        bool is_manual;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(words.size());
    for (const auto& word : words) {
        HotwordUsage usage;
        const auto it = store.find(word);
        if (it != store.end()) {
            usage = it->second;
        } else {
            usage.source = "manual";  // 无统计记录：视为手动加词，避免新词最先被逐
        }
        candidates.push_back({&word, HotwordScore(usage, now_s),
                              usage.source == "manual"});
    }
    const auto lowest = [](const std::vector<Candidate>& pool) -> const Candidate* {
        const Candidate* best = nullptr;
        for (const auto& candidate : pool) {
            if (best == nullptr || candidate.score < best->score ||
                (candidate.score == best->score && *candidate.word < *best->word)) {
                best = &candidate;
            }
        }
        return best;
    };
    // 手动加词保护：先在非 manual 词中选最低分；全 manual 时降级逐出最低分
    // manual 词（protect_manual=false 时直接在全体中选最低分）。
    if (protect_manual) {
        std::vector<Candidate> non_manual;
        non_manual.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (!candidate.is_manual) non_manual.push_back(candidate);
        }
        if (!non_manual.empty()) {
            const Candidate* picked = lowest(non_manual);
            return *picked->word;
        }
    }
    if (const Candidate* picked = lowest(candidates)) return *picked->word;
    return std::nullopt;
}

EvictedPool LoadEvictedPool(const std::filesystem::path& path) {
    EvictedPool pool;
    std::ifstream f(path, std::ios::binary);
    if (!f) return pool;
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* root = cJSON_Parse(json.c_str());
    if (!root) return pool;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) continue;
        auto* word = cJSON_GetObjectItemCaseSensitive(item, "word");
        if (!cJSON_IsString(word) || word->valuestring == nullptr) continue;
        EvictedWord entry;
        entry.word = word->valuestring;
        if (auto* category = cJSON_GetObjectItemCaseSensitive(item, "category");
            cJSON_IsString(category) && category->valuestring != nullptr) {
            entry.category = category->valuestring;
        }
        if (auto* ts = cJSON_GetObjectItemCaseSensitive(item, "evicted_ts");
            cJSON_IsNumber(ts)) {
            entry.evicted_ts = static_cast<std::int64_t>(ts->valuedouble);
        }
        if (auto* count = cJSON_GetObjectItemCaseSensitive(item, "count");
            cJSON_IsNumber(count)) {
            entry.usage.count = count->valueint;
        }
        if (auto* ts = cJSON_GetObjectItemCaseSensitive(item, "last_used_ts");
            cJSON_IsNumber(ts)) {
            entry.usage.last_used_ts = static_cast<std::int64_t>(ts->valuedouble);
        }
        if (auto* source = cJSON_GetObjectItemCaseSensitive(item, "source");
            cJSON_IsString(source) && source->valuestring != nullptr) {
            entry.usage.source = source->valuestring;
        }
        pool.push_back(std::move(entry));
    }
    return pool;
}

void SaveEvictedPool(const std::filesystem::path& path, const EvictedPool& pool) {
    auto* root = cJSON_CreateArray();
    if (!root) return;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    for (const auto& entry : pool) {
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "word", entry.word.c_str());
        cJSON_AddStringToObject(item, "category", entry.category.c_str());
        cJSON_AddNumberToObject(item, "evicted_ts", static_cast<double>(entry.evicted_ts));
        cJSON_AddNumberToObject(item, "count", entry.usage.count);
        cJSON_AddNumberToObject(item, "last_used_ts",
                                static_cast<double>(entry.usage.last_used_ts));
        cJSON_AddStringToObject(item, "source", entry.usage.source.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    if (!json) return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << json;
    cJSON_free(json);
}

std::optional<EvictedWord> PickReclaimWord(const EvictedPool& pool, const std::string& text) {
    if (pool.empty() || text.empty()) return std::nullopt;
    for (const auto& entry : pool) {
        if (!entry.word.empty() && ContainsCaseInsensitive(text, entry.word)) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace voicestick
