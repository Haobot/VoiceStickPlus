#include "selection_hotword_manager.h"

#include "localization.h"
#include "log.h"

#include <Windows.h>
#include <windowsx.h>
// WIN32_LEAN_AND_MEAN（voicestick_core PUBLIC 透传）会从 Windows.h 排除 ole2.h，
// 而 <uiautomation.h> 依赖 COM 接口定义，必须显式包含 ole2.h 在前。
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#pragma comment(lib, "uiautomationcore.lib")

namespace voicestick {

namespace {

// 静态实例指针：低级鼠标钩子回调是 C 函数，无 user data 通道，靠此路由到 manager。
// 仅在 UI 线程访问（钩子安装/卸载与回调分发均在 UI 线程），无需原子化。
SelectionHotwordManager* g_active_manager = nullptr;

constexpr UINT kWmMouseLeftUp = WM_APP + 210;     // 钩子→helper：WM_LBUTTONUP 通知
constexpr UINT kWmPopupClicked = WM_APP + 211;    // popup→helper：按钮被点击

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string NormalizeHotword(std::string text) {
    // 1) 把所有空白（含 \r\n\t）统一为单空格
    std::replace_if(text.begin(), text.end(),
                    [](unsigned char c) { return std::isspace(c) != 0; }, ' ');
    // 2) 压缩连续空格
    auto end = std::unique(text.begin(), text.end(),
                           [](char a, char b) { return a == ' ' && b == ' '; });
    text.erase(end, text.end());
    // 3) trim
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), is_space));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), is_space).base(), text.end());
    return text;
}

} // namespace

SelectionHotwordManager::SelectionHotwordManager(HINSTANCE instance, HWND parent)
    : instance_(instance), parent_(parent) {
    EnsureHelperWindow();
    EnsurePopupWindow();
}

SelectionHotwordManager::~SelectionHotwordManager() {
    SetEnabled(false);
    if (popup_hwnd_) DestroyWindow(popup_hwnd_);
    if (helper_hwnd_) DestroyWindow(helper_hwnd_);
}

int SelectionHotwordManager::Dp(int px) const {
    UINT dpi = 96;
    if (parent_) dpi = GetDpiForWindow(parent_);
    return static_cast<int>(px * static_cast<INT64>(dpi) / 96);
}

void SelectionHotwordManager::EnsureHelperWindow() {
    if (helper_hwnd_) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &SelectionHotwordManager::HelperWndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kHelperWindowClass;
    RegisterClassW(&wc);
    // HWND_MESSAGE：消息-only 窗口，不可见、不在任务栏，仅用于承载定时器与跨线程投递。
    helper_hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, instance_, this);
    if (helper_hwnd_) SetWindowLongPtrW(helper_hwnd_, GWLP_USERDATA,
                                        reinterpret_cast<LONG_PTR>(this));
}

void SelectionHotwordManager::RegisterPopupClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &SelectionHotwordManager::PopupWndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kPopupWindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;  // 自绘
    RegisterClassW(&wc);
}

void SelectionHotwordManager::EnsurePopupWindow() {
    if (popup_hwnd_) return;
    RegisterPopupClass();
    // WS_EX_NOACTIVATE：点击不抢焦点，保持原应用焦点（选区不丢失）。
    // WS_EX_TOOLWINDOW：不在任务栏/Alt+Tab 显示。
    // WS_EX_TOPMOST：浮在最前。
    // WS_EX_LAYERED：分层窗口，配合 SetLayeredWindowAttributes 做常量 alpha。
    popup_hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kPopupWindowClass, L"", WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, instance_, this);
    if (popup_hwnd_) {
        SetWindowLongPtrW(popup_hwnd_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        // 255 不透明；如需半透明可调小。color key 不用，避免边缘锯齿。
        SetLayeredWindowAttributes(popup_hwnd_, 0, 255, LWA_ALPHA);
    }
}

