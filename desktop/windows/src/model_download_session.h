// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地模型下载会话（Doc/Plan/local-model-distribution.md 迭代二）：把内置
// 清单组装成待下载条目，顺序流经 ModelDownloader，聚合全局进度与部分成功
// 语义（ASR 必选条目失败即整体失败；精修条目失败/跳过不阻塞）。
// 纯编排逻辑，不碰 UI；向导对话框在后台线程调 Run()。

#ifndef VOICESTICK_MODEL_DOWNLOAD_SESSION_H_
#define VOICESTICK_MODEL_DOWNLOAD_SESSION_H_

#include "model_downloader.h"
#include "model_manifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace voicestick {

// 会话全局进度：downloaded 含续传基数与已完成条目字节。
struct ModelSessionProgress {
    std::size_t item_index = 0;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
};
using ModelSessionProgressFn = std::function<void(const ModelSessionProgress&)>;

// 单个待下载文件（由清单条目展开：ASR 两文件 + 精修一文件）。
struct ModelDownloadItem {
    ModelKind kind = ModelKind::kAsr;
    bool selected = true;  // 精修条目可被用户取消勾选
    ModelFileSpec spec;
    std::filesystem::path dest;
};

struct ModelDownloadSummary {
    // 必选（kAsr）条目全部成功；失败或取消则 false。
    bool asr_ok = false;
    // 精修条目被跳过（未勾选）。
    bool refine_skipped = false;
    // 精修条目勾选且成功。
    bool refine_ok = false;
    bool cancelled = false;
    // 每个失败条目一行：展示名 + 失败原因摘要。
    std::vector<std::string> errors;
};

// 缓存根目录：%LOCALAPPDATA%\VoiceStick\models\sense-voice-int8-2024-07-17
//（models_dir 直接指向 ASR 目录，GGUF 恰为其子目录，与现有解析零改动兼容）。
std::filesystem::path LocalModelCacheModelsDir();

// 组装下载条目：include_refine=false 时不含精修（用户跳过）。
std::vector<ModelDownloadItem> BuildModelDownloadItems(
    const std::filesystem::path& models_dir, bool include_refine);

class ModelDownloadSession {
 public:
    // items：BuildModelDownloadItems 产物（selected 已按用户勾选置位）。
    // progress / cancel_flag：向导注入；cancel_flag 由 UI 关闭与取消按钮置位。
    ModelDownloadSession(std::vector<ModelDownloadItem> items,
                         ModelDownloader* downloader,
                         const ModelSessionProgressFn& progress = {},
                         std::shared_ptr<std::atomic<bool>> cancel_flag =
                             std::make_shared<std::atomic<bool>>(false));

    // 同步执行（后台线程调用）。条目顺序：kAsr 失败或取消即停止后续条目；
    // kRefine 失败记录后自然收尾。每条目下载前创建 dest 父目录。
    ModelDownloadSummary Run();

 private:
    std::vector<ModelDownloadItem> items_;
    ModelDownloader* downloader_;
    ModelSessionProgressFn progress_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

}  // namespace voicestick

#endif  // VOICESTICK_MODEL_DOWNLOAD_SESSION_H_
