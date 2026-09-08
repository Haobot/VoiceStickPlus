#pragma once

#include <Windows.h>

#include <functional>
#include <vector>

namespace voicestick {

// 公共快捷键捕获组件：WH_KEYBOARD_LL 低级键盘钩子，捕获一次"修饰键+主键"按键。
// 从 hotkey_settings_dialog 抽出，供全局热键设置与小米遥控器按键映射等场景复用。
// 语义（与原 hotkey_settings_dialog 内嵌实现完全一致）：
// - 修饰键 keydown：累积（Ctrl/Alt/Shift/Win）并吞掉（return 1），其 keyup 放行；
// - Esc keydown：清空已捕获修饰键，结束捕获并触发 on_cancelled；
// - 非修饰键 keydown：require_modifier 且无修饰键时触发 on_rejected_no_modifier
//   并结束捕获；否则记录主键，结束捕获并触发 on_captured；
// - 捕获结束（成功/拒绝/取消）即 UnhookWindowsHookEx；回调在安装钩子的线程触发。
// 进程内单例（与 VoiceF5Suppressor 同款 active_instance_ 指针模式），同时只允许
// 一个捕获会话；Start 幂等（捕获中再次调用直接忽略）。
class ShortcutCapture {
public:
    struct Options {
        // true = 必须含修饰键（全局热键场景）；false = 单键也可（按键映射场景）。
        bool require_modifier = false;
        // 单键模式下修饰键（VK_LCONTROL/VK_RCONTROL 等左右变体）直接作为主键
        // 捕获而非累积：按住说话热键（right ctrl/capslock）等「修饰键即功能键」
        // 场景用。仅 require_modifier=false 时有意义。
        bool allow_modifier_as_key = false;
    };
    struct Result {
        // VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN 子集，固定 Ctrl/Alt/Shift/Win 序。
        std::vector<UINT> modifiers;
        UINT vk = 0;  // 主键
    };

    // keydown 事件的处置决策（纯函数，可单测）：
    // - kAccumulateModifier：吞掉并累积修饰键状态（keyup 放行）；
    // - kCapture：作为主键捕获，走统一收尾（吞掉该键）；
    // - kCancel：Esc 取消；
    // - kRejectNoModifier：需修饰键但按了裸主键，拒绝并结束捕获。
    enum class KeyAction { kAccumulateModifier, kCapture, kCancel, kRejectNoModifier };
    static KeyAction ClassifyKey(UINT vk, const Options& options, bool have_modifier);

    ShortcutCapture() = default;
    ~ShortcutCapture();
    ShortcutCapture(const ShortcutCapture&) = delete;
    ShortcutCapture& operator=(const ShortcutCapture&) = delete;

    // 捕获成功（在安装钩子的线程回调）。
    std::function<void(const Result&)> on_captured;
    // require_modifier 时按了无修饰键的主键：调用方显示提示，捕获随之结束。
    std::function<void(UINT vk)> on_rejected_no_modifier;
    // Esc 取消。
    std::function<void()> on_cancelled;

    // 幂等：捕获中再次调用直接忽略。
    void Start(const Options& options);
    // 程序主动取消（如对话框销毁）：仅卸钩子，不触发任何回调。
    void Cancel();
    bool active() const { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM w_param, LPARAM l_param);
    // 结束捕获：卸钩子、清单例，返回前由调用方触发对应回调。
    void FinishCapture();

    HHOOK hook_ = nullptr;
    Options options_;
    bool ctrl_ = false;
    bool alt_ = false;
    bool shift_ = false;
    bool win_ = false;
    // 首个键盘事件只记一次日志（区分"钩子被 UIPI 等环境因素隔离收不到事件"与
    // "收到事件但回调链路没走通"，GitHub Issue #1 排障盲区）。
    bool first_event_logged_ = false;
    static ShortcutCapture* active_instance_;
};

} // namespace voicestick
