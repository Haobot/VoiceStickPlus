#include "correction_pairs.h"

#include "cJSON.h"
#include "pinyin_guard.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

namespace voicestick {

namespace {

// ---- UTF-8 码点工具（风格对齐 hotword_selector.cc 的手写解码）----

std::vector<char32_t> DecodeCodepoints(std::string_view text) {
    std::vector<char32_t> cps;
    cps.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        char32_t cp = 0xFFFD;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0 && i + 1 < text.size()) {
            len = 2;
            cp = (lead & 0x1F) << 6 | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        } else if ((lead & 0xF0) == 0xE0 && i + 2 < text.size()) {
            len = 3;
            cp = (lead & 0x0F) << 12 | (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3F);
        } else if ((lead & 0xF8) == 0xF0 && i + 3 < text.size()) {
            len = 4;
            cp = (lead & 0x07) << 18 | (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12 |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3F);
        }
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

std::string EncodeCodepoints(const std::vector<char32_t>& cps) {
    std::string out;
    for (const char32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | cp >> 18));
            out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool IsAsciiSpace(char32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'; }

// CJK 基本区/扩展A 或字母数字：过滤纯标点/空白调整（对 few-shot 无锚定价值）。
bool IsContentChar(char32_t cp) {
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) return true;
    const auto c = static_cast<unsigned char>(cp);
    return cp < 0x80 && (std::isalnum(c) != 0);
}

bool HasContentChar(const std::vector<char32_t>& cps) {
    for (const char32_t cp : cps) {
        if (IsContentChar(cp)) return true;
    }
    return false;
}

// 去除首尾 ASCII 空白（diff 片段经常捎带周边空格，few-shot 锚点不需要）。
std::vector<char32_t> TrimSpaces(const std::vector<char32_t>& cps) {
    std::size_t begin = 0;
    std::size_t end = cps.size();
    while (begin < end && IsAsciiSpace(cps[begin])) ++begin;
    while (end > begin && IsAsciiSpace(cps[end - 1])) --end;
    return std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(begin),
                                 cps.begin() + static_cast<std::ptrdiff_t>(end));
}

// 提取进程级文件互斥（coordinator 精修回调与设置页/未来通道同写一个文件）。
std::mutex& CorrectionPairsFileMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::vector<std::pair<std::string, std::string>> ExtractReplacementPairs(
    std::string_view original, std::string_view refined) {
    // 整句改写量级护栏：两侧码点数超限不提取（DP 表与产出质量双保险）。
    constexpr std::size_t kMaxCodepoints = 512;
    const auto a = DecodeCodepoints(original);
    const auto b = DecodeCodepoints(refined);
    if (a.size() > kMaxCodepoints || b.size() > kMaxCodepoints) return {};

    // LCS（码点级）求最长公共子序列，未匹配的连续片段即替换对。
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::uint16_t> dp((n + 1) * (m + 1), 0);
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            dp[i * (m + 1) + j] = a[i - 1] == b[j - 1]
                                      ? static_cast<std::uint16_t>(dp[(i - 1) * (m + 1) + j - 1] + 1)
                                      : std::max(dp[(i - 1) * (m + 1) + j], dp[i * (m + 1) + j - 1]);
        }
    }
    // 回溯收集匹配点（按 original 坐标升序还原）。
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            matches.emplace_back(i - 1, j - 1);
            --i;
            --j;
        } else if (dp[(i - 1) * (m + 1) + j] >= dp[i * (m + 1) + j - 1]) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(matches.begin(), matches.end());

    // 相邻匹配点之间的未匹配片段即一次替换；两侧均为空（纯匹配）跳过。
    std::vector<std::pair<std::string, std::string>> pairs;
    std::size_t prev_a = 0;
    std::size_t prev_b = 0;
    auto emit = [&](std::size_t end_a, std::size_t end_b) {
        if (end_a == prev_a && end_b == prev_b) return;
        auto before = TrimSpaces({a.begin() + static_cast<std::ptrdiff_t>(prev_a),
                                  a.begin() + static_cast<std::ptrdiff_t>(end_a)});
        auto after = TrimSpaces({b.begin() + static_cast<std::ptrdiff_t>(prev_b),
                                 b.begin() + static_cast<std::ptrdiff_t>(end_b)});
        prev_a = end_a;
        prev_b = end_b;
        if (before.empty() || after.empty()) return;                    // 删除/插入不入表
        if (before.size() > static_cast<std::size_t>(kCorrectionPairMaxSideChars) ||
            after.size() > static_cast<std::size_t>(kCorrectionPairMaxSideChars)) {
            return;                                                     // 整句改写量级
        }
        if (!HasContentChar(before) || !HasContentChar(after)) return;  // 纯标点/空白调整
        pairs.emplace_back(EncodeCodepoints(before), EncodeCodepoints(after));
    };
    for (const auto& [ma, mb] : matches) {
        emit(ma, mb);
        prev_a = ma + 1;
        prev_b = mb + 1;
    }
    emit(n, m);

    // 同一次提取内去重（保序）。
    std::vector<std::pair<std::string, std::string>> deduped;
    for (auto& pair : pairs) {
        bool seen = false;
        for (const auto& existing : deduped) {
            if (existing == pair) {
                seen = true;
                break;
            }
        }
        if (!seen) deduped.push_back(std::move(pair));
    }
    return deduped;
}

