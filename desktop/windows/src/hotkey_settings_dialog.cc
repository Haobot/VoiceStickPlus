#include "hotkey_settings_dialog.h"

#include "global_hotkey_win.h"
#include "localization.h"
#include "log.h"
#include "dpi_util.h"

#include <algorithm>

namespace voicestick {

namespace {

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) {
        buffer->push_back(0);
    }
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const BYTE* bytes = reinterpret_cast<const BYTE*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        buffer->push_back(bytes[i]);
    }
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    for (const wchar_t* p = text; *p; ++p) {
        AppendDialogWord(buffer, static_cast<WORD>(*p));
    }
    AppendDialogWord(buffer, 0);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE instance) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, instance, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, UINT id,
                  HINSTANCE instance) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(id), instance, nullptr);
}

std::wstring Utf16FromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), len);
    return result;
}

} // namespace

HotkeySettingsDialog::HotkeySettingsDialog(HINSTANCE instance, HWND parent, UiLanguage language)
    : instance_(instance), parent_(parent), language_(language) {}

HotkeySettingsDialog::~HotkeySettingsDialog() {
    DestroyControls();
    if (ui_font_) DeleteObject(ui_font_);
}

void HotkeySettingsDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_, DialogProc,
                           reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK HotkeySettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    HotkeySettingsDialog* dlg = nullptr;
    if (message == WM_INITDIALOG) {
        dlg = reinterpret_cast<HotkeySettingsDialog*>(l_param);
        dlg->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
    } else {
        dlg = reinterpret_cast<HotkeySettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (dlg) {
        return dlg->HandleMessage(message, w_param, l_param);
    }
    return FALSE;
}

LPCDLGTEMPLATE HotkeySettingsDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 300;
    dialog_template.cy = 180;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, TrW(StringId::kHotkeyTitle, language_).c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

int HotkeySettingsDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