LRESULT CALLBACK SelectionHotwordManager::HelperWndProc(HWND hwnd, UINT msg,
                                                         WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SelectionHotwordManager*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
            case WM_TIMER: {
                if (wp == kQueryTimerId) {
                    KillTimer(hwnd, kQueryTimerId);
                    self->OnQueryTimer();
                    return 0;
                }
                if (wp == kAutoHideTimerId) {
                    KillTimer(hwnd, kAutoHideTimerId);
                    self->OnAutoHideTimer();
                    return 0;
                }
                break;
            }
            case kWmMouseLeftUp: {
                POINT pt{};
                pt.x = static_cast<LONG>(wp);
                pt.y = static_cast<LONG>(lp);
                self->OnMouseLeftUp(pt);
                return 0;
            }
            case kWmPopupClicked: {
                self->HidePopup();
                std::string text = self->pending_text_;
                if (self->on_add_hotword) self->on_add_hotword(text);
                return 0;
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SelectionHotwordManager::PopupWndProc(HWND hwnd, UINT msg,
                                                        WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SelectionHotwordManager*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
            case WM_LBUTTONUP: {
                // 点击弹窗任意位置即触发"添加到热词"。投递到 helper 处理，
                // 避免在弹窗 WndProc 内回调时弹窗仍可见导致重入。
                PostMessage(self->helper_hwnd_, kWmPopupClicked, 0, 0);
                return 0;
            }
            case WM_MOUSEMOVE: {
                // 简单 hover 反馈：光标已由窗口类设为 IDC_HAND，无需额外处理。
                break;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);

                // 背景：浅色；边框：1px 深灰。
                HBRUSH bg = CreateSolidBrush(RGB(250, 250, 252));
                FillRect(ps.hdc, &rc, bg);
                DeleteObject(bg);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 205, 215));
                HGDIOBJ old_pen = SelectObject(ps.hdc, pen);
                HBRUSH null_brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
                HGDIOBJ old_brush = SelectObject(ps.hdc, null_brush);
                Rectangle(ps.hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
                SelectObject(ps.hdc, old_pen);
                SelectObject(ps.hdc, old_brush);
                DeleteObject(pen);

                // 文本：居中。
                const auto button_text = TrW(StringId::kSelectionHotwordButton,
                                             self->language_);
                HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                HGDIOBJ old_font = SelectObject(ps.hdc, font);
                SetBkMode(ps.hdc, TRANSPARENT);
                SetTextColor(ps.hdc, RGB(40, 45, 55));
                RECT text_rc = rc;
                text_rc.left += self->Dp(8);
                text_rc.right -= self->Dp(8);
                DrawTextW(ps.hdc, button_text.c_str(), -1, &text_rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(ps.hdc, old_font);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                // 自绘背景，阻止默认擦除避免闪烁。
                return 1;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SelectionHotwordManager::MouseHookProc(int code,
                                                         WPARAM w_param,
                                                         LPARAM l_param) {
    if (code == HC_ACTION && w_param == WM_LBUTTONUP && g_active_manager) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(l_param);
        // 投递到 helper 窗口在 UI 线程异步处理，避免阻塞鼠标系统。
        // LowLevelHooksTimeout 默认 ~300ms，超时 Windows 会摘钩子。
        PostMessage(g_active_manager->helper_hwnd_, kWmMouseLeftUp,
                    static_cast<WPARAM>(info->pt.x), static_cast<LPARAM>(info->pt.y));
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

void SelectionHotwordManager::SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    if (enabled) {
        EnsureHelperWindow();
        if (!mouse_hook_) {
            g_active_manager = this;
            mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance_, 0);
            if (!mouse_hook_) {
                g_active_manager = nullptr;
                LogApp("SelectionHotwordManager: SetWindowsHookEx WH_MOUSE_LL failed err="
                        + std::to_string(GetLastError()));
            } else {
                LogApp("SelectionHotwordManager: mouse hook installed");
            }
        }
    } else {
        HidePopup();
        if (mouse_hook_) {
            UnhookWindowsHookEx(mouse_hook_);
            mouse_hook_ = nullptr;
            if (g_active_manager == this) g_active_manager = nullptr;
            LogApp("SelectionHotwordManager: mouse hook removed");
        }
    }
    enabled_ = enabled;
}

void SelectionHotwordManager::OnMouseLeftUp(POINT pt) {
    if (!enabled_) return;
    // 若点击落在当前弹窗上（用户在弹窗上释放鼠标），不触发新查询，让弹窗处理点击。
    if (popup_visible_ && IsPointOnPopup(pt)) return;
    // 若弹窗正显示且用户在弹窗外点击，先隐藏旧弹窗。
    if (popup_visible_) HidePopup();
    pending_cursor_ = pt;
    // 启动短延时查询：等 UIA 选区稳定，并避免在鼠标消息路径上做 COM 调用。
    if (query_timer_id_) KillTimer(helper_hwnd_, kQueryTimerId);
    query_timer_id_ = SetTimer(helper_hwnd_, kQueryTimerId, kQueryDelayMs, nullptr);
}

void SelectionHotwordManager::OnQueryTimer() {
    query_timer_id_ = 0;
    if (!enabled_) return;
    std::string raw = GetSelectedTextViaUia();
    std::string text = NormalizeHotword(std::move(raw));
    if (text.empty()) return;          // 无选区或纯空白：静默
    if (text.size() > static_cast<std::size_t>(max_length_)) return;  // 选区过长：静默忽略
    ShowPopup(text, pending_cursor_);
}

void SelectionHotwordManager::OnAutoHideTimer() {
    auto_hide_timer_id_ = 0;
    HidePopup();
}

bool SelectionHotwordManager::IsPointOnPopup(POINT pt) const {
    if (!popup_hwnd_ || !popup_visible_) return false;
    RECT rc{};
    if (!GetWindowRect(popup_hwnd_, &rc)) return false;
    return PtInRect(&rc, pt) != 0;
}

void SelectionHotwordManager::ShowPopup(const std::string& text, POINT near_pt) {
    if (!popup_hwnd_) return;
    pending_text_ = text;

    const int w = Dp(kPopupWidthDp);
    const int h = Dp(kPopupHeightDp);
    // 默认放在光标右下角；若超出屏幕右/下边界则翻到左/上侧。
    int x = near_pt.x + kPopupOffsetX;
    int y = near_pt.y + kPopupOffsetY;
    HMONITOR mon = MonitorFromPoint(near_pt, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            if (x + w > mi.rcWork.right) x = near_pt.x - kPopupOffsetX - w;
            if (y + h > mi.rcWork.bottom) y = near_pt.y - kPopupOffsetY - h;
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
        }
    }
    SetWindowPos(popup_hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetLayeredWindowAttributes(popup_hwnd_, 0, 255, LWA_ALPHA);
    popup_visible_ = true;
    // 启动自动隐藏定时器
    if (auto_hide_timer_id_) KillTimer(helper_hwnd_, kAutoHideTimerId);
    auto_hide_timer_id_ = SetTimer(helper_hwnd_, kAutoHideTimerId, kAutoHideMs, nullptr);
}

void SelectionHotwordManager::HidePopup() {
    if (auto_hide_timer_id_) {
        KillTimer(helper_hwnd_, kAutoHideTimerId);
        auto_hide_timer_id_ = 0;
    }
    if (popup_hwnd_ && popup_visible_) {
        ShowWindow(popup_hwnd_, SW_HIDE);
    }
    popup_visible_ = false;
}

std::string SelectionHotwordManager::GetSelectedTextViaUia() {
    // UI Automation 通过 COM 调用焦点控件的 TextPattern 选区。Win32App 主线程已
    // 通过 winrt::init_apartment 初始化 COM STA，此处直接使用。
    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (FAILED(hr) || !automation) return "";

    std::string result;
    IUIAutomationElement* focused = nullptr;
    hr = automation->GetFocusedElement(&focused);
    if (SUCCEEDED(hr) && focused) {
        IUIAutomationTextPattern* text_pattern = nullptr;
        hr = focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern));
        if (SUCCEEDED(hr) && text_pattern) {
            IUIAutomationTextRangeArray* selections = nullptr;
            hr = text_pattern->GetSelection(&selections);
            if (SUCCEEDED(hr) && selections) {
                int length = 0;
                selections->get_Length(&length);
                for (int i = 0; i < length; ++i) {
                    IUIAutomationTextRange* range = nullptr;
                    if (SUCCEEDED(selections->GetElement(i, &range)) && range) {
                        BSTR text = nullptr;
                        // GetText(maxLength, &text)：maxLength=-1 表示不限制长度，
                        // 后续 NormalizeHotword 会裁剪/过滤过长内容。
                        if (SUCCEEDED(range->GetText(-1, &text)) && text) {
                            if (SysStringLen(text) > 0) {
                                // 取首个非空选区即可（多选区场景罕见且热词只需一段）。
                                result = Utf8FromUtf16(
                                    std::wstring(text, SysStringLen(text)));
                                SysFreeString(text);
                                range->Release();
                                break;
                            }
                            SysFreeString(text);
                        }
                        range->Release();
                    }
                }
                selections->Release();
            }
            text_pattern->Release();
        }
        focused->Release();
    }
    automation->Release();
    return result;
}

} // namespace voicestick