void RecordCorrectionPairs(std::vector<CorrectionPair>& store,
                           const std::vector<std::pair<std::string, std::string>>& pairs,
                           std::int64_t now_ts, const std::string& app) {
    for (const auto& [before, after] : pairs) {
        // 合并键含 app（对齐 doubao UNIQUE(src,dst,app_bucket)——同对在不同桶各自累计）。
        CorrectionPair* existing = nullptr;
        for (auto& entry : store) {
            if (entry.before == before && entry.after == after && entry.app == app) {
                existing = &entry;
                break;
            }
        }
        if (existing != nullptr) {
            existing->count += 1;
            existing->last_seen_ts = now_ts;
            continue;
        }
        CorrectionPair entry;
        entry.before = before;
        entry.after = after;
        entry.count = 1;
        // 入表闸门（doubao §3.2 拼音校验前置）：同音/近音对标注，非同音不丢弃只降权。
        entry.same_pronunciation = SameOrNearText(before, after);
        entry.last_seen_ts = now_ts;
        entry.app = app;
        store.push_back(std::move(entry));
    }
    // 容量硬上限：count 升序、last_seen 升序、字典序稳定逐出。
    if (store.size() > kCorrectionPairStoreCap) {
        std::vector<std::size_t> order(store.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&store](std::size_t lhs, std::size_t rhs) {
            const auto& x = store[lhs];
            const auto& y = store[rhs];
            if (x.count != y.count) return x.count < y.count;
            if (x.last_seen_ts != y.last_seen_ts) return x.last_seen_ts < y.last_seen_ts;
            if (x.before != y.before) return x.before < y.before;
            return x.after < y.after;
        });
        std::vector<bool> evict(store.size(), false);
        const std::size_t to_evict = store.size() - kCorrectionPairStoreCap;
        for (std::size_t k = 0; k < to_evict; ++k) evict[order[k]] = true;
        std::vector<CorrectionPair> kept;
        kept.reserve(kCorrectionPairStoreCap);
        for (std::size_t k = 0; k < store.size(); ++k) {
            if (!evict[k]) kept.push_back(std::move(store[k]));
        }
        store = std::move(kept);
    }
}

std::vector<CorrectionPair> LoadCorrectionPairs(const std::filesystem::path& path) {
    std::vector<CorrectionPair> store;
    std::ifstream f(path, std::ios::binary);
    if (!f) return store;
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* root = cJSON_Parse(json.c_str());
    if (!root) return store;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* pairs = cJSON_GetObjectItemCaseSensitive(root, "pairs");
    if (!cJSON_IsArray(pairs)) return store;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, pairs) {
        if (!cJSON_IsObject(item)) continue;
        auto str_field = [item](const char* key) -> std::string {
            auto* node = cJSON_GetObjectItemCaseSensitive(item, key);
            return cJSON_IsString(node) && node->valuestring != nullptr ? std::string(node->valuestring)
                                                                        : std::string();
        };
        CorrectionPair pair;
        pair.before = str_field("before");
        pair.after = str_field("after");
        if (pair.before.empty() || pair.after.empty()) continue;
        auto* count = cJSON_GetObjectItemCaseSensitive(item, "count");
        if (cJSON_IsNumber(count)) pair.count = count->valueint;
        auto* use_count = cJSON_GetObjectItemCaseSensitive(item, "use_count");
        if (cJSON_IsNumber(use_count)) pair.use_count = use_count->valueint;
        auto* same = cJSON_GetObjectItemCaseSensitive(item, "same_pronunciation");
        if (cJSON_IsBool(same)) pair.same_pronunciation = cJSON_IsTrue(same);
        auto* last_seen = cJSON_GetObjectItemCaseSensitive(item, "last_seen_ts");
        if (cJSON_IsNumber(last_seen)) pair.last_seen_ts = static_cast<std::int64_t>(last_seen->valuedouble);
        pair.source = str_field("source");
        if (pair.source.empty()) pair.source = "refine";
        pair.app = str_field("app");
        store.push_back(std::move(pair));
    }
    return store;
}

