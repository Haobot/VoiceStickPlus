// selection_correction_dialog.h 的实现：对话框模板与消息处理沿用
// model_download_dialog.cc 的既定模式（动态 DLGTEMPLATE + DPI 重建）。

#include "selection_correction_dialog.h"

#include "dpi_util.h"
#include "localization.h"
#include "log.h"

#include <algorithm>
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

void AppendDialogData(std::vector<BYTE>* buffer, const void* data,
                      std::size_t size) {
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

}  // namespace

void LlmCorrectionCandidatesClient::Request(
    const std::string& wrong_text, const std::string& context,
    const std::vector<std::string>& hotwords,
    std::function<void(bool, std::vector<std::string>)> on_done) const {
    ChatAsync(BuildCandidatesSystemPrompt(),
              BuildCorrectionCandidatesPrompt(wrong_text, context, hotwords),
              [wrong_text, on_done = std::move(on_done)](bool ok,
                                                         std::string reply) {
                  if (!ok) {
                      on_done(false, {});
                      return;
                  }
                  on_done(true, FilterCandidates(
                                    wrong_text, ParseCandidateLines(reply)));
              });
}

SelectionCorrectionDialog::SelectionCorrectionDialog(
    HINSTANCE instance, HWND owner, std::string wrong_text, UiLanguage language,
    CandidatesProvider provider)
    : instance_(instance),
      owner_(owner),
      language_(language),
      wrong_text_(std::move(wrong_text)),
      provider_(std::move(provider)) {}

SelectionCorrectionDialog::~SelectionCorrectionDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) DeleteObject(ui_font_);
}

void SelectionCorrectionDialog::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }
    hwnd_ = CreateDialogIndirectParamW(instance_, BuildDialogTemplate(), owner_,
                                       SelectionCorrectionDialog::DialogProc,
                                       reinterpret_cast<LPARAM>(this));
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    if (provider_) {
        // 候选生成后台发起；窗口先开，候选区显示加载态。
        provider_([this](bool ok, std::vector<std::string> candidates) {
            if (!hwnd_) return;  // 窗口已关：结果丢弃
            auto* payload =
                new std::pair<bool, std::vector<std::string>>(ok,
                                                              std::move(candidates));
            if (!PostMessageW(hwnd_, kMsgCandidates, 0,
                              reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        });
    }
}

INT_PTR CALLBACK SelectionCorrectionDialog::DialogProc(HWND hwnd, UINT message,
                                                        WPARAM w_param,
                                                        LPARAM l_param) {
    auto* dialog = reinterpret_cast<SelectionCorrectionDialog*>(
        GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<SelectionCorrectionDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildUi();
        dialog->CenterWindow();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR SelectionCorrectionDialog::HandleMessage(UINT message, WPARAM w_param,
                                                 LPARAM l_param) {
    switch (message) {
    case WM_COMMAND: {
        const UINT id = LOWORD(w_param);
        const UINT code = HIWORD(w_param);
        if (id >= kCandidateBaseId &&
            id < kCandidateBaseId + candidates_.size() &&
            code == BN_CLICKED) {
            ConfirmWord(candidates_[id - kCandidateBaseId]);
            return TRUE;
        }
        if (id == kOkId && code == BN_CLICKED) {
            wchar_t buffer[128] = {};
            // 手输上限与热词一致（64 UTF-8 字节 ≈ 21 汉字，纠错词足够）
            const int len =
                manual_edit_ ? GetWindowTextW(manual_edit_, buffer, 128) : 0;
            std::string word;
            if (len > 0) {
                word = LLMChatClient::Utf8FromUtf16(
                    std::wstring(buffer, static_cast<std::size_t>(len)));
            }
            // 空输入不响应（候选可点选，不强制手输）
            if (!word.empty()) ConfirmWord(word);
            return TRUE;
        }
        if (id == kCancelId && code == BN_CLICKED) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return TRUE;
        }
        break;
    }
    case kMsgCandidates: {
        auto* payload = reinterpret_cast<std::pair<bool, std::vector<std::string>>*>(
            l_param);
        if (payload) {
            HandleCandidates(payload->first, std::move(payload->second));
            delete payload;
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
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

void SelectionCorrectionDialog::HandleCandidates(
    bool ok, std::vector<std::string> candidates) {
    candidates_loaded_ = true;
    candidates_ = std::move(candidates);
    if (!ok) {
        LogApp("Selection correction candidates: engine failed");
    }
    RebuildCandidateButtons();
}

void SelectionCorrectionDialog::ConfirmWord(const std::string& word) {
    if (on_confirm) on_confirm(word);
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LPCDLGTEMPLATE SelectionCorrectionDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog{};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog.cdit = 0;
    dialog.x = 0;
    dialog.y = 0;
    dialog.cx = 200;
    dialog.cy = 200;
    AppendDialogData(&dialog_template_, &dialog, sizeof(dialog));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_,
                           TrW(StringId::kSelectionCorrectionTitle, language_)
                               .c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void SelectionCorrectionDialog::BuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    auto remember = [&](HWND control) {
        if (control) {
            all_controls_.push_back(control);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                         TRUE);
        }
        return control;
    };
    const int width = kClientWidth - Dp(48);

    // 错词行：标签前缀 + 错词本体重点展示。
    const std::wstring wrong_line =
        TrW(StringId::kSelectionCorrectionWrongLabel, language_) +
        Utf16(wrong_text_);
    wrong_label_ = remember(CreateWindowExW(
        0, L"STATIC", wrong_line.c_str(), WS_CHILD | WS_VISIBLE, Dp(24), Dp(14),
        width, Dp(22), hwnd_, nullptr, instance_, nullptr));

    candidates_label_ = remember(CreateWindowExW(
        0, L"STATIC",
        TrW(StringId::kSelectionCorrectionCandidatesLabel, language_).c_str(),
        WS_CHILD | WS_VISIBLE, Dp(24), Dp(44), width, Dp(20), hwnd_, nullptr,
        instance_, nullptr));

    // 候选按钮区（RebuildCandidateButtons 按加载结果重建）。
    status_label_ = remember(CreateWindowExW(
        0, L"STATIC",
        TrW(StringId::kSelectionCorrectionLoading, language_).c_str(),
        WS_CHILD | WS_VISIBLE, Dp(36), Dp(70), width, Dp(20), hwnd_, nullptr,
        instance_, nullptr));
    RebuildCandidateButtons();

    manual_label_ = remember(CreateWindowExW(
        0, L"STATIC",
        TrW(StringId::kSelectionCorrectionManualLabel, language_).c_str(),
        WS_CHILD | WS_VISIBLE, Dp(24), Dp(228), width, Dp(20), hwnd_, nullptr,
        instance_, nullptr));
    manual_edit_ = remember(CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, Dp(24), Dp(250),
        width, Dp(24), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance_,
        nullptr));
    ok_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kOk, language_).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, Dp(24), Dp(282),
        Dp(90), Dp(28), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), instance_,
        nullptr));
    cancel_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kCancel, language_).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, Dp(126), Dp(282),
        Dp(90), Dp(28), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), instance_,
        nullptr));

    if (manual_edit_) SetFocus(manual_edit_);
}

