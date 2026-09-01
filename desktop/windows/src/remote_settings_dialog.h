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

// 设备级遥控器设置对话框：从托盘设备子菜单「遥控器设置…」打开（仅小米遥控器显示），
// 编辑单台设备的 XiaomiSettings。传入的 current 为已用全局默认填平的有效值；保存时
// 与 defaults 相同则回调 std::nullopt（调用方 erase 覆盖，回落全局默认）。
class RemoteSettingsDialog {
public:
    RemoteSettingsDialog(HINSTANCE instance, HWND parent,
                         std::string device_id,
                         XiaomiSettings current,
                         XiaomiSettings defaults,
                         UiLanguage language);
    ~RemoteSettingsDialog();

    void Show();

    // (device_id, override)：override 为 nullopt 表示与全局默认一致（清除覆盖）。
    std::function<void(const std::string& device_id,
                       std::optional<XiaomiSettings> override)> on_settings_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildControls();
    void LoadSettingsIntoControls();
    void SaveSettings();
    void RestoreDefaults();
    int Dp(int px) const;
    void Relayout();

    struct Row {
        int advance;                  // 可见时推进的 y（Dp 换算后）
        std::vector<HWND> controls;   // 该行全部控件（标签 + 输入控件）
    };

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::string device_id_;
    XiaomiSettings current_;     // 当前有效值（加载进控件；保存时回写）
    XiaomiSettings defaults_;    // 全局默认（「恢复默认」按钮与保存比较基准）
    UiLanguage language_;
    UINT dpi_ = 96;

    HWND gain_db_edit_ = nullptr;
    HWND double_click_ms_edit_ = nullptr;
    // 「设置将在下次连接时生效」提示行（SS_LEFT 静态文本，布局时左对齐跨宽）。
    HWND hint_label_ = nullptr;
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

    static constexpr int kClientWidth = 480;
    // 分组标题 + 2 行输入 + 提示行 + 顶部/底部边距 + 按钮区，固定高度无滚动。
    static constexpr int kClientHeight = 228;

    static constexpr UINT kIdGainDb = 2700;
    static constexpr UINT kIdDoubleClickMs = 2701;
    static constexpr UINT kIdSave = 2702;
    static constexpr UINT kIdCancel = 2703;
    static constexpr UINT kIdRestoreDefaults = 2704;
};

} // namespace voicestick
