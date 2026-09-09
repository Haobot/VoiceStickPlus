// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// model_download_dialog.h 的实现。对话框模板与消息处理沿用
// firmware_update_dialog.cc 的既定模式（动态 DLGTEMPLATE + DPI 重建）。

#include "model_download_dialog.h"

#include "dpi_util.h"
#include "localization.h"
#include "log.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <utility>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    while (text && *text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

// 字节数的人话显示（与设置页体积口径一致：240 MB / 1.1 GB）。
std::wstring FormatSizeBytes(std::uint64_t bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1.0) {
        wchar_t text[32] = {};
        swprintf(text, 32, L"%.1f GB", gib);
        return text;
    }
    wchar_t text[32] = {};
    swprintf(text, 32, L"%llu MB",
             static_cast<unsigned long long>(bytes / (1024 * 1024)));
    return text;
}

std::wstring FormatText(std::wstring text, const std::wstring& value) {
    const auto pos = text.find(L"%s");
    if (pos == std::wstring::npos) return text;
    text.replace(pos, 2, value);
    return text;
}

}  // namespace

ModelDownloadDialog::ModelDownloadDialog(HINSTANCE instance, HWND owner,
                                         UiLanguage language)
    : instance_(instance), owner_(owner), language_(language) {}

ModelDownloadDialog::~ModelDownloadDialog() {
    if (worker_.joinable()) {
        cancel_flag_->store(true);
        worker_.join();
    }
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) DeleteObject(ui_font_);
}

void ModelDownloadDialog::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    hwnd_ = CreateDialogIndirectParamW(instance_, BuildDialogTemplate(), owner_,
                                       ModelDownloadDialog::DialogProc,
                                       reinterpret_cast<LPARAM>(this));
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void ModelDownloadDialog::StartDownload() {
    const bool include_refine =
        refine_check_ && SendMessageW(refine_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto models_dir = LocalModelCacheModelsDir();
    std::vector<ModelDownloadItem> items = BuildModelDownloadItems(models_dir, true);
    // 已在位（大小相符）的文件跳过：重下既浪费带宽也无意义，完整性由设置页
    // 状态检查与加载校验把关。
    std::error_code ec;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const ModelDownloadItem& item) {
                                   if (std::filesystem::exists(item.dest, ec) &&
                                       std::filesystem::file_size(item.dest, ec) ==
                                           item.spec.bytes) {
                                       Log("MDL", "already present, skip: " +
                                                      item.spec.rel_path);
                                       return true;
                                   }
                                   return false;
                               }),
                items.end());
    if (!include_refine) {
        for (auto& item : items) {
            if (item.kind == ModelKind::kRefine) item.selected = false;
        }
    }
    if (items.empty()) {
        // 全部已在位：直接按成功收尾（含 on_complete 回填）。
        ModelDownloadSummary summary;
        summary.asr_ok = true;
        summary.refine_skipped = !include_refine;
        summary.refine_ok = include_refine;
        HandleFinished(summary);
        return;
    }

    items_ = items;
    downloading_ = true;
    cancel_flag_->store(false);
    EnableWindow(start_button_, FALSE);
    EnableWindow(asr_check_, FALSE);
    EnableWindow(refine_check_, FALSE);
    ShowWindow(cancel_button_, SW_SHOW);
    EnableWindow(cancel_button_, TRUE);
    SetText(status_label_, TrW(StringId::kModelDownloadRunning, language_));
    worker_ = std::thread([this, items = std::move(items)]() mutable {
        RunWorker(std::move(items));
    });
}

void ModelDownloadDialog::RunWorker(std::vector<ModelDownloadItem> items) {
    ModelDownloadSession session(std::move(items), &downloader_,
                                 [this](const ModelSessionProgress& progress) {
                                     auto* copy = new ModelSessionProgress(progress);
                                     if (!PostMessageW(hwnd_, kMsgProgress, 0,
                                                       reinterpret_cast<LPARAM>(copy))) {
                                         delete copy;
                                     }
                                 },
                                 cancel_flag_);
    auto* summary = new ModelDownloadSummary(session.Run());
    if (!PostMessageW(hwnd_, kMsgFinished, 0, reinterpret_cast<LPARAM>(summary))) {
        delete summary;
    }
}

