// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地模型下载向导（Doc/Plan/local-model-distribution.md 迭代二）：条目勾选
// （识别必选 / 精修可选）→ 后台线程跑 ModelDownloadSession → PostMessage 报
// 进度与结果。非模态（设置页保持打开），owner 为设置窗口；关闭时置取消旗标
// 并 join 下载线程，避免 downloader 生命周期悬空。

#ifndef VOICESTICK_MODEL_DOWNLOAD_DIALOG_H_
#define VOICESTICK_MODEL_DOWNLOAD_DIALOG_H_

#include "app_config.h"
#include "model_download_session.h"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace voicestick {

class ModelDownloadDialog {
public:
    ModelDownloadDialog(HINSTANCE instance, HWND owner, UiLanguage language);
    ~ModelDownloadDialog();

    void Show();

    // 下载结束回调（UI 线程）：设置页据此回填 models_dir 并刷新状态回显。
    std::function<void(const ModelDownloadSummary&)> on_complete;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                       LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildUi();
    void DestroyControls();
    void CenterWindow();
    void SetText(HWND control, const std::wstring& text);
    int Dp(int px) const;
    void StartDownload();
    void RunWorker(std::vector<ModelDownloadItem> items);
    void HandleProgress(const ModelSessionProgress& progress);
    void HandleFinished(const ModelDownloadSummary& summary);
    void RequestCancel();

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    UiLanguage language_ = UiLanguage::kEnglish;
    UINT dpi_ = 96;

    HWND intro_label_ = nullptr;
    HWND asr_check_ = nullptr;
    HWND refine_check_ = nullptr;
    HWND disk_label_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND percent_label_ = nullptr;
    HWND status_label_ = nullptr;
    HWND start_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HWND close_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    std::vector<HWND> all_controls_;
    std::vector<BYTE> dialog_template_;

    // 下载状态：worker 线程引用 downloader_/items_，析构前必须 join。
    ModelDownloader downloader_;
    std::vector<ModelDownloadItem> items_;  // 本次会话条目（进度显示当前文件）
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_ =
        std::make_shared<std::atomic<bool>>(false);
    bool downloading_ = false;

    static constexpr int kClientWidth = 460;
    static constexpr int kClientHeight = 330;
    static constexpr UINT kStartId = 101;
    static constexpr UINT kCancelId = 102;
    static constexpr UINT kCloseId = 103;
    static constexpr UINT kMsgProgress = WM_APP + 0x501;
    static constexpr UINT kMsgFinished = WM_APP + 0x502;
};

}  // namespace voicestick

#endif  // VOICESTICK_MODEL_DOWNLOAD_DIALOG_H_
