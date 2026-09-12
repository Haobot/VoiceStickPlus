#pragma once

#include "app_config.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 热词纠错对话框：划词后点击"纠错"按钮弹出。显示被识别错误的文本，用户在输入框
// 手动输入正确文本（或点击 AI 建议词汇）保存纠错规则到热词库（错误文本 -> 正确文本）。
//
// AI 辅助纠错：输入框 EN_CHANGE 后 600ms 防抖，经 HotwordSuggestClient 异步请求与
// 当前输入措辞相近的建议词汇，回填建议列表；单击列表项即保存该建议。
//
// 线程模型：窗口创建于 UI 线程，消息由主消息泵分发；LLM 回调在 detached 后台线程，
// 解析结果后经 PostMessage(kWmSuggestReady) 携带堆分配 vector 回切 UI 线程（处理完
// delete，无跨线程成员访问）。
class CorrectionDialog {
public:
    CorrectionDialog(HINSTANCE instance, HWND parent);
    ~CorrectionDialog();

    CorrectionDialog(const CorrectionDialog&) = delete;
    CorrectionDialog& operator=(const CorrectionDialog&) = delete;

    // 显示对话框：wrong_text 为划词选中的错误文本（规范化后），near_pt 为建议的
    // 弹出锚点（屏幕坐标），target_hwnd 为划词发生时的前台窗口（保存后把正确文本
    // 粘贴回该窗口的选区）。config 用于 LLM 建议请求（拷贝，后台线程安全）。
    void Show(const std::string& wrong_text, POINT near_pt, HWND target_hwnd,
              const AppConfig& config);

    // 同步 UI 语言，影响对话框文案。配置变更时由上层调用。
    void SetLanguage(UiLanguage language) { language_ = language; }

    // 保存回调（UI 线程）：wrong 为错误文本，right 为正确文本，target_hwnd 为划词
    // 目标窗口。调用方负责写入热词纠错映射、持久化配置、回填替换目标窗口选区并
    // 给出反馈。
    std::function<void(const std::string& wrong, const std::string& right,
                       HWND target_hwnd)>
        on_save;

private:
    static LRESULT CALLBACK DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void RegisterWindowClass(HINSTANCE instance);

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void BuildControls();
    void ApplyLanguage();
    int Dp(int px) const;

    // 输入变化 -> 防抖定时器到期 -> 请求 AI 建议。
    void OnSuggestTimer();
    void RequestSuggestions();
    // kWmSuggestReady 处理：wp=ok，lp=堆分配的 vector<string>*（处理后 delete）。
    void OnSuggestReady(WPARAM wp, LPARAM lp);
    void FillSuggestionList(const std::vector<std::string>& suggestions);
    // 保存当前输入（或建议项）的纠错规则并隐藏对话框。
    void SaveFromEdit();
    void SaveRight(const std::string& right);
    void Hide();

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    HWND wrong_label_ = nullptr;
    HWND wrong_value_label_ = nullptr;
    HWND right_label_ = nullptr;
    HWND right_edit_ = nullptr;
    HWND suggest_label_ = nullptr;
    HWND suggest_list_ = nullptr;
    HWND save_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    UiLanguage language_ = UiLanguage::kSystem;

    std::string wrong_text_;   // 当前纠错的错误文本
    HWND target_hwnd_ = nullptr;  // 划词发生时的前台窗口（保存后回填替换）
    AppConfig config_;         // Show 时快照，LLM 建议请求用（拷贝）
    UINT_PTR suggest_timer_id_ = 0;
    // 请求序号：递增后旧的异步建议响应被丢弃（快速连续输入时只应用最新请求结果）。
    std::uint32_t suggest_request_seq_ = 0;

    static constexpr UINT_PTR kSuggestTimerId = 5201;
    static constexpr UINT kWmSuggestReady = WM_APP + 220;
    static constexpr int kSuggestDebounceMs = 600;
    static constexpr int kClientWidthDp = 380;
    static constexpr int kClientHeightDp = 312;
    static constexpr wchar_t kWindowClass[] = L"VoiceStickCorrectionDialog";
    // 控件 ID
    static constexpr int kIdRightEdit = 2101;
    static constexpr int kIdSuggestList = 2102;
    static constexpr int kIdSave = 2103;
    static constexpr int kIdCancel = 2104;
};

} // namespace voicestick