void SaveCorrectionPairs(const std::filesystem::path& path,
                         const std::vector<CorrectionPair>& store) {
    // 保存前按 (before, after, app) 字典序排序，文件内容确定性（便于比对与 diff）。
    std::vector<const CorrectionPair*> sorted;
    sorted.reserve(store.size());
    for (const auto& entry : store) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const CorrectionPair* lhs, const CorrectionPair* rhs) {
        if (lhs->before != rhs->before) return lhs->before < rhs->before;
        if (lhs->after != rhs->after) return lhs->after < rhs->after;
        return lhs->app < rhs->app;
    });

    auto* root = cJSON_CreateObject();
    if (!root) return;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    cJSON_AddNumberToObject(root, "version", 1);
    auto* pairs = cJSON_CreateArray();
    for (const CorrectionPair* entry : sorted) {
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "before", entry->before.c_str());
        cJSON_AddStringToObject(item, "after", entry->after.c_str());
        cJSON_AddNumberToObject(item, "count", entry->count);
        cJSON_AddNumberToObject(item, "use_count", entry->use_count);
        cJSON_AddBoolToObject(item, "same_pronunciation", entry->same_pronunciation);
        cJSON_AddNumberToObject(item, "last_seen_ts", static_cast<double>(entry->last_seen_ts));
        cJSON_AddStringToObject(item, "source", entry->source.c_str());
        cJSON_AddStringToObject(item, "app", entry->app.c_str());
        cJSON_AddItemToArray(pairs, item);
    }
    cJSON_AddItemToObject(root, "pairs", pairs);

    char* json = cJSON_PrintUnformatted(root);
    if (!json) return;
    // B13：先写同目录临时文件再改名（trunc 直写会被并发读到半截 JSON 判空）。
    // 改名失败（极端）回退直写，宁可退化为旧行为也不丢数据。
    const std::filesystem::path tmp = path.string() + ".tmp";
    bool wrote = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            f << json;
            wrote = static_cast<bool>(f);
        }
    }
    if (wrote) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        wrote = !ec;
    }
    if (!wrote) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) f << json;
    }
    cJSON_free(json);
}

void RecordCorrectionPairsToDisk(const std::filesystem::path& path,
                                 const std::vector<std::pair<std::string, std::string>>& pairs,
                                 std::int64_t now_ts, const std::string& app) {
    if (pairs.empty()) return;
    std::lock_guard lock(CorrectionPairsFileMutex());
    // 每次从磁盘重读（reload-merge-save）：绝不使用陈旧快照写回。
    std::vector<CorrectionPair> store = LoadCorrectionPairs(path);
    RecordCorrectionPairs(store, pairs, now_ts, app);
    SaveCorrectionPairs(path, store);
}

// 分桶匹配（与 macOS isBucketMatch 同语义）：app 为空/nil（未分工桶）= 全局查询；
// 否则当前应用桶 ∪ 全局桶。
bool AnonBucketMatch(const std::string& entry_app, const std::string& app) {
    if (app.empty()) return entry_app.empty();
    return entry_app == app || entry_app.empty();
}

// 当前应用桶判定（与 macOS isCurrentBucket 同语义）。
bool AnonIsCurrentBucket(const std::string& entry_app, const std::string& app) {
    return !app.empty() && entry_app == app;
}