void ModelDownloadDialog::HandleProgress(const ModelSessionProgress& progress) {
    if (progress.total > 0) {
        const int percent = std::clamp(
            static_cast<int>(progress.downloaded * 100 / progress.total), 0, 100);
        SendMessageW(progress_bar_, PBM_SETPOS, percent, 0);
        SetText(percent_label_, std::to_wstring(percent) + L"%");
    }
    if (progress.item_index < items_.size()) {
        SetText(status_label_,
                TrW(StringId::kModelDownloadRunning, language_) + L"\n" +
                    Utf16(items_[progress.item_index].spec.rel_path));
    }
}

void ModelDownloadDialog::HandleFinished(const ModelDownloadSummary& summary) {
    downloading_ = false;
    EnableWindow(cancel_button_, FALSE);
    ShowWindow(cancel_button_, SW_HIDE);
    ShowWindow(close_button_, SW_SHOW);
    EnableWindow(close_button_, TRUE);

    if (summary.cancelled) {
        SetText(status_label_, TrW(StringId::kModelDownloadCancelled, language_));
    } else if (summary.asr_ok && summary.refine_ok) {
        SendMessageW(progress_bar_, PBM_SETPOS, 100, 0);
        SetText(percent_label_, L"100%");
        SetText(status_label_, TrW(StringId::kModelDownloadDone, language_));
    } else if (summary.asr_ok) {
        SendMessageW(progress_bar_, PBM_SETPOS, 100, 0);
        SetText(percent_label_, L"100%");
        std::wstring text = TrW(StringId::kModelDownloadDoneRefineMissing, language_);
        for (const auto& error : summary.errors) {
            text += L"\n" + Utf16(error);
        }
        SetText(status_label_, text);
    } else {
        std::wstring text = TrW(StringId::kModelDownloadFailed, language_);
        for (const auto& error : summary.errors) {
            text += L"\n" + Utf16(error);
        }
        SetText(status_label_, text);
    }
    if (on_complete) on_complete(summary);
}

void ModelDownloadDialog::RequestCancel() {
    if (downloading_) {
        cancel_flag_->store(true);
        EnableWindow(cancel_button_, FALSE);
    }
}

INT_PTR CALLBACK ModelDownloadDialog::DialogProc(HWND hwnd, UINT message,
                                                 WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<ModelDownloadDialog*>(
        GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<ModelDownloadDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildUi();
        dialog->CenterWindow();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR ModelDownloadDialog::HandleMessage(UINT message, WPARAM w_param,
                                           LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(w_param) == kStartId) {
            StartDownload();
            return TRUE;
        }
        if (LOWORD(w_param) == kCancelId) {
            RequestCancel();
            return TRUE;
        }
        if (LOWORD(w_param) == kCloseId) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return TRUE;
        }
        break;
    case kMsgProgress: {
        auto* progress = reinterpret_cast<ModelSessionProgress*>(l_param);
        if (progress) {
            HandleProgress(*progress);
            delete progress;
        }
        return TRUE;
    }
    case kMsgFinished: {
        auto* summary = reinterpret_cast<ModelDownloadSummary*>(l_param);
        if (summary) {
            HandleFinished(*summary);
            delete summary;
        }
        return TRUE;
    }
    case WM_CLOSE:
        // 下载中先请求取消，待 worker 收尾（kMsgFinished）后再关；空闲直接关。
        if (downloading_) {
            RequestCancel();
        } else {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        return TRUE;
    case WM_DPICHANGED: {
        const UINT new_dpi = HIWORD(w_param);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            const auto* rect = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildUi();
        }
        return TRUE;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        DestroyControls();
        return TRUE;
    }
    return FALSE;
}