void HotkeySettingsDialog::BuildControls() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    dpi_ = GetDpiForWindow(hwnd_);
    ui_font_ = CreateUiFont(dpi_);

    const int margin = Dp(16);
    const int button_height = Dp(32);
    const int capture_height = Dp(48);

    hotkey_label_ = CreateLabel(hwnd_, TrW(StringId::kHotkeyCurrent, language_).c_str(),
                                margin, margin, Dp(120), Dp(20), instance_);
    SendMessageW(hotkey_label_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    all_controls_.push_back(hotkey_label_);

    hotkey_capture_button_ = CreateButton(hwnd_, TrW(StringId::kHotkeyCaptureButton, language_).c_str(),
                                          margin, margin + Dp(32),
                                          client.right - margin * 2, capture_height,
                                          kIdHotkeyCapture, instance_);
    SendMessageW(hotkey_capture_button_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    all_controls_.push_back(hotkey_capture_button_);

    hint_label_ = CreateLabel(hwnd_, TrW(StringId::kHotkeyHint, language_).c_str(),
                              margin, margin + Dp(32) + capture_height + Dp(8),
                              client.right - margin * 2, Dp(20), instance_);
    SendMessageW(hint_label_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    all_controls_.push_back(hint_label_);

    const int button_width = Dp(100);
    const int button_y = client.bottom - margin - button_height;
    ok_button_ = CreateButton(hwnd_, TrW(StringId::kOk, language_).c_str(),
                              client.right - margin * 2 - button_width * 2,
                              button_y, button_width, button_height,
                              kIdOk, instance_);
    SendMessageW(ok_button_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    EnableWindow(ok_button_, FALSE);
    all_controls_.push_back(ok_button_);

    cancel_button_ = CreateButton(hwnd_, TrW(StringId::kCancel, language_).c_str(),
                                  client.right - margin - button_width,
                                  button_y, button_width, button_height,
                                  kIdCancel, instance_);
    SendMessageW(cancel_button_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    all_controls_.push_back(cancel_button_);
}

void HotkeySettingsDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        DestroyWindow(control);
    }
    all_controls_.clear();
    hotkey_label_ = nullptr;
    hotkey_capture_button_ = nullptr;
    hint_label_ = nullptr;
    ok_button_ = nullptr;
    cancel_button_ = nullptr;
}

void HotkeySettingsDialog::UpdateHotkeyDisplay() {
    if (captured_vk_ == 0) {
        if (capture_.active()) {
            SetWindowTextW(hotkey_capture_button_, TrW(StringId::kHotkeyCapturePrompt, language_).c_str());
        } else {
            SetWindowTextW(hotkey_capture_button_, TrW(StringId::kHotkeyCaptureButton, language_).c_str());
        }
        EnableWindow(ok_button_, FALSE);
        return;
    }
    GlobalHotkeyWin::Binding binding{};
    binding.modifiers = captured_modifiers_;
    binding.vk = captured_vk_;
    const auto hotkey_str = GlobalHotkeyWin::BindingToString(binding);
    const auto display_text = Utf16FromUtf8(hotkey_str);
    SetWindowTextW(hotkey_capture_button_, display_text.c_str());
    EnableWindow(ok_button_, TRUE);
}

void HotkeySettingsDialog::OnHotkeyCapture() {
    if (capture_.active()) return;
    captured_modifiers_ = 0;
    captured_vk_ = 0;
    SetWindowTextW(hotkey_capture_button_, TrW(StringId::kHotkeyCapturePrompt, language_).c_str());
    // ShortcutCapture::Result → GlobalHotkeyWin::Binding 的 MOD_* 标志。
    capture_.on_captured = [this](const ShortcutCapture::Result& result) {
        UINT modifiers = 0;
        for (UINT mod : result.modifiers) {
            switch (mod) {
                case VK_CONTROL: modifiers |= MOD_CONTROL; break;
                case VK_MENU: modifiers |= MOD_ALT; break;
                case VK_SHIFT: modifiers |= MOD_SHIFT; break;
                case VK_LWIN: modifiers |= MOD_WIN; break;
                default: break;
            }
        }
        captured_modifiers_ = modifiers;
        captured_vk_ = result.vk;
        UpdateHotkeyDisplay();
    };
    capture_.on_rejected_no_modifier = [this](UINT) {
        SetWindowTextW(hint_label_, TrW(StringId::kHotkeyMissingModifier, language_).c_str());
        UpdateHotkeyDisplay();
    };
    capture_.on_cancelled = [this]() {
        UpdateHotkeyDisplay();
    };
    ShortcutCapture::Options options;
    options.require_modifier = true;  // 全局热键场景：必须含修饰键
    capture_.Start(options);
}


bool HotkeySettingsDialog::ValidateAndSave() {
    if (captured_modifiers_ == 0 || captured_vk_ == 0) {
        return false;
    }
    GlobalHotkeyWin::Binding binding{};
    binding.modifiers = captured_modifiers_;
    binding.vk = captured_vk_;
    if (!GlobalHotkeyWin::TestBinding(binding)) {
        MessageBoxW(hwnd_, TrW(StringId::kHotkeyConflictMessage, language_).c_str(),
                    TrW(StringId::kHotkeyConflictTitle, language_).c_str(),
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    const auto hotkey_str = GlobalHotkeyWin::BindingToString(binding);
    if (on_hotkey_confirmed) {
        on_hotkey_confirmed(hotkey_str);
    }
    return true;
}

INT_PTR HotkeySettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_INITDIALOG:
            BuildControls();
            return TRUE;
        case WM_COMMAND: {
            const UINT cmd = LOWORD(w_param);
            if (cmd == kIdHotkeyCapture) {
                OnHotkeyCapture();
                return TRUE;
            }
            if (cmd == kIdOk) {
                if (ValidateAndSave()) {
                    EndDialog(hwnd_, IDOK);
                }
                return TRUE;
            }
            if (cmd == kIdCancel) {
                EndDialog(hwnd_, IDCANCEL);
                return TRUE;
            }
            break;
        }
        case WM_DPICHANGED:
            DestroyControls();
            BuildControls();
            UpdateHotkeyDisplay();
            return 0;
        case WM_DESTROY:
            capture_.Cancel();
            DestroyControls();
            return 0;
        default:
            break;
    }
    return FALSE;
}

} // namespace voicestick
