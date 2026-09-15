#include "shortcut_capture.h"

#include "log.h"

#include <cstdio>
#include <string>

namespace voicestick {

ShortcutCapture* ShortcutCapture::active_instance_ = nullptr;

namespace {

constexpr UINT_PTR kArmTimerId = 1;
constexpr UINT_PTR kPollTimerId = 2;
constexpr UINT kPollIntervalMs = 15;
constexpr wchar_t kFallbackClassName[] = L"VoiceStickShortcutCaptureFallback";

// 武装兜底时记录前台窗口身份（标题 + 进程路径）：真机日志据此指认吞键的
// 第三方（输入法/安全软件等），不再需要用户口述复现环境。
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::string DescribeForegroundWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg) return "(none)";
    wchar_t title[256] = {};
    GetWindowTextW(fg, title, static_cast<int>(std::size(title)));
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    std::wstring process;
    HANDLE proc =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) process = path;
        CloseHandle(proc);
    }
    return "hwnd=" + std::to_string(reinterpret_cast<UINT_PTR>(fg)) + " pid=" +
           std::to_string(pid) + " title=\"" + Utf8(title) + "\" process=\"" +
           Utf8(process) + "\"";
}

} // namespace

// keydown 分类：决策与状态操作分离，纯函数可单测（钩子 proc 按返回值操作状态）。
ShortcutCapture::KeyAction ShortcutCapture::ClassifyKey(UINT vk, const Options& options,
                                                        bool have_modifier) {
    if (vk == VK_ESCAPE) return KeyAction::kCancel;
    const bool is_modifier = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                              vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                              vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                              vk == VK_LWIN || vk == VK_RWIN);
    if (is_modifier) {
        // 单键场景下修饰键本身即功能键（按住说话的 right ctrl）：直接作为主键
        // 捕获，不再累积等待后续主键。
        if (options.allow_modifier_as_key) return KeyAction::kCapture;
        return KeyAction::kAccumulateModifier;
    }
    if (options.require_modifier && !have_modifier) return KeyAction::kRejectNoModifier;
    return KeyAction::kCapture;
}

bool ShortcutCapture::IsPollEligibleVk(UINT vk) {
    // 0x01-0x06 是鼠标键（GetAsyncKeyState 同样上报），0x07 未定义；
    // 0xFF 保留。其余 0x08-0xFE 全覆盖（含 VK_BACK/媒体键/浏览器键）。
    return vk >= VK_BACK && vk <= 0xFE;
}

ShortcutCapture::~ShortcutCapture() { Cancel(); }

void ShortcutCapture::Start(const Options& options) {
    if (hook_) return;  // 幂等：捕获中忽略重复 Start
    options_ = options;
    ctrl_ = alt_ = shift_ = win_ = false;
    first_event_logged_ = false;
    poll_armed_ = false;
    active_instance_ = this;
    // LL 钩子在安装进程上下文执行，hMod 传本进程模块句柄（对齐 VoiceF5Suppressor）。
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        active_instance_ = nullptr;
        LogApp("ShortcutCapture: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
        return;
    }
    LogApp(std::string("ShortcutCapture: started require_modifier=") +
           (options_.require_modifier ? "1" : "0"));
    // 武装兜底取证通道：kFallbackArmMs 内 LL 钩子零事件则转轮询。
    EnsureFallbackWindow();
    if (fallback_hwnd_) {
        SetTimer(fallback_hwnd_, kArmTimerId, kFallbackArmMs, nullptr);
    }
}

void ShortcutCapture::Cancel() {
    StopFallback();
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
}

void ShortcutCapture::FinishCapture() {
    StopFallback();
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
}

// 两通道共用的 keydown 处理：状态判定与收尾完全等价，仅吞键能力不同。
bool ShortcutCapture::HandleKeyDown(UINT vk, const char* via_tag) {
    const bool have_modifier = ctrl_ || alt_ || shift_ || win_;
    switch (ClassifyKey(vk, options_, have_modifier)) {
        case KeyAction::kAccumulateModifier:
            // 修饰键 keydown 累积（LL 通道吞掉，其 keyup 放行），等待主键。
            switch (vk) {
                case VK_CONTROL:
                case VK_LCONTROL:
                case VK_RCONTROL: ctrl_ = true; break;
                case VK_MENU:
                case VK_LMENU:
                case VK_RMENU: alt_ = true; break;
                case VK_SHIFT:
                case VK_LSHIFT:
                case VK_RSHIFT: shift_ = true; break;
                case VK_LWIN:
                case VK_RWIN: win_ = true; break;
            }
            return true;
        case KeyAction::kCancel:
            // Esc 取消：清空已捕获修饰键，走统一收尾。
            ctrl_ = alt_ = shift_ = win_ = false;
            break;
        case KeyAction::kRejectNoModifier: {
            // 无修饰键的主键：拒绝，由调用方显示提示，捕获随之结束。
            char vk_text[8];
            snprintf(vk_text, sizeof(vk_text), "%02X", vk);
            LogApp(std::string("ShortcutCapture: rejected vk=0x") + vk_text +
                   " (modifier required)");
            FinishCapture();
            // 回调先拷贝到局部：回调内允许重新 Start 或销毁本对象。
            const auto on_rejected = on_rejected_no_modifier;
            if (on_rejected) on_rejected(vk);
            return true;
        }
        case KeyAction::kCapture:
            break;
    }
    // 统一收尾：Esc 取消或捕获成功，均结束捕获（LL 通道吞掉该键）。
    const bool cancelled = (vk == VK_ESCAPE);
    Result result;
    if (!cancelled) {
        // 修饰键固定 Ctrl/Alt/Shift/Win 序。
        if (ctrl_) result.modifiers.push_back(VK_CONTROL);
        if (alt_) result.modifiers.push_back(VK_MENU);
        if (shift_) result.modifiers.push_back(VK_SHIFT);
        if (win_) result.modifiers.push_back(VK_LWIN);
        result.vk = vk;
    }
    {
        char vk_text[8];
        snprintf(vk_text, sizeof(vk_text), "%02X", vk);
        LogApp(cancelled
                   ? std::string("ShortcutCapture: cancelled (Esc) via=") + via_tag
                   : "ShortcutCapture: captured vk=0x" + std::string(vk_text) +
                         " modifiers=" +
                         std::to_string(result.modifiers.size()) + " via=" + via_tag);
    }
    FinishCapture();
    if (cancelled) {
        const auto cancelled_cb = on_cancelled;
        if (cancelled_cb) cancelled_cb();
    } else {
        const auto captured_cb = on_captured;
        if (captured_cb) captured_cb(result);
    }
    return true;
}

