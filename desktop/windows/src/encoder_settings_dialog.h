#pragma once

#include "app_config.h"
#include "localization.h"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

// 设备级编码器设置对话框：从托盘设备子菜单「编码器设置…」打开，编辑单台设备的
// MiniEncoderC 编码器配置。传入的 current 为已用全局默认填平的有效值；保存时与
// defaults 相同则回调 std::nullopt（调用方 erase 覆盖，回落全局默认）。
class EncoderSettingsDialog {
public:
    EncoderSettingsDialog(HINSTANCE instance, HWND parent,
                           std::string device_id,
                           EncoderSettings current,
                           EncoderSettings defaults,
                           UiLanguage language);
    ~EncoderSettingsDialog();

    void Show();

    // (device_id, override)：override 为 nullopt 表示与全局默认一致（清除覆盖）。
    std::function<void(const std::string& device_id,
                       std::optional<EncoderSettings> override)> on_settings_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildControls();
    void LoadSettingsIntoControls();
    void SaveSettings();
    void RestoreDefaults();
    // 按 press_action / double_click_action 下拉选择切换 press_key / double_click_key 行显隐。
    void UpdateActionKeyVisibility();
    int Dp(int px) const;
    void Relayout();

    struct Row {
        int advance;                  // 可见时推进的 y（Dp 换算后）
        std::vector<HWND> controls;   // 该行全部控件（标签 + 输入控件）
        std::function<bool()> visible;  // 空 = 始终可见
    };

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::string device_id_;
    EncoderSettings current_;     // 当前有效值（加载进控件；保存时回写）
    EncoderSettings defaults_;    // 全局默认（「恢复默认」按钮与保存比较基准）
    UiLanguage language_;
    UINT dpi_ = 96;

    HWND to_arrow_check_ = nullptr;
    HWND rotation_invert_check_ = nullptr;
    HWND rotate_cw_edit_ = nullptr;
    HWND rotate_ccw_edit_ = nullptr;
    HWND fast_threshold_edit_ = nullptr;
    HWND rotate_cw_fast_edit_ = nullptr;
    HWND rotate_ccw_fast_edit_ = nullptr;
    HWND decide_window_edit_ = nullptr;
    HWND led_color_combo_ = nullptr;
    HWND press_action_combo_ = nullptr;
    HWND press_key_edit_ = nullptr;
    HWND double_click_action_combo_ = nullptr;
    HWND double_click_key_edit_ = nullptr;
    HWND save_button_ = nullptr;
    HWND restore_defaults_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT title_font_ = nullptr;

    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;
    std::vector<HWND> title_controls_;
    std::vector<Row> rows_;

    static constexpr int kClientWidth = 580;
    // 13 行控件 + 分组标题 + 顶部/底部边距 + 按钮区，固定高度无滚动。
    static constexpr int kClientHeight = 620;

    static constexpr UINT kIdToArrow = 2080;
    static constexpr UINT kIdRotationInvert = 2081;
    static constexpr UINT kIdRotateCwKey = 2082;
    static constexpr UINT kIdRotateCcwKey = 2083;
    static constexpr UINT kIdRotateFastThreshold = 2084;
    static constexpr UINT kIdRotateCwFastKey = 2085;
    static constexpr UINT kIdRotateCcwFastKey = 2086;
    static constexpr UINT kIdRotateDecideWindow = 2087;
    static constexpr UINT kIdLedColor = 2088;
    static constexpr UINT kIdPressAction = 2089;
    static constexpr UINT kIdPressKey = 2090;
    static constexpr UINT kIdDoubleClickAction = 2091;
    static constexpr UINT kIdDoubleClickKey = 2092;
    static constexpr UINT kIdSave = 2093;
    static constexpr UINT kIdCancel = 2094;
    static constexpr UINT kIdRestoreDefaults = 2095;
};

} // namespace voicestick
