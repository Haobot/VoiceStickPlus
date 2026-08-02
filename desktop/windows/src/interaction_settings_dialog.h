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

// 设备级交互设置对话框：从托盘设备子菜单「设备交互设置…」打开，编辑单台设备的
// IMU/体感交互配置。传入的 settings 为已用全局默认填平的有效值；保存时与 defaults
// 相同则回调 std::nullopt（调用方 erase 覆盖，回落全局默认）。
class InteractionSettingsDialog {
public:
    InteractionSettingsDialog(HINSTANCE instance, HWND parent,
                               std::string device_id,
                               InteractionSettings settings,
                               InteractionSettings defaults,
                               UiLanguage language);
    ~InteractionSettingsDialog();

    void Show();

    // (device_id, override)：override 为 nullopt 表示与全局默认一致（清除覆盖）。
    std::function<void(const std::string& device_id,
                       std::optional<InteractionSettings> override)> on_settings_changed;

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
        std::function<bool()> visible;  // 空 = 始终可见
    };

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::string device_id_;
    InteractionSettings settings_;   // 当前有效值（加载进控件；保存时回写）
    InteractionSettings defaults_;   // 全局默认（「恢复默认」按钮与保存比较基准）
    UiLanguage language_;
    UINT dpi_ = 96;

    HWND imu_wake_sensitivity_combo_ = nullptr;
    HWND tap_to_arrow_check_ = nullptr;
    HWND tap_sensitivity_trackbar_ = nullptr;
    HWND tap_sensitivity_value_label_ = nullptr;
    HWND air_mouse_sensitivity_x_trackbar_ = nullptr;
    HWND air_mouse_sensitivity_x_value_label_ = nullptr;
    HWND air_mouse_sensitivity_y_trackbar_ = nullptr;
    HWND air_mouse_sensitivity_y_value_label_ = nullptr;
    HWND save_button_ = nullptr;
    HWND restore_defaults_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;

    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;
    std::vector<Row> rows_;

    static constexpr int kClientWidth = 580;
    // 5 行控件（IMU 唤醒灵敏度/敲击映射/敲击灵敏度/体感鼠标 X/体感鼠标 Y）+ 顶部/底部边距
    // + 按钮区，固定高度无滚动。
    static constexpr int kClientHeight = 420;

    static constexpr UINT kIdImuWakeSensitivity = 2060;
    static constexpr UINT kIdTapToArrow = 2061;
    static constexpr UINT kIdTapSensitivity = 2062;
    static constexpr UINT kIdAirMouseSensitivityX = 2063;
    static constexpr UINT kIdAirMouseSensitivityY = 2064;
    static constexpr UINT kIdSave = 2070;
    static constexpr UINT kIdCancel = 2071;
    static constexpr UINT kIdRestoreDefaults = 2072;
};

} // namespace voicestick
