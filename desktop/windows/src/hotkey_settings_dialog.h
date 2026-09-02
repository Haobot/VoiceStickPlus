#pragma once

#include <Windows.h>

#include "app_config.h"
#include "shortcut_capture.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

class HotkeySettingsDialog {
public:
    HotkeySettingsDialog(HINSTANCE instance, HWND parent, UiLanguage language);
    ~HotkeySettingsDialog();

    void Show();

    std::function<void(const std::string& hotkey_string)> on_hotkey_confirmed;

    void UpdateHotkeyDisplay();

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildControls();
    void DestroyControls();
    void OnHotkeyCapture();
    bool ValidateAndSave();
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    HFONT ui_font_ = nullptr;
    UiLanguage language_ = UiLanguage::kEnglish;

    HWND hotkey_label_ = nullptr;
    HWND hotkey_capture_button_ = nullptr;
    HWND hint_label_ = nullptr;
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;

    ShortcutCapture capture_;
    UINT captured_modifiers_ = 0;
    UINT captured_vk_ = 0;

    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;

    static constexpr int kClientWidth = 360;
    static constexpr int kClientHeight = 180;
    static constexpr UINT kIdHotkeyCapture = 6001;
    static constexpr UINT kIdOk = 6002;
    static constexpr UINT kIdCancel = 6003;
};

} // namespace voicestick
