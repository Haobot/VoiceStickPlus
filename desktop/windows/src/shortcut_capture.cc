#include "shortcut_capture.h"

#include "log.h"

#include <cstdio>

namespace voicestick {

ShortcutCapture* ShortcutCapture::active_instance_ = nullptr;

ShortcutCapture::~ShortcutCapture() { Cancel(); }

void ShortcutCapture::Start(const Options& options) {
    if (hook_) return;  // 幂等：捕获中忽略重复 Start
    options_ = options;
    ctrl_ = alt_ = shift_ = win_ = false;
    first_event_logged_ = false;
    active_instance_ = this;
    // LL 钩子在安装进程上下文执行，hMod 传本进程模块句柄（对齐 VoiceF5Suppressor）。
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        active_instance_ = nullptr;
        LogApp("ShortcutCapture: SetWindowsHookEx WH_KEYBOARD_LL failed err=" +
               std::to_string(GetLastError()));
    } else {
        LogApp(std::string("ShortcutCapture: started require_modifier=") +
               (options_.require_modifier ? "1" : "0"));
    }
}

void ShortcutCapture::Cancel() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
}

void ShortcutCapture::FinishCapture() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_instance_ == this) active_instance_ = nullptr;
}

LRESULT CALLBACK ShortcutCapture::LowLevelKeyboardProc(int code, WPARAM w_param,
                                                       LPARAM l_param) {
    auto* self = active_instance_;
    // 首事件打点：钩子确实活着、事件在到达（一次）。若录入超时且日志里没有这条，
    // 说明事件根本没进钩子（UIPI 前台提权隔离等环境因素），而非回调链路缺陷。
    if (code >= 0 && self && self->hook_ && !self->first_event_logged_) {
        self->first_event_logged_ = true;
        LogApp("ShortcutCapture: first keyboard event wparam=" +
               std::to_string(static_cast<unsigned>(w_param)));
    }
    if (code >= 0 && self && (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN)) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
        const UINT vk = kb->vkCode;
        switch (vk) {
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL:
                self->ctrl_ = true;
                return 1;
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:
                self->alt_ = true;
                return 1;
            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:
                self->shift_ = true;
                return 1;
            case VK_LWIN:
            case VK_RWIN:
                self->win_ = true;
                return 1;
            case VK_ESCAPE:
                // Esc 取消：清空已捕获修饰键，走统一收尾。
                self->ctrl_ = self->alt_ = self->shift_ = self->win_ = false;
                break;
            default:
                if (self->options_.require_modifier &&
                    !self->ctrl_ && !self->alt_ && !self->shift_ && !self->win_) {
                    // 无修饰键的主键：拒绝，由调用方显示提示，捕获随之结束。
                    char vk_text[8];
                    snprintf(vk_text, sizeof(vk_text), "%02X", vk);
                    LogApp(std::string("ShortcutCapture: rejected vk=0x") + vk_text +
                           " (modifier required)");
                    self->FinishCapture();
                    // 回调先拷贝到局部：回调内允许重新 Start 或销毁本对象。
                    const auto on_rejected = self->on_rejected_no_modifier;
                    if (on_rejected) on_rejected(vk);
                    return 1;
                }
                break;
        }
        // 统一收尾：Esc 取消或捕获成功，均结束捕获并吞掉该键。
        const bool cancelled = (vk == VK_ESCAPE);
        Result result;
        if (!cancelled) {
            // 修饰键固定 Ctrl/Alt/Shift/Win 序。
            if (self->ctrl_) result.modifiers.push_back(VK_CONTROL);
            if (self->alt_) result.modifiers.push_back(VK_MENU);
            if (self->shift_) result.modifiers.push_back(VK_SHIFT);
            if (self->win_) result.modifiers.push_back(VK_LWIN);
            result.vk = vk;
        }
        {
            char vk_text[8];
            snprintf(vk_text, sizeof(vk_text), "%02X", vk);
            LogApp(cancelled
                       ? "ShortcutCapture: cancelled (Esc)"
                       : "ShortcutCapture: captured vk=0x" + std::string(vk_text) +
                             " modifiers=" +
                             std::to_string(result.modifiers.size()));
        }
        self->FinishCapture();
        if (cancelled) {
            const auto on_cancelled = self->on_cancelled;
            if (on_cancelled) on_cancelled();
        } else {
            const auto on_captured = self->on_captured;
            if (on_captured) on_captured(result);
        }
        return 1;
    }
    // keyup 与其他消息一律放行（与原 hotkey_settings_dialog 行为一致）。
    return CallNextHookEx(nullptr, code, w_param, l_param);
}

} // namespace voicestick
