#include "session_history.h"

#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <fstream>

namespace voicestick {

namespace {

// 迁移 SQL（版本号 = 数组下标 + 1）。与 macOS SessionHistory.swift 完全一致：
// 仿 Typeless drizzle 迁移——按序应用、schema_migrations 记版本，schema 演进
// 只追加不回填。
constexpr const char* kMigrations[] = {
    "CREATE TABLE IF NOT EXISTS history ("
    "  id TEXT PRIMARY KEY,"
    "  created_at INTEGER NOT NULL,"
    "  device_id TEXT,"
    "  asr_provider TEXT,"
    "  mode TEXT,"
    "  raw_asr TEXT,"
    "  refined_text TEXT,"
    "  injected_text TEXT,"
    "  edited_text TEXT,"
    "  edited_text_status TEXT NOT NULL DEFAULT 'NOT_EXTRACTED',"
    "  edited_text_attempts INTEGER NOT NULL DEFAULT 0,"
    "  audio_local_path TEXT,"
    "  duration REAL,"
    "  focused_app_bundle_id TEXT,"
    "  hotwords_sent TEXT,"
    "  mic_device TEXT,"
    "  client_metadata TEXT"
    ")",
};
constexpr int kMigrationCount = static_cast<int>(sizeof(kMigrations) / sizeof(kMigrations[0]));

}  // namespace

std::unique_ptr<SessionHistoryStore> SessionHistoryStore::Open(const std::filesystem::path& path) {
    const std::string native = path.string();
    if (native.empty()) return nullptr;
    sqlite3* db = nullptr;
    // FULLMUTEX：与 macOS 同参数（协调器单写 + 未来设置页/导出读不互堵）。
    if (sqlite3_open_v2(native.c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK || db == nullptr) {
        sqlite3_close(db);
        return nullptr;
    }
    auto store = std::unique_ptr<SessionHistoryStore>(new SessionHistoryStore());
    store->handle_ = db;
    char* error = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &error) != SQLITE_OK) {
        if (error) sqlite3_free(error);
        store.reset();
        return nullptr;
    }
    if (!store->Migrate()) {
        store.reset();
        return nullptr;
    }
    return store;
}

SessionHistoryStore::~SessionHistoryStore() { Close(); }

void SessionHistoryStore::Close() {
    if (handle_ != nullptr) {
        sqlite3_close(static_cast<sqlite3*>(handle_));
        handle_ = nullptr;
    }
}

bool SessionHistoryStore::Exec(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(static_cast<sqlite3*>(handle_), sql.c_str(), nullptr, nullptr, &error) !=
        SQLITE_OK) {
        if (error != nullptr) {
            std::fprintf(stderr, "session history exec failed: %s\n", error);
            sqlite3_free(error);
        }
        return false;
    }
    return true;
}

bool SessionHistoryStore::Migrate() {
    if (!Exec(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "  version INTEGER PRIMARY KEY,"
            "  applied_at INTEGER NOT NULL"
            ")")) {
        return false;
    }
    std::vector<std::int64_t> applied;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_),
                           "SELECT version FROM schema_migrations", -1, &statement, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        applied.push_back(sqlite3_column_int64(statement, 0));
    }
    sqlite3_finalize(statement);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (int index = 0; index < kMigrationCount; ++index) {
        const std::int64_t version = index + 1;
        const bool already = [&] {
            for (const std::int64_t v : applied) {
                if (v == version) return true;
            }
            return false;
        }();
        if (already) continue;
        if (!Exec("BEGIN")) return false;
        bool ok = Exec(kMigrations[index]);
        sqlite3_stmt* insert = nullptr;
        if (ok && sqlite3_prepare_v2(static_cast<sqlite3*>(handle_),
                                     "INSERT INTO schema_migrations(version, applied_at) "
                                     "VALUES(?, ?)",
                                     -1, &insert, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(insert, 1, version);
            sqlite3_bind_int64(insert, 2, now);
            ok = sqlite3_step(insert) == SQLITE_DONE;
        }
        sqlite3_finalize(insert);
        if (!Exec(ok ? "COMMIT" : "ROLLBACK") || !ok) return false;
    }
    return true;
}

namespace {

// SQLITE_TRANSIENT(-1)：SQLite 自拷贝文本（跨语句生命周期安全，与 macOS 一致）。
void BindText(sqlite3_stmt* statement, const int index, const std::string& value) {
    // SQLITE_TRANSIENT(-1)：SQLite 自拷贝文本（跨语句生命周期安全，与 macOS 一致）。
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                      reinterpret_cast<sqlite3_destructor_type>(-1));
}

}  // namespace

