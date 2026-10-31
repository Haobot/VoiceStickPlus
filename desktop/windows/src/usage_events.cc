#include "usage_events.h"

#include "cJSON.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace voicestick {

void AppendUsageEvent(const std::filesystem::path& path, const UsageEvent& event) {
    auto* root = cJSON_CreateObject();
    if (!root) return;
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    cJSON_AddStringToObject(root, "kind", event.kind.c_str());
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(event.ts));
    cJSON_AddStringToObject(root, "device", event.device.c_str());
    cJSON_AddNumberToObject(root, "text_length", event.text_length);
    auto* hotwords = cJSON_CreateArray();
    for (const auto& word : event.hotwords) {
        cJSON_AddItemToArray(hotwords, cJSON_CreateString(word.c_str()));
    }
    cJSON_AddItemToObject(root, "hotwords", hotwords);
    if (event.dwell_seconds.has_value()) {
        cJSON_AddNumberToObject(root, "dwell_s", *event.dwell_seconds);
    }

    char* json = cJSON_PrintUnformatted(root);
    if (!json) return;
    // JSONL 追加（原 trunc 直写会丢历史流水；app 模式追加一行，损坏不影响旧行）。
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (f) {
        f << json << "\n";
    }
    cJSON_free(json);
}

std::vector<UsageEvent> LoadUsageEvents(const std::filesystem::path& path) {
    std::vector<UsageEvent> events;
    std::ifstream f(path, std::ios::binary);
    if (!f) return events;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos <= content.size()) {
        std::size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        const std::string line = content.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) {
            if (end >= content.size()) break;
            continue;
        }
        auto* root = cJSON_Parse(line.c_str());
        if (!root) continue;  // 损坏行跳过
        auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
        auto str_field = [root](const char* key) -> std::string {
            auto* node = cJSON_GetObjectItemCaseSensitive(root, key);
            return cJSON_IsString(node) && node->valuestring != nullptr
                       ? std::string(node->valuestring)
                       : std::string();
        };
        UsageEvent event;
        event.kind = str_field("kind");
        if (event.kind.empty()) continue;
        auto* ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
        if (cJSON_IsNumber(ts)) event.ts = static_cast<std::int64_t>(ts->valuedouble);
        event.device = str_field("device");
        auto* length = cJSON_GetObjectItemCaseSensitive(root, "text_length");
        if (cJSON_IsNumber(length)) event.text_length = length->valueint;
        auto* hotwords = cJSON_GetObjectItemCaseSensitive(root, "hotwords");
        if (cJSON_IsArray(hotwords)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, hotwords) {
                if (cJSON_IsString(item) && item->valuestring != nullptr) {
                    event.hotwords.emplace_back(item->valuestring);
                }
            }
        }
        auto* dwell = cJSON_GetObjectItemCaseSensitive(root, "dwell_s");
        if (cJSON_IsNumber(dwell)) event.dwell_seconds = dwell->valuedouble;
        events.push_back(std::move(event));
        if (end >= content.size()) break;
    }
    return events;
}

}  // namespace voicestick
