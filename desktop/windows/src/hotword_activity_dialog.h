#pragma once

#include "hotword_activity.h"
#include "localization.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace voicestick {

// 智能热词管理可视化对话框（模态快照视图）：各分类热词（使用次数/活跃度/闲置
// 天数）、回收池逐出词、活跃分类监控状态。打开与"刷新"时经 take_snapshot 回调
// 取一次快照，无后台自动刷新（活跃度统计本身由协调器实时维护）。回收池每行
// 行首为 ✗ 图标（自定义 state image，替代内置勾选框）：点击即删除该词（经
// remove_evicted_words 回调落盘）；"清空回收池"按钮一键删除全部池内词（带
// 确认框）。
class HotwordActivityDialog {
public:
    using SnapshotType = HotwordActivitySnapshot;

    HotwordActivityDialog(HINSTANCE instance, HWND owner, UiLanguage language);
    ~HotwordActivityDialog();

    void Show();

    // 快照数据源（外壳接线到 VoiceStickCoordinator::TakeHotwordActivitySnapshot，
    // UI 线程调用；未接线时列表为空）。
    std::function<HotwordActivitySnapshot()> take_snapshot;
    // 回收池删除入口（外壳接线到 VoiceStickCoordinator::RemoveEvictedWords；
    // 勾选单行传一个词，清空按钮传当前显示的全部词；未接线时删除入口禁用）。
    std::function<void(const std::vector<std::string>&)> remove_evicted_words;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildContent();
    void Refresh();
    void DestroyControls();
    void HandleEvictedCheck(int item_index);
    void HandleClearPool();
    std::wstring Utf16(const std::string& text) const;
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND status_label_ = nullptr;
    HWND word_list_ = nullptr;
    HWND evicted_label_ = nullptr;
    HWND evicted_list_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND close_button_ = nullptr;
    HIMAGELIST evicted_state_images_ = nullptr;
    HFONT ui_font_ = nullptr;
    UiLanguage language_ = UiLanguage::kEnglish;
    UINT dpi_ = 96;
    std::vector<BYTE> dialog_template_;
    // 回收池当前显示行的词（行号 → 词，Refresh 时重建；勾选删除与清空入口用）。
    std::vector<std::string> evicted_rows_;

    static constexpr int kWordListId = 100;
    static constexpr int kEvictedListId = 101;
    static constexpr int kRefreshButtonId = 102;
    static constexpr int kClearPoolButtonId = 103;
};

} // namespace voicestick
