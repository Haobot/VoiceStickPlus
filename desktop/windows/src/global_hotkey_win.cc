#include "global_hotkey_win.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace voicestick {

namespace {

std::vector<std::string> SplitString(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

std::string TrimString(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(),
            s.end());
    return s;
}

std::string Lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::optional<UINT> ParseSingleKey(const std::string& key) {
    const auto lower = Lowercase(TrimString(key));
    if (lower == "ctrl" || lower == "control") return MOD_CONTROL;
    if (lower == "alt") return MOD_ALT;
    if (lower == "shift") return MOD_SHIFT;
    if (lower == "win" || lower == "windows" || lower == "meta") return MOD_WIN;

    if (lower.size() == 1) {
        char ch = static_cast<char>(std::toupper(lower[0]));
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<UINT>(ch);
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<UINT>(ch);
        }
    }

    if (lower == "space") return VK_SPACE;
    if (lower == "enter" || lower == "return") return VK_RETURN;
    if (lower == "esc" || lower == "escape") return VK_ESCAPE;
    if (lower == "tab") return VK_TAB;
    if (lower == "backspace") return VK_BACK;
    if (lower == "delete") return VK_DELETE;

    if (lower.substr(0, 1) == "f" && lower.size() >= 2) {
        int num = 0;
        for (size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') return std::nullopt;
            num = num * 10 + (lower[i] - '0');
        }
        if (num >= 1 && num <= 24) {
            return VK_F1 + (num - 1);
        }
    }

    return std::nullopt;
}

bool IsModifierKey(UINT key) {
    return key == MOD_CONTROL || key == MOD_ALT || key == MOD_SHIFT || key == MOD_WIN;
}

UINT ModifierToVkey(UINT mod) {
    switch (mod) {
        case MOD_CONTROL: return VK_CONTROL;
        case MOD_ALT: return VK_MENU;
        case MOD_SHIFT: return VK_SHIFT;
        case MOD_WIN: return VK_LWIN;
        default: return 0;
    }
}

std::string VkeyToName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
    }
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    return std::string();
}

} // namespace

std::string GlobalHotkeyWin::BindingToString(const Binding& binding) {
    std::vector<std::string> parts;
    if (binding.modifiers & MOD_CONTROL) parts.push_back("Ctrl");
    if (binding.modifiers & MOD_ALT) parts.push_back("Alt");
    if (binding.modifiers & MOD_SHIFT) parts.push_back("Shift");
    if (binding.modifiers & MOD_WIN) parts.push_back("Win");
    const auto key_name = VkeyToName(binding.vk);
    if (!key_name.empty()) {
        parts.push_back(key_name);
    }
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "+";
        result += parts[i];
    }
    return result;
}

bool GlobalHotkeyWin::TestBinding(const Binding& binding) {
    static const int kTestId = 9999;
    static HWND dummy_hwnd = nullptr;
    if (!dummy_hwnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VoiceStickHotkeyTestWindow";
        RegisterClassW(&wc);
        dummy_hwnd = CreateWindowExW(0, L"VoiceStickHotkeyTestWindow", L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    UINT modifiers = binding.modifiers | MOD_NOREPEAT;
    if (!::RegisterHotKey(dummy_hwnd, kTestId, modifiers, binding.vk)) {
        return false;
    }
    ::UnregisterHotKey(dummy_hwnd, kTestId);
    return true;
}

std::optional<GlobalHotkeyWin::Binding> GlobalHotkeyWin::ParseHotkeyString(const std::string& text) {
    auto parts = SplitString(text, '+');
    if (parts.empty()) return std::nullopt;

    Binding binding{};
    std::vector<UINT> keys;
    for (const auto& part : parts) {
        auto key = ParseSingleKey(part);
        if (!key) return std::nullopt;
        keys.push_back(*key);
    }

    UINT non_modifier = 0;
    for (auto key : keys) {
        if (IsModifierKey(key)) {
            binding.modifiers |= key;
        } else {
            if (non_modifier != 0) {
                return std::nullopt;
            }
            non_modifier = key;
        }
    }

    if (non_modifier == 0) {
        return std::nullopt;
    }

    binding.vk = non_modifier;
    binding.display_text = text;
    return binding;
}

GlobalHotkeyWin::GlobalHotkeyWin(HWND hwnd) : hwnd_(hwnd) {}

GlobalHotkeyWin::~GlobalHotkeyWin() {
    Unregister();
}

bool GlobalHotkeyWin::Register(const std::string& hotkey_string) {
    Unregister();

    if (hotkey_string.empty()) {
        LogHotkey("global hotkey disabled");
        return false;
    }

    auto binding = ParseHotkeyString(hotkey_string);
    if (!binding) {
        LogHotkey("failed to parse hotkey string: " + hotkey_string);
        return false;
    }
    binding_ = *binding;

    UINT modifiers = binding_.modifiers | MOD_NOREPEAT;
    if (!::RegisterHotKey(hwnd_, hotkey_id_, modifiers, binding_.vk)) {
        DWORD error = ::GetLastError();
        LogHotkey("RegisterHotKey failed for '" + hotkey_string + "', error=" + std::to_string(error));
        return false;
    }

    registered_ = true;
    is_down_ = false;
    LogHotkey("registered hotkey: " + hotkey_string);
    return true;
}

void GlobalHotkeyWin::Unregister() {
    StopReleasePolling();
    if (registered_) {
        ::UnregisterHotKey(hwnd_, hotkey_id_);
        registered_ = false;
        is_down_ = false;
        LogHotkey("unregistered hotkey");
    }
    binding_ = {};
}

void GlobalHotkeyWin::StartReleasePolling() {
    if (timer_id_ != 0) return;
    timer_id_ = ::SetTimer(hwnd_, 1, 30, nullptr);
}

void GlobalHotkeyWin::StopReleasePolling() {
    if (timer_id_ != 0) {
        ::KillTimer(hwnd_, timer_id_);
        timer_id_ = 0;
    }
}

bool GlobalHotkeyWin::IsBindingStillDown() const {
    if (binding_.modifiers & MOD_CONTROL) {
        if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) return false;
    }
    if (binding_.modifiers & MOD_ALT) {
        if ((::GetAsyncKeyState(VK_MENU) & 0x8000) == 0) return false;
    }
    if (binding_.modifiers & MOD_SHIFT) {
        if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) return false;
    }
    if (binding_.modifiers & MOD_WIN) {
        if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
            (::GetAsyncKeyState(VK_RWIN) & 0x8000) == 0) {
            return false;
        }
    }
    if ((::GetAsyncKeyState(binding_.vk) & 0x8000) == 0) return false;
    return true;
}

bool GlobalHotkeyWin::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_HOTKEY) {
        if (w_param == hotkey_id_ && registered_) {
            if (!is_down_) {
                is_down_ = true;
                LogHotkey("hotkey pressed: " + binding_.display_text);
                if (on_pressed) on_pressed();
                StartReleasePolling();
            }
        }
        return true;
    }
    if (message == WM_TIMER && w_param == timer_id_ && timer_id_ != 0) {
        if (registered_ && is_down_) {
            if (!IsBindingStillDown()) {
                is_down_ = false;
                StopReleasePolling();
                if (on_released) on_released();
            }
        } else {
            StopReleasePolling();
        }
        return true;
    }
    return false;
}

} // namespace voicestick
