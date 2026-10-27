#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// 会话历史数据底座（用户数据飞轮 P0-3，自 macOS
// Sources/VoiceStickCore/SessionHistory.swift 1:1 移植；typeless 方案 P0-1
// history_v2 裁剪版）：每次识别会话落一行（raw/refined/injected + 设备/Provider/
// 时长/下发热词），为个人评测语料（P2-8）与 dialog_ctx 供数。
//
// schema 与 macOS 完全一致（同建表 SQL、同迁移版本管理）；存储用 vendored
// SQLite amalgamation（third_party/sqlite，公有领域）。打开/迁移失败一律降级
// 停用（CreateSessionHistory 返回 nullptr），历史记录绝不阻塞识别主链路。

inline constexpr char kSessionHistoryEditedStatusDefault[] = "NOT_EXTRACTED";

struct SessionHistoryRecord {
    std::string id;                    // 会话 UUID
    std::int64_t created_at = 0;       // epoch 秒
    std::string device_id;             // 触发设备
    std::string asr_provider;          // volcengine / tencent / voicestick_cloud
    std::string mode;                  // original / translate
    std::string raw_asr;               // ASR 原文
    std::string refined_text;          // 精修后文本
    std::string injected_text;         // 实际注入内容（与 refined 可能不同，如翻译）
    std::string edited_text;           // 用户注入后修改的改动区间（编辑追踪产出）
    std::string edited_text_status = kSessionHistoryEditedStatusDefault;
    int edited_text_attempts = 0;
    std::string audio_local_path;
    std::optional<double> duration;    // 录音时长秒
    std::string focused_app_bundle_id;
    std::string hotwords_sent;         // JSON 数组：本会话 ASR 下发的热词集
    std::string mic_device;
    std::string client_metadata;       // JSON：app 版本等
};

class SessionHistoryStore {
public:
    // 打开并按序应用迁移；任何失败返回 nullptr（调用方降级停用）。
    static std::unique_ptr<SessionHistoryStore> Open(const std::filesystem::path& path);

    ~SessionHistoryStore();

    SessionHistoryStore(const SessionHistoryStore&) = delete;
    SessionHistoryStore& operator=(const SessionHistoryStore&) = delete;

    void Close();

    // 插入一行（best-effort，失败记日志静默——历史记录不阻塞主链路）。
    void Insert(const SessionHistoryRecord& record);

    // 更新实际注入文本（翻译异步完成后回填 injected_text）。
    void UpdateInjectedText(const std::string& id, const std::string& text);

    // 编辑追踪结果回填（Typeless edited_* 三件套语义：状态机 + 尝试计数）。
    void UpdateEditedText(const std::string& id, const std::string& text,
                          const std::string& status, int attempts);

    // 读回某行的 edited_* 三件套（测试与设置页展示用）。
    struct EditedText {
        std::string text;
        std::string status;
        int attempts = 0;
    };
    std::optional<EditedText> GetEditedText(const std::string& id);

    // 行数（测试与导出用）。
    int RowCount();

    // 最近 N 条精修后文本（旧→新；dialog_ctx 接线的数据源）。
    std::vector<std::string> LatestRefinedTexts(int limit);

private:
    SessionHistoryStore() = default;
    bool Exec(const std::string& sql);
    bool Migrate();

    void* handle_ = nullptr;  // sqlite3*
};

}  // namespace voicestick
