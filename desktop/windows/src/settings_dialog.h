#pragma once

#include "app_config.h"

#include <Windows.h>

#include <functional>
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
    void UpdateProviderVisibility();
    void ChooseDebugDirectory();
    bool IsLabelControl(HWND control) const;
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    AppConfig config_;
    UINT dpi_ = 96;

    HWND provider_combo_ = nullptr;
    HWND api_key_edit_ = nullptr;
    HWND resource_combo_ = nullptr;
    HWND auto_enter_check_ = nullptr;
    HWND debug_audio_check_ = nullptr;
    HWND debug_dir_edit_ = nullptr;
    HWND resource_label_ = nullptr;
    HFONT ui_font_ = nullptr;
    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;

    static constexpr int kClientWidth = 520;
    static constexpr int kClientHeight = 340;
    static constexpr UINT kIdProviderCombo = 2001;
    static constexpr UINT kIdApiKeyEdit = 2002;
    static constexpr UINT kIdResourceCombo = 2003;
    static constexpr UINT kIdAutoEnter = 2004;
    static constexpr UINT kIdDebugAudio = 2005;
    static constexpr UINT kIdDebugDirEdit = 2006;
    static constexpr UINT kIdChooseDir = 2007;
    static constexpr UINT kIdSave = 2008;
    static constexpr UINT kIdCancel = 2009;
};

} // namespace voicestick
