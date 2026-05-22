#pragma once

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>

namespace voicestick {

class GlobalHotkeyWin {
public:
    struct Binding {
        UINT modifiers = 0;
        UINT vk = 0;
        std::string display_text;
    };

    explicit GlobalHotkeyWin(HWND hwnd);
    ~GlobalHotkeyWin();

    GlobalHotkeyWin(const GlobalHotkeyWin&) = delete;
    GlobalHotkeyWin& operator=(const GlobalHotkeyWin&) = delete;

    bool Register(const std::string& hotkey_string);
    void Unregister();
    bool IsRegistered() const { return registered_; }

    bool HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);

    std::function<void()> on_pressed;
    std::function<void()> on_released;

    static std::optional<Binding> ParseHotkeyString(const std::string& text);

private:
    void StartReleasePolling();
    void StopReleasePolling();
    bool IsBindingStillDown() const;

    HWND hwnd_ = nullptr;
    int hotkey_id_ = 1;
    UINT timer_id_ = 0;
    Binding binding_{};
    bool registered_ = false;
    bool is_down_ = false;
};

} // namespace voicestick
