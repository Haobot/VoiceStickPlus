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
    // kGlobalHotkey：全局语音热键（组合键，必须含修饰键，RegisterHotkey 可注册）。
    // kPushToTalk：本机麦克风按住说话热键（单键，修饰键本身可作键，如 right ctrl；
    // LL 钩子实现，不走 RegisterHotkey，确认时不做占用检测）。
    enum class Mode {
        kGlobalHotkey,
        kPushToTalk,
    };

    HotkeySettingsDialog(HINSTANCE instance, HWND parent, UiLanguage language,
                         Mode mode = Mode::kGlobalHotkey,
                         std::string current_key = std::string());
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
    // 捕获结束/超时收尾：停提示定时器（幂等）。
    void StopCaptureHintTimer();
    bool ValidateAndSave();
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    HFONT ui_font_ = nullptr;
    UiLanguage language_ = UiLanguage::kEnglish;
    Mode mode_ = Mode::kGlobalHotkey;
    // PTT 模式初始展示的当前键名（config [local_asr].push_to_talk_key），空 = 未配置。
    std::string current_key_;

    HWND hotkey_label_ = nullptr;
    HWND hotkey_capture_button_ = nullptr;
    HWND hint_label_ = nullptr;
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;

    ShortcutCapture capture_;
    UINT captured_modifiers_ = 0;
    UINT captured_vk_ = 0;
    // PTT 模式捕获成功后的规范键名（push_to_talk_key.h Format 输出）。
    std::string captured_ptt_key_;

    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;

    static constexpr int kClientWidth = 360;
    static constexpr int kClientHeight = 180;
    static constexpr UINT kIdHotkeyCapture = 6001;
    static constexpr UINT kIdOk = 6002;
    static constexpr UINT kIdCancel = 6003;

    // 录入超时提示定时器：录入启动后 kCaptureHintTimeoutMs 内无任何键盘事件到
    // 达（UIPI 前台提权隔离等）时弹一次引导，不中断进行中的捕获。
    static constexpr UINT_PTR kCaptureHintTimerId = 0x5343;
    static constexpr UINT kCaptureHintTimeoutMs = 3000;
};

} // namespace voicestick
