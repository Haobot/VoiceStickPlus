#include "correction_dialog.h"

#include "hotword_suggest_client.h"
#include "llm_chat_client.h"
#include "localization.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

namespace voicestick {

namespace {

// AI 建议异步结果负载：后台线程 new，PostMessage 携带指针回 UI 线程，处理后 delete。
struct SuggestPayload {
    std::uint32_t seq = 0;  // 发起请求时的序号，回 UI 线程后校验是否仍是最新请求
    bool ok = false;
    std::vector<std::string> words;
};

} // namespace

void CorrectionDialog::RegisterWindowClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &CorrectionDialog::DialogWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    registered = true;
}

CorrectionDialog::CorrectionDialog(HINSTANCE instance, HWND parent)
    : instance_(instance), parent_(parent) {
    RegisterWindowClass(instance_);
    // WS_POPUP|WS_CAPTION|WS_SYSMENU：带标题栏与关闭按钮的固定尺寸弹出窗口，
    // 不进任务栏（无 WS_MINIMIZEBOX，toolwindow 不用，保持普通标题栏外观）。
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST, kWindowClass, L"", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 0, 0, parent_, nullptr, instance_, this);
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        BuildControls();
        ApplyLanguage();
    }
}

CorrectionDialog::~CorrectionDialog() {
    if (suggest_timer_id_ && hwnd_) KillTimer(hwnd_, kSuggestTimerId);
    if (hwnd_) DestroyWindow(hwnd_);
}

int CorrectionDialog::Dp(int px) const {
    UINT dpi = 96;
    if (parent_) dpi = GetDpiForWindow(parent_);
    return static_cast<int>(px * static_cast<INT64>(dpi) / 96);
}

void CorrectionDialog::BuildControls() {
    const auto make_label = [&](HWND* out, int x, int y, int w, int h) {
        *out = CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               Dp(x), Dp(y), Dp(w), Dp(h), hwnd_, nullptr,
                               instance_, nullptr);
    };
    make_label(&wrong_label_, 16, 16, 68, 20);
    wrong_value_label_ = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
        Dp(92), Dp(16), Dp(kClientWidthDp - 92 - 16), Dp(20), hwnd_, nullptr,
        instance_, nullptr);
    make_label(&right_label_, 16, 52, 68, 20);
    right_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        Dp(92), Dp(48), Dp(kClientWidthDp - 92 - 16), Dp(26), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRightEdit)),
        instance_, nullptr);
    make_label(&suggest_label_, 16, 92, kClientWidthDp - 32, 20);
    suggest_list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        Dp(16), Dp(114), Dp(kClientWidthDp - 32), Dp(128), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSuggestList)),
        instance_, nullptr);
    save_button_ = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        Dp(kClientWidthDp - 32 - 200), Dp(kClientHeightDp - 40), Dp(96), Dp(28),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSave)),
        instance_, nullptr);
    cancel_button_ = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        Dp(kClientWidthDp - 32 - 96), Dp(kClientHeightDp - 40), Dp(96), Dp(28),
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)),
        instance_, nullptr);
    // 统一 GUI 字体（控件默认是 System 字体，过老）。
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void CorrectionDialog::ApplyLanguage() {
    if (!hwnd_) return;
    SetWindowTextW(hwnd_, TrW(StringId::kCorrectionDialogTitle, language_).c_str());
    SetWindowTextW(wrong_label_,
                   TrW(StringId::kCorrectionWrongLabel, language_).c_str());
    SetWindowTextW(right_label_,
                   TrW(StringId::kCorrectionRightLabel, language_).c_str());
    SetWindowTextW(suggest_label_,
                   TrW(StringId::kCorrectionSuggestLabel, language_).c_str());
    SetWindowTextW(save_button_,
                   TrW(StringId::kCorrectionSaveButton, language_).c_str());
    SetWindowTextW(cancel_button_, TrW(StringId::kCancel, language_).c_str());
}

