#pragma once

#include "app_config.h"

#include <Windows.h>

#include <functional>
#include <string>

namespace voicestick {

// 划词添加热词管理器：启用后安装全局低级鼠标钩子，监听 WM_LBUTTONUP，
// 通过 UI Automation 读取焦点控件的选中文本，非空时在选区附近弹出"添加到热词"
// 浮层按钮。用户点击按钮后通过 on_add_hotword 回调把规范化后的文本交回上层。
//
// 线程模型：所有方法与回调均在创建它的 UI 线程执行（钩子虽为全局低级钩子，但
// 回调由安装线程的消息泵分发）。UI Automation 在 UI 线程查询，避免阻塞系统鼠标
// （低级钩子超时会被 Windows 强制摘除）。
//
// 弹窗为 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST 的分层窗口，不抢焦点、
// 不在任务栏显示。点击弹窗任意位置即触发添加；点击弹窗外或 4 秒无操作自动隐藏。
class SelectionHotwordManager {
public:
    SelectionHotwordManager(HINSTANCE instance, HWND parent);
    ~SelectionHotwordManager();

    SelectionHotwordManager(const SelectionHotwordManager&) = delete;
    SelectionHotwordManager& operator=(const SelectionHotwordManager&) = delete;

    // 启用/禁用划词监测。禁用时卸载钩子并隐藏弹窗。可在运行期反复切换。
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_; }

    // 同步 UI 语言，影响弹窗按钮文案。配置变更时由上层调用。
    void SetLanguage(UiLanguage language) { language_ = language; }

    // 用户点击"添加到热词"按钮时回调，参数为规范化后的选中文本。
    // 调用方负责把它加入热词库、持久化配置并给出反馈（通知/字幕等）。
    std::function<void(const std::string& text)> on_add_hotword;

private:
    // 隐藏辅助窗口的回调（消息泵分发）。
    static LRESULT CALLBACK HelperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // 弹窗的回调。
    static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // 低级鼠标钩子：仅识别 WM_LBUTTONUP，其余放行。
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM w_param, LPARAM l_param);

    void EnsureHelperWindow();
    void EnsurePopupWindow();
    void RegisterPopupClass();

    // 钩子触发：记录光标位置，启动短定时器延后查询 UIA（避免阻塞鼠标系统）。
    void OnMouseLeftUp(POINT pt);
    // 定时器到期：查询 UIA 选中文本，非空则显示弹窗。
    void OnQueryTimer();
    // 弹窗自动隐藏定时器到期。
    void OnAutoHideTimer();

    void ShowPopup(const std::string& text, POINT near_pt);
    void HidePopup();
    bool IsPointOnPopup(POINT pt) const;

    // 通过 UI Automation 读取焦点控件的选中文本。失败或无选中返回空串。
    std::string GetSelectedTextViaUia();

    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND helper_hwnd_ = nullptr;     // 消息-only 窗口，承载定时器
    HWND popup_hwnd_ = nullptr;      // 浮层按钮窗口
    HHOOK mouse_hook_ = nullptr;
    bool enabled_ = false;
    UiLanguage language_ = UiLanguage::kSystem;

    // 钩子记下的待查询光标位置（屏幕坐标）。
    POINT pending_cursor_{};
    UINT_PTR query_timer_id_ = 0;
    UINT_PTR auto_hide_timer_id_ = 0;
    bool popup_visible_ = false;
    std::string pending_text_;       // 弹窗当前对应的选中文本（已规范化）

    // 静态回调路由：从 HWND 反查 this 指针。窗口创建时存入 GWLP_USERDATA。
    static constexpr UINT_PTR kQueryTimerId = 5101;
    static constexpr UINT_PTR kAutoHideTimerId = 5102;
    static constexpr int kQueryDelayMs = 80;       // 等 UIA 选区稳定
    static constexpr int kAutoHideMs = 4000;       // 弹窗 4 秒自动隐藏
    static constexpr int kMaxHotwordLen = 64;      // 超长选区忽略，避免误把整段当热词
    static constexpr int kPopupOffsetX = 8;        // 弹窗相对光标的偏移
    static constexpr int kPopupOffsetY = 18;
    static constexpr int kPopupWidthDp = 132;      // 弹窗宽度（DIP）
    static constexpr int kPopupHeightDp = 32;      // 弹窗高度（DIP）
    static constexpr wchar_t kHelperWindowClass[] = L"VoiceStickSelectionHelper";
    static constexpr wchar_t kPopupWindowClass[] = L"VoiceStickSelectionPopup";
};

} // namespace voicestick
