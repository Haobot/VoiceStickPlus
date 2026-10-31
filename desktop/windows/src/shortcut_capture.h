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
//
// LL 钩子静默兜底（2026-09 小米按键映射排障）：真机出现「本对话框前台时物理键盘
// 事件完全到不了 LL 钩子、焦点切到别的窗口即恢复」（疑似第三方键盘过滤——本机装有
// 微信输入法/火绒等，注入键不受影响而物理键被吞）。为此内置独立取证通道：Start 后
// 若 kFallbackArmMs 内 LL 钩子零事件，自动武装 GetAsyncKeyState 轮询（15ms），
// 检测 up→down 跳变走同一套分类/收尾逻辑。轮询通道无法吞键（按下的键会同时透传到
// 前台窗口），因此仅在 LL 证据缺失时启用；LL 健康时零行为变化。
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

    // 轮询通道的 VK 采纳范围（纯函数，可单测）：排除鼠标键（0x01-0x06）与保留区，
    // 避免点击「录入」按钮/切换窗口的鼠标动作被误判为按键。
    static bool IsPollEligibleVk(UINT vk);

    // LL 静默多少毫秒后武装轮询兜底（与两个对话框的录入提示超时同量级）。
    static constexpr UINT kFallbackArmMs = 3000;

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
    // keydown 统一处理（LL 与轮询两通道共用）：分类→累积/收尾→回调。
    // 返回 true 表示该键已被捕获流程消费（LL 通道据此吞键；轮询通道无法吞键，
    // 返回值仅用于内部收尾判断）。via_tag 仅用于日志区分来源（"ll"/"poll"）。
    bool HandleKeyDown(UINT vk, const char* via_tag);
    // 结束捕获：卸钩子、停兜底、清单例，返回前由调用方触发对应回调。
    void FinishCapture();

    // ===== 轮询兜底（LL 静默时启用；全部运行在主线程，经消息窗口定时器驱动）=====
    void EnsureFallbackWindow();
    void StopFallback();  // 停表 + 销毁消息窗口（幂等）
    void OnArmTimer();    // 到点且 LL 仍静默 → 武装轮询
    void OnPollTimer();   // 一次采样：up→down 跳变转 HandleKeyDown
    static LRESULT CALLBACK FallbackWndProc(HWND hwnd, UINT msg, WPARAM w_param,
                                            LPARAM l_param);

    HHOOK hook_ = nullptr;
    Options options_;
    bool ctrl_ = false;
    bool alt_ = false;
    bool shift_ = false;
    bool win_ = false;
    // 首个键盘事件只记一次日志（区分"钩子被 UIPI 等环境因素隔离收不到事件"与
    // "收到事件但回调链路没走通"，GitHub Issue #1 排障盲区）。
    // 同时作为「LL 通道活着」的证据：武装定时器到点时若已见事件则不启用轮询。
    bool first_event_logged_ = false;
    // 轮询兜底状态。
    HWND fallback_hwnd_ = nullptr;
    bool poll_armed_ = false;
    bool poll_down_[256] = {};
    static ShortcutCapture* active_instance_;
};

} // namespace voicestick