LRESULT CALLBACK ShortcutCapture::LowLevelKeyboardProc(int code, WPARAM w_param,
                                                       LPARAM l_param) {
    auto* self = active_instance_;
    // 首事件打点：钩子确实活着、事件在到达（一次）。若录入超时且日志里没有这条，
    // 说明事件根本没进钩子（UIPI 前台提权隔离、第三方键盘过滤等环境因素），
    // 而非回调链路缺陷。该标志同时阻止轮询兜底武装。
    if (code >= 0 && self && self->hook_ && !self->first_event_logged_) {
        self->first_event_logged_ = true;
        LogApp("ShortcutCapture: first keyboard event wparam=" +
               std::to_string(static_cast<unsigned>(w_param)));
    }
    if (code >= 0 && self && self->hook_ &&
        (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN)) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
        if (self->HandleKeyDown(kb->vkCode, "ll")) return 1;
    }
    // keyup 与其他消息一律放行（与原 hotkey_settings_dialog 行为一致）。
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

// ===== 轮询兜底 =====
// 背景：真机出现本对话框前台时 LL 钩子完全收不到物理键盘事件（注入键正常），
// 焦点切走后恢复——钩子链首位置也防不住更底层的第三方过滤。GetAsyncKeyState
// 读的是系统异步键态表，在 LL 吞键之前更新，因此轮询可绕开整条钩子链取证。
// 代价是无法吞键，故只在 LL 证据缺失（kFallbackArmMs 零事件）时启用。

void ShortcutCapture::EnsureFallbackWindow() {
    if (fallback_hwnd_) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FallbackWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kFallbackClassName;
        if (RegisterClassW(&wc) != 0) registered = true;
    }
    if (!registered) return;
    fallback_hwnd_ = CreateWindowExW(0, kFallbackClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!fallback_hwnd_) {
        LogApp("ShortcutCapture: fallback window create failed err=" +
               std::to_string(GetLastError()));
    }
}

void ShortcutCapture::StopFallback() {
    poll_armed_ = false;
    if (fallback_hwnd_) {
        KillTimer(fallback_hwnd_, kArmTimerId);
        KillTimer(fallback_hwnd_, kPollTimerId);
        DestroyWindow(fallback_hwnd_);
        fallback_hwnd_ = nullptr;
    }
}

void ShortcutCapture::OnArmTimer() {
    KillTimer(fallback_hwnd_, kArmTimerId);
    if (!hook_ || first_event_logged_ || poll_armed_) return;
    // 快照当前键态：武装瞬间已按住的键不算新按下（如用户按住不放的修饰键）。
    for (UINT vk = 0; vk < 256; ++vk) {
        poll_down_[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }
    poll_armed_ = true;
    SetTimer(fallback_hwnd_, kPollTimerId, kPollIntervalMs, nullptr);
    LogApp("ShortcutCapture: LL hook silent for " + std::to_string(kFallbackArmMs) +
           "ms, poll fallback armed; foreground " + DescribeForegroundWindow());
}

void ShortcutCapture::OnPollTimer() {
    if (!hook_ || !poll_armed_) return;
    // 两趟扫描：先修饰键后主键。同一次采样内同时按下的组合键（如 Ctrl+Backspace，
    // 主键 VK 序可能更小）也能正确带上修饰键。
    for (int pass = 0; pass < 2; ++pass) {
        for (UINT vk = 0; vk <= 0xFE; ++vk) {
            if (!IsPollEligibleVk(vk)) continue;
            const bool is_modifier =
                (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                 vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                 vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                 vk == VK_LWIN || vk == VK_RWIN);
            if ((pass == 0) != is_modifier) continue;
            const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
            const bool was = poll_down_[vk];
            poll_down_[vk] = down;
            if (down && !was) {
                char vk_text[8];
                snprintf(vk_text, sizeof(vk_text), "%02X", vk);
                LogApp(std::string("ShortcutCapture: poll detected keydown vk=0x") +
                       vk_text);
                HandleKeyDown(vk, "poll");
                if (!hook_) return;  // 捕获已结束（FinishCapture 已停表清态）
            }
        }
    }
}

LRESULT CALLBACK ShortcutCapture::FallbackWndProc(HWND hwnd, UINT msg, WPARAM w_param,
                                                  LPARAM l_param) {
    auto* self = active_instance_;
    if (self && msg == WM_TIMER && hwnd == self->fallback_hwnd_) {
        if (w_param == kArmTimerId) {
            self->OnArmTimer();
            return 0;
        }
        if (w_param == kPollTimerId) {
            self->OnPollTimer();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace voicestick
