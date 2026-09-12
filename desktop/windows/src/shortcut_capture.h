#pragma once

// 全局键盘快捷键捕获器（WH_KEYBOARD_LL）：Start 后在安装线程的消息循环内
// 收集当前按住的修饰键，按下第一个非修饰键（或 Esc 取消）时经回调上报并
// 自动结束。捕获期间吞掉全部键盘事件，避免真实触发系统快捷键。
// 单会话限制：同一时刻仅允许一个活跃捕获（模态对话框场景不会并发）。
#include <Windows.h>

#include <functional>
#include <vector>

namespace voicestick {

class ShortcutCapture {
public:
    struct Result {
        std::vector<UINT> modifiers;  // 当前按住的修饰键（R* 已归一化为
                                      // VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN），按下序
        UINT vk = 0;                  // 触发捕获的非修饰键 VK
    };

    struct Options {
        bool require_modifier = false;  // 无修饰键时拒绝捕获并回调 on_rejected_no_modifier
    };

    // 捕获成功（require_modifier 满足或未要求）。
    std::function<void(const Result& result)> on_captured;
    // require_modifier 且按下的普通键无任何修饰键（参数为该键 VK）；捕获继续。
    std::function<void(UINT vk)> on_rejected_no_modifier;
    // Esc 取消捕获。
    std::function<void()> on_cancelled;

    ShortcutCapture() = default;
    ~ShortcutCapture();

    ShortcutCapture(const ShortcutCapture&) = delete;
    ShortcutCapture& operator=(const ShortcutCapture&) = delete;

    bool active() const { return active_; }

    // 安装键盘钩子开始捕获；已有活跃会话或安装失败返回 false。
    // 低级键盘钩子回调依赖安装线程的消息泵（模态对话框内部自带）。
    bool Start(const Options& options);
    // 主动结束捕获（卸载钩子，不触发任何回调）。
    void Stop();
    // 等价 Stop()：窗口销毁/析构路径的清理入口（不触发 on_cancelled）。
    void Cancel() { Stop(); }

private:
    static LRESULT CALLBACK LowLevelProc(int code, WPARAM w_param, LPARAM l_param);

    // 归一化左侧/右侧变体：VK_LCONTROL/VK_RCONTROL → VK_CONTROL 等；非修饰键返回 0。
    static UINT NormalizeModifier(UINT vk);

    void Finish();

    Options options_;
    HHOOK hook_ = nullptr;
    bool active_ = false;
    std::vector<UINT> modifiers_;  // 当前按住的修饰键（按下序、去重）

    // 同一时刻仅一个活跃捕获（两个模态对话框不可能同时捕获）。
    static ShortcutCapture* s_active;
};

} // namespace voicestick
