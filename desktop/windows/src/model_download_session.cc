// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// model_download_session.h 的实现：清单→条目组装、缓存路径、顺序编排。

#include "model_download_session.h"

#include <ShlObj.h>
#include <Windows.h>

#include <string>
#include <utility>

#include "log.h"

namespace voicestick {

std::filesystem::path LocalModelCacheModelsDir() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                    &local_app_data)) ||
        local_app_data == nullptr) {
        return {};
    }
    const std::filesystem::path dir =
        std::filesystem::path(local_app_data) / "VoiceStick" / "models" /
        "sense-voice-int8-2024-07-17";
    CoTaskMemFree(local_app_data);
    return dir;
}

std::vector<ModelDownloadItem> BuildModelDownloadItems(
    const std::filesystem::path& models_dir, bool include_refine) {
    std::vector<ModelDownloadItem> items;
    for (const auto& entry : BundledModelEntries()) {
        if (entry.kind == ModelKind::kRefine && !include_refine) continue;
        for (const auto& file : entry.files) {
            ModelDownloadItem item;
            item.kind = entry.kind;
            item.selected = true;
            item.spec = file;
            // rel_path 为 POSIX 分隔符，Windows path 构造原生接受。
            item.dest = models_dir / file.rel_path;
            items.push_back(std::move(item));
        }
    }
    return items;
}

ModelDownloadSession::ModelDownloadSession(
    std::vector<ModelDownloadItem> items, ModelDownloader* downloader,
    const ModelSessionProgressFn& progress,
    std::shared_ptr<std::atomic<bool>> cancel_flag)
    : items_(std::move(items)), downloader_(downloader), progress_(progress),
      cancel_flag_(std::move(cancel_flag)) {}

ModelDownloadSummary ModelDownloadSession::Run() {
    ModelDownloadSummary summary;
    std::uint64_t total = 0;
    for (const auto& item : items_) {
        if (item.selected) total += item.spec.bytes;
    }
    std::uint64_t done = 0;
    bool asr_all_ok = true;
    bool asr_failed = false;  // ASR 条目失败后不再发起后续条目
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        if (!item.selected) {
            if (item.kind == ModelKind::kRefine) summary.refine_skipped = true;
            continue;
        }
        if (asr_failed) break;
        if (cancel_flag_ && cancel_flag_->load()) {
            summary.cancelled = true;
            if (item.kind == ModelKind::kAsr) asr_all_ok = false;
            break;
        }
        std::error_code ec;
        std::filesystem::create_directories(item.dest.parent_path(), ec);

        const auto wrapped_progress = [&](const DownloadProgress& p) {
            if (!progress_) return;
            ModelSessionProgress session_progress;
            session_progress.item_index = i;
            session_progress.downloaded = done + p.downloaded;
            session_progress.total = total;
            progress_(session_progress);
        };
        const auto is_cancelled = [this] {
            return cancel_flag_ && cancel_flag_->load();
        };
        const DownloadOutcome outcome =
            downloader_->DownloadFile(item.spec, item.dest, wrapped_progress, is_cancelled);
        if (outcome.result == DownloadResult::kOk) {
            done += item.spec.bytes;
            if (item.kind == ModelKind::kRefine) summary.refine_ok = true;
            continue;
        }
        if (outcome.result == DownloadResult::kCancelled) {
            summary.cancelled = true;
            if (item.kind == ModelKind::kAsr) asr_all_ok = false;
            break;
        }
        summary.errors.push_back(item.spec.rel_path + ": " + outcome.error);
        if (item.kind == ModelKind::kAsr) {
            asr_all_ok = false;
            asr_failed = true;
        }
    }
    summary.asr_ok = asr_all_ok;
    // 汇总一行：asr/refine 结果、取消与错误数，供用户报障时直接从日志定位。
    std::string summary_line = "session done: asr_ok=" +
                               std::string(summary.asr_ok ? "1" : "0") +
                               " refine_ok=" + std::string(summary.refine_ok ? "1" : "0") +
                               " refine_skipped=" +
                               std::string(summary.refine_skipped ? "1" : "0") +
                               " cancelled=" + std::string(summary.cancelled ? "1" : "0") +
                               " errors=" + std::to_string(summary.errors.size());
    Log("MDL", summary_line);
    for (const auto& error : summary.errors) {
        Log("MDL", "session error: " + error);
    }
    return summary;
}

}  // namespace voicestick
