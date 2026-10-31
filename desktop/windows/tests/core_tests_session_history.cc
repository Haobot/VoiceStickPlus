// 用户数据飞轮 P0-3 Windows 接线：会话历史批（session_history.{h,cc}，自 macOS
// SessionHistory.swift 1:1 移植；与 macOS SessionHistoryTests 同向量钉死——
// 迁移幂等、插入读回、injected/edited 回填、损坏路径降级）。
#include "test_suites.h"

#include "session_history.h"

#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using voicestick::SessionHistoryRecord;
using voicestick::SessionHistoryStore;

void TestSessionHistoryStore() {
    const auto dir = fs::temp_directory_path() / "voicestick_session_history_tests_win";
    fs::create_directories(dir);
    const auto db_path = dir / "session_history.db";
    std::error_code ec;
    fs::remove(db_path, ec);

    // 打开 + 迁移 + 插入读回。
    auto store = SessionHistoryStore::Open(db_path);
    assert(store != nullptr);
    assert(store->RowCount() == 0);

    SessionHistoryRecord first;
    first.id = "session-1";
    first.created_at = 1760000000;
    first.device_id = "AB12";
    first.asr_provider = "volcengine";
    first.mode = "original";
    first.raw_asr = "这个鱼器渍是什么";
    first.refined_text = "这个语气词是什么";
    first.injected_text = "这个语气词是什么";
    first.duration = 3.2;
    first.hotwords_sent = "[\"语气词\"]";
    first.client_metadata = "{\"app_version\":\"2.4.10\"}";
    store->Insert(first);
    const SessionHistoryRecord second = SessionHistoryRecord("session-2", 1760000100);
    assert(second.edited_text_status == voicestick::kSessionHistoryEditedStatusDefault);
    assert(second.edited_text_attempts == 0);
    store->Insert(second);
    assert(store->RowCount() == 2);

    // 重开同库：迁移幂等，存量资产行数不丢。
    store->Close();
    auto reopened = SessionHistoryStore::Open(db_path);
    assert(reopened != nullptr);
    assert(reopened->RowCount() == 2);
    const auto latest = reopened->LatestRefinedTexts(5);
    assert(latest.size() == 1 && latest.front() == "这个语气词是什么");

    // translate 注入文本回填（不新增行）。
    reopened->UpdateInjectedText("session-2", "translated text");
    assert(reopened->RowCount() == 2);

    // edited_* 回填与读回。
    reopened->UpdateEditedText("session-1", "啊", "EXTRACTED", 2);
    const auto edited = reopened->GetEditedText("session-1");
    assert(edited.has_value());
    assert(edited->text == "啊" && edited->status == "EXTRACTED" && edited->attempts == 2);
    assert(!reopened->GetEditedText("missing").has_value());

    // 目录路径打不开 → nullptr（降级停用，不抛错）。
    assert(SessionHistoryStore::Open(dir) == nullptr);

    fs::remove_all(dir, ec);
}

void RunSessionHistoryBatchTests() {
    TestSessionHistoryStore();
}