std::optional<CorrectionPair> ExactMatchCorrectionPair(
    const std::string& text, const std::vector<CorrectionPair>& store,
    const std::string& app) {
    // 去首尾 ASCII 空白后全串比较（ASR final 偶带首尾空格）。
    std::string trimmed(text);
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    trimmed.erase(trimmed.begin(), std::find_if_not(trimmed.begin(), trimmed.end(), is_space));
    trimmed.erase(std::find_if_not(trimmed.rbegin(), trimmed.rend(), is_space).base(), trimmed.end());
    if (trimmed.empty()) return std::nullopt;
    const CorrectionPair* best = nullptr;
    for (const auto& entry : store) {
        if (entry.count <= 0) continue;
        std::string before_trimmed = entry.before;
        before_trimmed.erase(before_trimmed.begin(),
                             std::find_if_not(before_trimmed.begin(), before_trimmed.end(), is_space));
        before_trimmed.erase(
            std::find_if_not(before_trimmed.rbegin(), before_trimmed.rend(), is_space).base(),
            before_trimmed.end());
        if (before_trimmed != trimmed) continue;
        if (!AnonBucketMatch(entry.app, app)) continue;
        // 当前桶优先 → 同音优先 → count 降序 → last_seen 降序 → 字典序（确定性）。
        const bool entry_current = AnonIsCurrentBucket(entry.app, app);
        if (best == nullptr) {
            best = &entry;
            continue;
        }
        const bool best_current = AnonIsCurrentBucket(best->app, app);
        if (entry_current != best_current && entry_current) {
            best = &entry;
            continue;
        }
        if (entry_current == best_current && entry.same_pronunciation != best->same_pronunciation &&
            entry.same_pronunciation) {
            best = &entry;
            continue;
        }
        if (entry_current == best_current && entry.same_pronunciation == best->same_pronunciation &&
            (entry.count > best->count ||
             (entry.count == best->count && entry.last_seen_ts > best->last_seen_ts) ||
             (entry.count == best->count && entry.last_seen_ts == best->last_seen_ts &&
              (entry.before < best->before ||
               (entry.before == best->before && entry.after < best->after))))) {
            best = &entry;
        }
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

void MarkCorrectionPairUsedOnDisk(const std::filesystem::path& path,
                                  const CorrectionPair& pair, std::int64_t now_ts) {
    std::lock_guard lock(CorrectionPairsFileMutex());
    std::vector<CorrectionPair> store = LoadCorrectionPairs(path);
    for (auto& entry : store) {
        if (entry.before == pair.before && entry.after == pair.after && entry.app == pair.app) {
            entry.use_count += 1;
            entry.last_seen_ts = now_ts;
            break;
        }
    }
    SaveCorrectionPairs(path, store);
}

std::vector<CorrectionPair> TrimCorrectionPairsForPrompt(const std::vector<CorrectionPair>& store,
                                                         int max_pairs, const std::string& app) {
    std::vector<const CorrectionPair*> candidates;
    candidates.reserve(store.size());
    for (const auto& entry : store) {
        if (entry.count > 0 && AnonBucketMatch(entry.app, app)) candidates.push_back(&entry);
    }
    // 当前桶优先 → 同音优先 → count 降序 → last_seen 降序 → 字典序（确定性）。
    std::sort(candidates.begin(), candidates.end(),
              [app](const CorrectionPair* lhs, const CorrectionPair* rhs) {
                  if (AnonIsCurrentBucket(lhs->app, app) != AnonIsCurrentBucket(rhs->app, app)) {
                      return AnonIsCurrentBucket(lhs->app, app);
                  }
                  if (lhs->same_pronunciation != rhs->same_pronunciation) {
                      return lhs->same_pronunciation;
                  }
                  if (lhs->count != rhs->count) return lhs->count > rhs->count;
                  if (lhs->last_seen_ts != rhs->last_seen_ts) return lhs->last_seen_ts > rhs->last_seen_ts;
                  if (lhs->before != rhs->before) return lhs->before < rhs->before;
                  if (lhs->after != rhs->after) return lhs->after < rhs->after;
                  return lhs->app < rhs->app;
              });
    if (max_pairs <= 0) return {};
    if (candidates.size() > static_cast<std::size_t>(max_pairs)) {
        candidates.resize(static_cast<std::size_t>(max_pairs));
    }
    std::vector<CorrectionPair> result;
    result.reserve(candidates.size());
    for (const CorrectionPair* entry : candidates) result.push_back(*entry);
    return result;
}

std::string BuildCorrectionFewshotSection(const std::vector<CorrectionPair>& pairs) {
    if (pairs.empty()) return "";
    std::string section =
        "\n\n用户历史纠正（输出时请遵循这些偏好：仅当识别文本出现与纠正前写法相同或同音、"
        "近音的错误时替换为纠正后写法，其余内容不得改动）：";
    int appended = 0;
    for (const auto& pair : pairs) {
        // 格式安全过滤：含引号/换行的对会破坏行式 few-shot 结构，跳过。
        if (pair.before.empty() || pair.after.empty()) continue;
        if (pair.before.find('"') != std::string::npos ||
            pair.after.find('"') != std::string::npos) {
            continue;
        }
        if (pair.before.find('\n') != std::string::npos ||
            pair.after.find('\n') != std::string::npos) {
            continue;
        }
        section += "\n- \"" + pair.before + "\" → \"" + pair.after + "\"";
        ++appended;
    }
    if (appended == 0) return "";
    return section;
}

std::filesystem::path CorrectionPairsPath(const std::filesystem::path& config_path) {
    return config_path.parent_path() / "correction_pairs.json";
}

} // namespace voicestick
