#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// 负信号采集（用户数据飞轮 P1-1，自 macOS Sources/VoiceStickCore/UsageEvents.swift
// 1:1 移植；对齐 WeType「完整动作历史：上屏与回退都记录，正负样本都采集」）：
// 待确认粘贴的确认/取消/识别中途取消全部落 JSONL 事件流水，供热词评分扩展负
// 样本维度使用。
//
// 隐私红线：事件只记文本长度与热词命中名单（子串匹配的成员关系），绝不记
// 文本本身；纯本地追加，无遥测。

struct UsageEvent {
    std::string kind;                   // 动作类型（UsageEventKind 常量）
    std::int64_t ts = 0;                // epoch 秒
    std::string device;                 // 触发设备 ID
    int text_length = 0;                // 本次结果文本长度（字符数；只记长度不记内容）
    std::vector<std::string> hotwords;  // 结果文本命中的热词名单
    std::optional<double> dwell_seconds;  // 确认前停留秒数（识别中途取消为 nullopt）
};

namespace usage_event_kind {
inline constexpr char kPasteAutoConfirmed[] = "paste_auto_confirmed";
inline constexpr char kPasteManuallyConfirmed[] = "paste_manually_confirmed";
inline constexpr char kPasteCanceled[] = "paste_canceled";
inline constexpr char kRecognitionCanceled[] = "recognition_canceled";
}  // namespace usage_event_kind

// JSONL 追加一行（文件不存在则创建；写失败静默 best-effort）。
void AppendUsageEvent(const std::filesystem::path& path, const UsageEvent& event);

// JSONL 读取（缺失文件返回空；损坏行跳过）。
std::vector<UsageEvent> LoadUsageEvents(const std::filesystem::path& path);

}  // namespace voicestick
