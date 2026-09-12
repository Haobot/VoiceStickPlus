#include "shortcut_capture.h"

namespace voicestick {

ShortcutCapture* ShortcutCapture::s_active = nullptr;

ShortcutCapture::~ShortcutCapture() {
    Stop();
}

UINT ShortcutCapture::NormalizeModifier(UINT vk) {
    switch (vk) {
        case VK_LCONTROL:
        case VK_RCONTROL:
            return VK_CONTROL;
        case VK_LMENU:
        case VK_RMENU:
            return VK_MENU;
        case VK_LSHIFT:
        case VK_RSHIFT:
            return VK_SHIFT;
        case VK_LWIN:
        case VK_RWIN:
            return VK_LWIN;
        default:
            return 0;
    }
}

bool ShortcutCapture::Start(const Options& options) {
    if (active_ || s_active) return false;
    // WH_KEYBOARD_LL 的钩子回调在该线程取消息时执行；dwThreadId 传 0
    // （全局钩子）但对话框模态循环与本线程 UI 输入即可满足捕获需求。
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) return false;
    options_ = options;
    modifiers_.clear();
    active_ = true;
    s_active = this;
    return true;
}

void ShortcutCapture::Stop() {
    if (!active_) return;
    Finish();
}

void ShortcutCapture::Finish() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    active_ = false;
    modifiers_.clear();
    if (s_active == this) s_active = nullptr;
}

LRESULT CALLBACK ShortcutCapture::LowLevelProc(int code, WPARAM w_param, LPARAM l_param) {
    auto* capture = s_active;
    if (code != HC_ACTION || !capture || !capture->active_) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
    }
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    const UINT vk = info->vkCode;
    // 捕获期间吞掉全部键盘事件，防止快捷键真实生效。
    if (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN) {
        if (vk == VK_ESCAPE) {
            if (capture->on_cancelled) capture->on_cancelled();
            capture->Finish();
            return 1;
        }
        if (const UINT modifier = NormalizeModifier(vk); modifier != 0) {
            bool exists = false;
            for (UINT held : capture->modifiers_) exists |= (held == modifier);
            if (!exists) capture->modifiers_.push_back(modifier);
            return 1;
        }
        if (capture->options_.require_modifier && capture->modifiers_.empty()) {
            if (capture->on_rejected_no_modifier) capture->on_rejected_no_modifier(vk);
            return 1;
        }
        Result result;
        result.modifiers = capture->modifiers_;
        result.vk = vk;
        if (capture->on_captured) capture->on_captured(result);
        capture->Finish();
        return 1;
    }
    if (w_param == WM_KEYUP || w_param == WM_SYSKEYUP) {
        if (const UINT modifier = NormalizeModifier(vk); modifier != 0) {
            for (auto it = capture->modifiers_.begin(); it != capture->modifiers_.end(); ++it) {
                if (*it == modifier) {
                    capture->modifiers_.erase(it);
                    break;
                }
            }
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

} // namespace voicestick
