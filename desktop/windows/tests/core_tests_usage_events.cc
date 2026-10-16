// 用户数据飞轮 P1-1：负信号事件流水批（usage_events.{h,cc}，自 macOS
// UsageEvents.swift 1:1 移植；与 macOS UsageEventTests 同向量钉死）+ 评分负项
// 消费端（hotword_selector RecordHotwordNegativeInText / 负项量值）。
#include "test_suites.h"

#include "hotword_selector.h"
#include "usage_events.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using voicestick::AppendUsageEvent;
using voicestick::LoadUsageEvents;
using voicestick::UsageEvent;
using voicestick::usage_event_kind::kPasteAutoConfirmed;
using voicestick::usage_event_kind::kPasteCanceled;
using voicestick::usage_event_kind::kRecognitionCanceled;

void TestUsageEventLog() {
    const auto dir = fs::temp_directory_path() / "voicestick_usage_events_test";
    fs::create_directories(dir);
    const auto path = dir / "usage_events.jsonl";
    std::error_code ec;
    fs::remove(path, ec);

    // 缺失文件返回空。
    assert(LoadUsageEvents(path).empty());

    // 追加三行（自动确认 / 取消 / 识别中途取消）。
    UsageEvent confirmed;
    confirmed.kind = kPasteAutoConfirmed;
    confirmed.ts = 1000;
    confirmed.device = "AB12";
    confirmed.text_length = 42;
    confirmed.hotwords = {"语气词"};
    confirmed.dwell_seconds = 0.4;
    AppendUsageEvent(path, confirmed);

    UsageEvent canceled;
    canceled.kind = kPasteCanceled;
    canceled.ts = 2000;
    canceled.device = "AB12";
    canceled.text_length = 18;
    AppendUsageEvent(path, canceled);

    UsageEvent recognition;
    recognition.kind = kRecognitionCanceled;
    recognition.ts = 3000;
    AppendUsageEvent(path, recognition);

    const auto events = LoadUsageEvents(path);
    assert(events.size() == 3);
    assert(events[0].kind == kPasteAutoConfirmed);
    assert(events[0].dwell_seconds.has_value() && *events[0].dwell_seconds == 0.4);
    assert(events[0].hotwords == std::vector<std::string>{"语气词"});
    assert(events[1].kind == kPasteCanceled);
    assert(events[1].hotwords.empty());
    assert(events[2].text_length == 0);
    assert(!events[2].dwell_seconds.has_value());

    // 隐私红线：序列化内容不含文本字段（只记长度与热词名单）。
    std::ifstream f(path, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(raw.find("\"text\":") == std::string::npos);
    assert(raw.find("\"text_length\":") != std::string::npos);

    // 损坏行跳过。
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << raw << "{broken json\n";
    }
    assert(LoadUsageEvents(path).size() == 3);

    fs::remove_all(dir, ec);
}

void TestHotwordNegativeScoring() {
    using namespace voicestick;
    // 与 macOS RefineStreamTests/CorrectionPairsTests 同向量：命中记负、未命中不记。
    HotwordUsageStore neg_store;
    RecordHotwordNegativeInText(neg_store, "讲一下语气词的用法", {"语气词", "Claude"});
    assert(neg_store.at("语气词").negative_count == 1);
    assert(!neg_store.contains("Claude"));
    const std::int64_t now_ts = 1'800'000'000;
    const HotwordUsage neg_scored{50, 9, now_ts - 100, "mined"};
    const HotwordUsage pos_scored{50, 0, now_ts - 100, "mined"};
    assert(HotwordScore(neg_scored, now_ts) < HotwordScore(pos_scored, now_ts));
    const double delta = HotwordScore(pos_scored, now_ts) - HotwordScore(neg_scored, now_ts);
    assert(std::abs(delta - kHotwordWNegative * std::log1p(9.0)) < 1e-9);

    // JSON 往返保留 negative_count。
    const auto dir = fs::temp_directory_path() / "voicestick_negative_scoring";
    fs::create_directories(dir);
    const auto path = dir / "hotword_usage.json";
    std::error_code ec;
    fs::remove(path, ec);
    SaveHotwordUsage(path, neg_store);
    const auto loaded = LoadHotwordUsage(path);
    assert(loaded.at("语气词").negative_count == 1);
    fs::remove_all(dir, ec);
}

void RunUsageEventsBatchTests() {
    TestUsageEventLog();
    TestHotwordNegativeScoring();
}
