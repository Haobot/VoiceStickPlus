#pragma once

#include "app_config.h"

#include <Windows.h>

#include <functional>
#include <utility>
#include <vector>

namespace voicestick {

class SettingsDialog {
public:
    SettingsDialog(HINSTANCE instance, HWND parent, AppConfig config);
    ~SettingsDialog();

    void Show();

    std::function<void(AppConfig)> on_config_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void RebuildUi();
    void DestroyControls();
    void BuildControls();
    void LoadConfigIntoControls();
    void SaveSettings();
    void UpdateOutputTargetVisibility();
    void UpdateProviderVisibility();
    void UpdateRefinePromptVisibility();
    void UpdateTapSensitivityLabel();
    void UpdateAirMouseSensitivityXLabel();
    void UpdateAirMouseSensitivityYLabel();
    void ApplyTrialApiKey();
    void ChooseDebugDirectory();
    bool IsLabelControl(HWND control) const;
    int Dp(int px) const;
    // 按声明式布局表重新定位所有控件并按可见行数动态调整窗口高度。
    void Relayout();
    // 按客户区高度调整窗口尺寸（顶部固定，底部伸缩）。
    void ResizeWindow(int client_h);
    // 行内条件：apply_trial_button 显隐 + api_key_edit 宽度，在 Relayout 末尾调用。
    void ApplyApiKeyLayout();

    // 布局模型：把每行/块抽象为可独立显隐的条目，Relayout 统一应用定位。
    struct LayoutPart {
        HWND control;
        int x;
        int y_off;  // 相对行基线 y 的偏移
        int w;
        int h;
        // true=仅参与定位，显隐交给外部（如 apply_trial_button 行内条件按钮），
        // 避免 Relayout 在可见行上 ShowWindow(SW_SHOW) 覆盖外部隐藏。
        bool defer_visibility = false;
    };
    struct LayoutEntry {
        int advance;                         // 该项可见时推进的 y（Dp 换算后）
        std::vector<LayoutPart> parts;       // 该项的控件
        std::function<bool()> visible;       // 空 = 始终可见
    };

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    AppConfig config_;
    UINT dpi_ = 96;

    HWND language_combo_ = nullptr;
    HWND provider_combo_ = nullptr;
    HWND api_key_edit_ = nullptr;
    HWND apply_trial_button_ = nullptr;
    HWND resource_combo_ = nullptr;
    HWND hotwords_edit_ = nullptr;
    HWND llm_base_url_edit_ = nullptr;
    HWND llm_api_key_edit_ = nullptr;
    HWND llm_model_edit_ = nullptr;
    HWND refine_check_ = nullptr;
    HWND refine_prompt_label_ = nullptr;
    HWND refine_prompt_edit_ = nullptr;
    HWND launch_at_login_check_ = nullptr;
    HWND debug_audio_check_ = nullptr;
    HWND show_imu_debug_check_ = nullptr;
    HWND imu_wake_sensitivity_combo_ = nullptr;
    HWND tap_to_arrow_check_ = nullptr;
    HWND tap_sensitivity_trackbar_ = nullptr;
    HWND tap_sensitivity_value_label_ = nullptr;
    HWND air_mouse_sensitivity_x_trackbar_ = nullptr;
    HWND air_mouse_sensitivity_x_value_label_ = nullptr;
    HWND air_mouse_sensitivity_y_trackbar_ = nullptr;
    HWND air_mouse_sensitivity_y_value_label_ = nullptr;
    HWND output_target_combo_ = nullptr;
    HWND wechat_hotkey_edit_ = nullptr;
    HWND wechat_hotkey_label_ = nullptr;
    HWND trigger_mode_label_ = nullptr;
    HWND trigger_mode_hold_radio_ = nullptr;
    HWND trigger_mode_click_radio_ = nullptr;
    HWND debug_dir_edit_ = nullptr;
    HWND resource_label_ = nullptr;
    HWND save_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT title_font_ = nullptr;
    int scroll_pos_ = 0;  // 垂直滚动位置（像素，Dp 换算后）
    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;
    std::vector<HWND> title_controls_;
    std::vector<LayoutEntry> layout_;

    static constexpr int kClientWidth = 580;
    static constexpr int kClientHeight = 1240;
    static constexpr UINT kIdLanguageCombo = 2000;
    static constexpr UINT kIdProviderCombo = 2001;
    static constexpr UINT kIdApiKeyEdit = 2002;
    static constexpr UINT kIdResourceCombo = 2003;
    static constexpr UINT kIdHotwordsEdit = 2004;
    static constexpr UINT kIdLlmBaseUrlEdit = 2005;
    static constexpr UINT kIdLlmApiKeyEdit = 2006;
    static constexpr UINT kIdLlmModelEdit = 2007;
    static constexpr UINT kIdRefineText = 2016;
    static constexpr UINT kIdLaunchAtLogin = 2009;
    static constexpr UINT kIdDebugAudio = 2010;
    static constexpr UINT kIdShowImuDebug = 2017;
    static constexpr UINT kIdImuWakeSensitivity = 2018;
    static constexpr UINT kIdTapToArrow = 2019;
    static constexpr UINT kIdDebugDirEdit = 2011;
    static constexpr UINT kIdChooseDir = 2012;
    static constexpr UINT kIdSave = 2013;
    static constexpr UINT kIdCancel = 2014;
    static constexpr UINT kIdApplyTrialApiKey = 2015;
    static constexpr UINT kIdRefinePromptEdit = 2022;
    static constexpr UINT kIdTapSensitivity = 2023;
    static constexpr UINT kIdAirMouseSensitivityX = 2024;
    static constexpr UINT kIdAirMouseSensitivityY = 2025;
    static constexpr UINT kIdOutputTarget = 2026;
    static constexpr UINT kIdWechatHotkey = 2027;
    static constexpr UINT kIdWechatVirtualMic = 2028;
    static constexpr UINT kIdWechatAutoSwitch = 2029;
    static constexpr UINT kIdWechatVirtualMicCapture = 2030;
    static constexpr UINT kIdTriggerModeHold = 2031;
    static constexpr UINT kIdTriggerModeClick = 2032;
};

} // namespace voicestick