void SessionHistoryStore::Insert(const SessionHistoryRecord& record) {
    if (handle_ == nullptr) return;
    const char* sql =
        "INSERT OR REPLACE INTO history("
        "  id, created_at, device_id, asr_provider, mode, raw_asr, refined_text,"
        "  injected_text, edited_text, edited_text_status, edited_text_attempts,"
        "  audio_local_path, duration, focused_app_bundle_id, hotwords_sent,"
        "  mic_device, client_metadata"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_), sql, -1, &statement, nullptr) !=
        SQLITE_OK) {
        return;
    }
    BindText(statement, 1, record.id);
    sqlite3_bind_int64(statement, 2, record.created_at);
    BindText(statement, 3, record.device_id);
    BindText(statement, 4, record.asr_provider);
    BindText(statement, 5, record.mode);
    BindText(statement, 6, record.raw_asr);
    BindText(statement, 7, record.refined_text);
    BindText(statement, 8, record.injected_text);
    BindText(statement, 9, record.edited_text);
    BindText(statement, 10, record.edited_text_status);
    sqlite3_bind_int(statement, 11, record.edited_text_attempts);
    BindText(statement, 12, record.audio_local_path);
    if (record.duration.has_value()) {
        sqlite3_bind_double(statement, 13, *record.duration);
    }
    BindText(statement, 14, record.focused_app_bundle_id);
    BindText(statement, 15, record.hotwords_sent);
    BindText(statement, 16, record.mic_device);
    BindText(statement, 17, record.client_metadata);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void SessionHistoryStore::UpdateInjectedText(const std::string& id, const std::string& text) {
    if (handle_ == nullptr) return;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_),
                           "UPDATE history SET injected_text = ? WHERE id = ?", -1, &statement,
                           nullptr) != SQLITE_OK) {
        return;
    }
    BindText(statement, 1, text);
    BindText(statement, 2, id);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void SessionHistoryStore::UpdateEditedText(const std::string& id, const std::string& text,
                                           const std::string& status, const int attempts) {
    if (handle_ == nullptr) return;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            static_cast<sqlite3*>(handle_),
            "UPDATE history SET edited_text = ?, edited_text_status = ?, "
            "edited_text_attempts = ? WHERE id = ?",
            -1, &statement, nullptr) != SQLITE_OK) {
        return;
    }
    BindText(statement, 1, text);
    BindText(statement, 2, status);
    sqlite3_bind_int(statement, 3, attempts);
    BindText(statement, 4, id);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

std::optional<SessionHistoryStore::EditedText> SessionHistoryStore::GetEditedText(
    const std::string& id) {
    if (handle_ == nullptr) return std::nullopt;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_),
                           "SELECT edited_text, edited_text_status, edited_text_attempts "
                           "FROM history WHERE id = ?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    BindText(statement, 1, id);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    EditedText out;
    if (const auto* text = sqlite3_column_text(statement, 0)) {
        out.text = reinterpret_cast<const char*>(text);
    }
    if (const auto* status = sqlite3_column_text(statement, 1)) {
        out.status = reinterpret_cast<const char*>(status);
    }
    out.attempts = static_cast<int>(sqlite3_column_int64(statement, 2));
    sqlite3_finalize(statement);
    return out;
}

int SessionHistoryStore::RowCount() {
    if (handle_ == nullptr) return 0;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_), "SELECT COUNT(*) FROM history", -1,
                           &statement, nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return 0;
    }
    const int count = static_cast<int>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    return count;
}

std::vector<std::string> SessionHistoryStore::LatestRefinedTexts(const int limit) {
    std::vector<std::string> texts;
    if (handle_ == nullptr) return texts;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(handle_),
                           "SELECT refined_text FROM history WHERE refined_text IS NOT NULL "
                           "ORDER BY created_at DESC LIMIT ?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return texts;
    }
    sqlite3_bind_int(statement, 1, limit);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (const auto* text = sqlite3_column_text(statement, 0)) {
            texts.emplace_back(reinterpret_cast<const char*>(text));
        }
    }
    sqlite3_finalize(statement);
    std::reverse(texts.begin(), texts.end());  // 旧→新（对话语序）
    return texts;
}

}  // namespace voicestick