void SelectionCorrectionDialog::RebuildCandidateButtons() {
    // 只重建候选按钮子集（编辑框内容/焦点不受影响）。
    for (HWND button : candidate_buttons_) {
        if (button && IsWindow(button)) DestroyWindow(button);
        all_controls_.erase(
            std::remove(all_controls_.begin(), all_controls_.end(), button),
            all_controls_.end());
    }
    candidate_buttons_.clear();

    const std::wstring status = candidates_loaded_
                                    ? (candidates_.empty()
                                           ? TrW(StringId::
                                                     kSelectionCorrectionNoCandidates,
                                                 language_)
                                           : std::wstring())
                                    : TrW(StringId::kSelectionCorrectionLoading,
                                          language_);
    if (status_label_) SetWindowTextW(status_label_, status.c_str());

    int y = 70;
    for (std::size_t i = 0; i < candidates_.size() && i < 5; ++i) {
        HWND button = CreateWindowExW(
            0, L"BUTTON", Utf16(candidates_[i]).c_str(),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, Dp(36), Dp(y + 22),
            Dp(kClientWidth - 72), Dp(26), hwnd_,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kCandidateBaseId + i)),
            instance_, nullptr);
        if (button) {
            candidate_buttons_.push_back(button);
            all_controls_.push_back(button);
            if (ui_font_) {
                SendMessageW(button, WM_SETFONT,
                             reinterpret_cast<WPARAM>(ui_font_), TRUE);
            }
        }
        y += 30;
    }
}

void SelectionCorrectionDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    candidate_buttons_.clear();
    wrong_label_ = nullptr;
    candidates_label_ = nullptr;
    status_label_ = nullptr;
    manual_label_ = nullptr;
    manual_edit_ = nullptr;
    ok_button_ = nullptr;
    cancel_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void SelectionCorrectionDialog::CenterWindow() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    RECT work_area = GetWorkAreaForWindow(hwnd_);
    const int x =
        work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
    const int y =
        work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

int SelectionCorrectionDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

}  // namespace voicestick