LRESULT CALLBACK CorrectionDialog::DialogWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                                  LPARAM lp) {
    auto* self = reinterpret_cast<CorrectionDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CorrectionDialog::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            if (wp == kSuggestTimerId) {
                KillTimer(hwnd_, kSuggestTimerId);
                suggest_timer_id_ = 0;
                OnSuggestTimer();
                return 0;
            }
            break;
        }
        case kWmSuggestReady: {
            OnSuggestReady(wp, lp);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == kIdRightEdit && code == EN_CHANGE) {
                // 输入变化：重置防抖定时器，停顿 600ms 后请求 AI 建议。
                if (suggest_timer_id_) KillTimer(hwnd_, kSuggestTimerId);
                suggest_timer_id_ = SetTimer(hwnd_, kSuggestTimerId,
                                             kSuggestDebounceMs, nullptr);
                return 0;
            }
            if (id == kIdSuggestList && code == LBN_SELCHANGE) {
                // 点击建议词汇即可保存。
                const int sel = static_cast<int>(
                    SendMessageW(suggest_list_, LB_GETCURSEL, 0, 0));
                if (sel != LB_ERR) {
                    const int len = static_cast<int>(
                        SendMessageW(suggest_list_, LB_GETTEXTLEN, sel, 0));
                    if (len != LB_ERR && len >= 0) {
                        std::wstring wide(static_cast<std::size_t>(len) + 1, L'\0');
                        if (SendMessageW(suggest_list_, LB_GETTEXT, sel,
                                         reinterpret_cast<LPARAM>(wide.data())) != LB_ERR) {
                            SaveRight(LLMChatClient::Utf8FromUtf16(wide));
                            return 0;
                        }
                    }
                }
                return 0;
            }
            if (id == kIdSave && code == BN_CLICKED) {
                SaveFromEdit();
                return 0;
            }
            if (id == kIdCancel && code == BN_CLICKED) {
                Hide();
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            Hide();
            return 0;
        }
        case WM_DESTROY: {
            // 进程退出销毁：清 GWLP_USERDATA 防 WndProc 回溯悬垂 this。
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void CorrectionDialog::Show(const std::string& wrong_text, POINT near_pt,
                            HWND target_hwnd, const AppConfig& config) {
    if (!hwnd_) return;
    wrong_text_ = wrong_text;
    target_hwnd_ = target_hwnd;
    config_ = config;
    ++suggest_request_seq_;  // 使在途旧建议响应失效
    if (suggest_timer_id_) {
        KillTimer(hwnd_, kSuggestTimerId);
        suggest_timer_id_ = 0;
    }

    ApplyLanguage();
    SetWindowTextW(wrong_value_label_,
                   LLMChatClient::Utf16FromUtf8(wrong_text_).c_str());
    SetWindowTextW(right_edit_, L"");
    SendMessageW(suggest_list_, LB_RESETCONTENT, 0, 0);

    // 定位：锚点右下角，屏幕工作区边界钳制。
    const int w = Dp(kClientWidthDp) + GetSystemMetrics(SM_CXFIXEDFRAME) * 2;
    const int h = Dp(kClientHeightDp) + GetSystemMetrics(SM_CYCAPTION) +
                  GetSystemMetrics(SM_CYFIXEDFRAME) * 2;
    int x = near_pt.x + 12;
    int y = near_pt.y + 20;
    HMONITOR mon = MonitorFromPoint(near_pt, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            if (x + w > mi.rcWork.right) x = near_pt.x - 12 - w;
            if (y + h > mi.rcWork.bottom) y = near_pt.y - 20 - h;
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
        }
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(right_edit_);
}

void CorrectionDialog::Hide() {
    if (suggest_timer_id_) {
        KillTimer(hwnd_, kSuggestTimerId);
        suggest_timer_id_ = 0;
    }
    ++suggest_request_seq_;  // 使在途建议响应失效
    ShowWindow(hwnd_, SW_HIDE);
}

void CorrectionDialog::OnSuggestTimer() {
    suggest_timer_id_ = 0;
    RequestSuggestions();
}

void CorrectionDialog::RequestSuggestions() {
    wchar_t buffer[512] = {};
    const int len = GetWindowTextW(right_edit_, buffer, 512);
    const std::string input =
        LLMChatClient::Utf8FromUtf16(std::wstring(buffer, len));
    SendMessageW(suggest_list_, LB_RESETCONTENT, 0, 0);
    if (input.find_first_not_of(" \t\r\n") == std::string::npos) return;  // 空输入：清空列表

    // 未配置 LLM key：列表给出提示行（不可保存的占位说明）。
    if (config_.llm_api_key.find_first_not_of(" \t\r\n") == std::string::npos) {
        SendMessageW(suggest_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         TrW(StringId::kCorrectionNoKey, language_).c_str()));
        return;
    }
    SendMessageW(suggest_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
                     TrW(StringId::kCorrectionSuggestLoading, language_).c_str()));

    const std::uint32_t seq = ++suggest_request_seq_;
    // ChatAsync 内部拷贝配置并 detached 线程执行，栈上临时对象安全；
    // 后台线程解析响应后堆分配 payload，PostMessage 回 UI 线程。
    HotwordSuggestClient(config_).Suggest(
        wrong_text_, input,
        [hwnd = hwnd_, seq](bool ok, std::string response) {
            auto* payload = new SuggestPayload{};
            payload->seq = seq;
            payload->ok = ok;
            if (ok) {
                payload->words = HotwordSuggestClient::ParseSuggestResponse(response);
            }
            PostMessage(hwnd, kWmSuggestReady, ok ? 1 : 0,
                        reinterpret_cast<LPARAM>(payload));
        });
}