LPCDLGTEMPLATE ModelDownloadDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog{};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog.cdit = 0;
    dialog.x = 0;
    dialog.y = 0;
    dialog.cx = 260;
    dialog.cy = 170;
    AppendDialogData(&dialog_template_, &dialog, sizeof(dialog));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_,
                           TrW(StringId::kModelDownloadTitle, language_).c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void ModelDownloadDialog::BuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    auto remember = [&](HWND control) {
        if (control) {
            all_controls_.push_back(control);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return control;
    };
    const int width = kClientWidth - Dp(48);
    auto label = [&](StringId id, int y, int h, DWORD extra = 0) {
        return remember(CreateWindowExW(
            0, L"STATIC", TrW(id, language_).c_str(),
            WS_CHILD | WS_VISIBLE | extra, Dp(24), Dp(y), width, Dp(h),
            hwnd_, nullptr, instance_, nullptr));
    };

    intro_label_ = label(StringId::kModelDownloadIntro, 16, 40);
    asr_check_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kModelDownloadAsrItem, language_).c_str(),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, Dp(24), Dp(62), width, Dp(22),
        hwnd_, nullptr, instance_, nullptr));
    SendMessageW(asr_check_, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(asr_check_, FALSE);  // 必选条目锁定
    refine_check_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kModelDownloadRefineItem, language_).c_str(),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, Dp(24), Dp(88), width, Dp(22),
        hwnd_, nullptr, instance_, nullptr));
    SendMessageW(refine_check_, BM_SETCHECK, BST_CHECKED, 0);
    {
        const auto& entries = BundledModelEntries();
        std::uint64_t total = 0;
        for (const auto& entry : entries) {
            for (const auto& file : entry.files) total += file.bytes;
        }
        disk_label_ = remember(CreateWindowExW(
            0, L"STATIC",
            FormatText(TrW(StringId::kModelDownloadDiskSpace, language_),
                       FormatSizeBytes(total))
                .c_str(),
            WS_CHILD | WS_VISIBLE, Dp(24), Dp(116), width, Dp(20),
            hwnd_, nullptr, instance_, nullptr));
    }
    progress_bar_ = remember(CreateWindowExW(
        0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, Dp(24), Dp(148), Dp(320),
        Dp(20), hwnd_, nullptr, instance_, nullptr));
    SendMessageW(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    percent_label_ = remember(CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT, Dp(352), Dp(148),
        Dp(84), Dp(20), hwnd_, nullptr, instance_, nullptr));
    status_label_ = label(StringId::kModelDownloadIntro, 178, 80);
    SetText(status_label_, L"");

    start_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kModelDownloadStart, language_).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, Dp(24), Dp(280), Dp(120), Dp(28),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartId)), instance_,
        nullptr));
    cancel_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kCancel, language_).c_str(),
        WS_CHILD | BS_PUSHBUTTON, Dp(160), Dp(280), Dp(90), Dp(28),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), instance_,
        nullptr));  // 初始隐藏，开始下载后显示
    close_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kClose, language_).c_str(),
        WS_CHILD | BS_PUSHBUTTON, Dp(360), Dp(280), Dp(76), Dp(28),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), instance_,
        nullptr));  // 初始隐藏，结束后显示
}

void ModelDownloadDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    intro_label_ = nullptr;
    asr_check_ = nullptr;
    refine_check_ = nullptr;
    disk_label_ = nullptr;
    progress_bar_ = nullptr;
    percent_label_ = nullptr;
    status_label_ = nullptr;
    start_button_ = nullptr;
    cancel_button_ = nullptr;
    close_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void ModelDownloadDialog::CenterWindow() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    RECT work_area = GetWorkAreaForWindow(hwnd_);
    const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void ModelDownloadDialog::SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

int ModelDownloadDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

}  // namespace voicestick