void CorrectionDialog::OnSuggestReady(WPARAM wp, LPARAM lp) {
    auto* payload = reinterpret_cast<SuggestPayload*>(lp);
    std::unique_ptr<SuggestPayload> owned(payload);
    // 过期响应（用户已继续输入或关闭对话框）：丢弃。
    if (payload->seq != suggest_request_seq_) return;

    SendMessageW(suggest_list_, LB_RESETCONTENT, 0, 0);
    if (!payload->ok) {
        SendMessageW(suggest_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
                         TrW(StringId::kCorrectionSuggestFailed, language_).c_str()));
        return;
    }
    if (payload->words.empty()) {
        SendMessageW(suggest_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
                         TrW(StringId::kCorrectionSuggestNone, language_).c_str()));
        return;
    }
    FillSuggestionList(payload->words);
}

void CorrectionDialog::FillSuggestionList(const std::vector<std::string>& suggestions) {
    SendMessageW(suggest_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& word : suggestions) {
        SendMessageW(suggest_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
                         LLMChatClient::Utf16FromUtf8(word).c_str()));
    }
}

void CorrectionDialog::SaveFromEdit() {
    wchar_t buffer[512] = {};
    const int len = GetWindowTextW(right_edit_, buffer, 512);
    std::string right = LLMChatClient::Utf8FromUtf16(std::wstring(buffer, len));
    // Trim 语义判空。
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    right.erase(right.begin(), std::find_if_not(right.begin(), right.end(), is_space));
    right.erase(std::find_if_not(right.rbegin(), right.rend(), is_space).base(),
                right.end());
    if (right.empty()) {
        // 通知由 Win32App 统一处理；这里只回填提示行。
        SendMessageW(suggest_list_, LB_RESETCONTENT, 0, 0);
        SendMessageW(suggest_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
                         TrW(StringId::kCorrectionEmptyBody, language_).c_str()));
        return;
    }
    SaveRight(right);
}

void CorrectionDialog::SaveRight(const std::string& right) {
    Hide();
    if (on_save) on_save(wrong_text_, right, target_hwnd_);
}

} // namespace voicestick
