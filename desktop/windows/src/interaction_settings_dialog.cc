#include "interaction_settings_dialog.h"

#include "dpi_util.h"
#include "log.h"

#include <CommCtrl.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

// {0} 占位符替换（标题含设备 ID）。
std::wstring FormatText(std::wstring text, std::initializer_list<std::wstring> values) {
    int index = 0;
    for (const auto& value : values) {
        const std::wstring placeholder = L"{" + std::to_wstring(index++) + L"}";
        std::size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::wstring::npos) {
            text.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return text;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    if (!text) {
        AppendDialogWord(buffer, 0);
        return;
    }
    while (*text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateLeftLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

HWND CreateCombo(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(0, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

HWND CreateTrackbar(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                           WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

} // namespace

InteractionSettingsDialog::InteractionSettingsDialog(HINSTANCE instance, HWND parent,
                                                     std::string device_id,
                                                     InteractionSettings settings,
                                                     InteractionSettings defaults,
                                                     UiLanguage language)
    : instance_(instance), parent_(parent), device_id_(std::move(device_id)),
      settings_(std::move(settings)), defaults_(std::move(defaults)),
      language_(language) {}

InteractionSettingsDialog::~InteractionSettingsDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void InteractionSettingsDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            InteractionSettingsDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK InteractionSettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                       LPARAM l_param) {
    auto* dialog = reinterpret_cast<InteractionSettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<InteractionSettingsDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        SetWindowTextW(hwnd, FormatText(TrW(StringId::kInteractionSettingsTitle,
                                            EffectiveUiLanguage(dialog->language_)),
                                        {Utf16(dialog->device_id_)}).c_str());
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        RECT desired{0, 0, dialog->Dp(kClientWidth), dialog->Dp(kClientHeight)};
        AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dialog->dpi_);
        SetWindowPos(hwnd, nullptr, 0, 0, desired.right - desired.left,
                     desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
        dialog->BuildControls();
        dialog->LoadSettingsIntoControls();
        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        const int window_width = window_rect.right - window_rect.left;
        const int window_height = window_rect.bottom - window_rect.top;
        RECT work_area = GetWorkAreaForWindow(hwnd);
        const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
        const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR InteractionSettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdSave:
            SaveSettings();
            return TRUE;
        case kIdCancel:
            EndDialog(hwnd_, IDCANCEL);
            return TRUE;
        case kIdRestoreDefaults:
            RestoreDefaults();
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd_, IDCANCEL);
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        const auto control = reinterpret_cast<HWND>(l_param);
        if (std::find(label_controls_.begin(), label_controls_.end(), control) !=
            label_controls_.end()) {
            auto dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE InteractionSettingsDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 300;
    dialog_template.cy = 210;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"VoiceStick");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void InteractionSettingsDialog::BuildControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    ui_font_ = CreateUiFont(dpi_);
    const auto language = EffectiveUiLanguage(language_);

    auto remember = [&](HWND control) {
        if (control) all_controls_.push_back(control);
        return control;
    };
    auto remember_label = [&](HWND control) {
        if (control) label_controls_.push_back(control);
        return remember(control);
    };

    const int label_w = Dp(180);
    const int ctrl_x = Dp(200);
    const int ctrl_w = Dp(kClientWidth - 230);
    const int row_h = Dp(28);
    const int label_h = Dp(20);

    {
        HWND label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, label_h, instance_));
        imu_wake_sensitivity_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(120),
                                                           kIdImuWakeSensitivity, instance_));
        const StringId wake_names[] = {
            StringId::kSettingsImuWakeSensitivityLow,
            StringId::kSettingsImuWakeSensitivityMedium,
            StringId::kSettingsImuWakeSensitivityHigh,
        };
        for (const auto id : wake_names) {
            SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(TrW(id, language).c_str()));
        }
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, imu_wake_sensitivity_combo_};
        rows_.push_back(std::move(row));
    }
    {
        HWND label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, label_h, instance_));
        tap_to_arrow_check_ = remember(CreateButton(
            hwnd_, TrW(StringId::kSettingsTapToArrow, language).c_str(),
            0, 0, ctrl_w, Dp(22), kIdTapToArrow, instance_, BS_AUTOCHECKBOX));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, tap_to_arrow_check_};
        rows_.push_back(std::move(row));
    }
    {
        HWND label = remember_label(CreateLabel(
            hwnd_, TrW(StringId::kSettingsTapSensitivity, language).c_str(),
            0, 0, label_w, label_h, instance_));
        tap_sensitivity_trackbar_ = remember(CreateTrackbar(
            hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28), kIdTapSensitivity, instance_));
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETPAGESIZE, 0, 1);
        tap_sensitivity_value_label_ = remember(CreateLeftLabel(
            hwnd_, L"5", 0, 0, Dp(30), label_h, instance_));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, tap_sensitivity_trackbar_, tap_sensitivity_value_label_};
        rows_.push_back(std::move(row));
    }
    {
        HWND label = remember_label(CreateLabel(
            hwnd_, TrW(StringId::kSettingsAirMouseSensitivityX, language).c_str(),
            0, 0, label_w, label_h, instance_));
        air_mouse_sensitivity_x_trackbar_ = remember(CreateTrackbar(
            hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28), kIdAirMouseSensitivityX, instance_));
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETPAGESIZE, 0, 1);
        air_mouse_sensitivity_x_value_label_ = remember(CreateLeftLabel(
            hwnd_, L"5", 0, 0, Dp(30), label_h, instance_));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, air_mouse_sensitivity_x_trackbar_, air_mouse_sensitivity_x_value_label_};
        rows_.push_back(std::move(row));
    }
    {
        HWND label = remember_label(CreateLabel(
            hwnd_, TrW(StringId::kSettingsAirMouseSensitivityY, language).c_str(),
            0, 0, label_w, label_h, instance_));
        air_mouse_sensitivity_y_trackbar_ = remember(CreateTrackbar(
            hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28), kIdAirMouseSensitivityY, instance_));
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETPAGESIZE, 0, 1);
        air_mouse_sensitivity_y_value_label_ = remember(CreateLeftLabel(
            hwnd_, L"5", 0, 0, Dp(30), label_h, instance_));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, air_mouse_sensitivity_y_trackbar_, air_mouse_sensitivity_y_value_label_};
        rows_.push_back(std::move(row));
    }

    restore_defaults_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kEncoderSettingsRestoreDefaults, language).c_str(),
        0, 0, Dp(110), Dp(30), kIdRestoreDefaults, instance_));
    save_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kSave, language).c_str(),
        0, 0, Dp(80), Dp(30), kIdSave, instance_, BS_DEFPUSHBUTTON));
    cancel_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kCancel, language).c_str(),
        0, 0, Dp(80), Dp(30), kIdCancel, instance_));

    for (HWND control : all_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
}

void InteractionSettingsDialog::LoadSettingsIntoControls() {
    int wake_idx = 0;
    if (settings_.imu_wake_sensitivity == ImuWakeSensitivity::kMedium) wake_idx = 1;
    if (settings_.imu_wake_sensitivity == ImuWakeSensitivity::kHigh) wake_idx = 2;
    SendMessageW(imu_wake_sensitivity_combo_, CB_SETCURSEL, wake_idx, 0);
    SendMessageW(tap_to_arrow_check_, BM_SETCHECK,
                 settings_.tap_to_arrow ? BST_CHECKED : BST_UNCHECKED, 0);
    const int tap_pos = std::clamp(settings_.tap_sensitivity, 1, 10);
    SendMessageW(tap_sensitivity_trackbar_, TBM_SETPOS, TRUE, tap_pos);
    SetWindowTextW(tap_sensitivity_value_label_, std::to_wstring(tap_pos).c_str());
    const int ax_pos = std::clamp(settings_.air_mouse_sensitivity_x, 1, 10);
    SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETPOS, TRUE, ax_pos);
    SetWindowTextW(air_mouse_sensitivity_x_value_label_, std::to_wstring(ax_pos).c_str());
    const int ay_pos = std::clamp(settings_.air_mouse_sensitivity_y, 1, 10);
    SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETPOS, TRUE, ay_pos);
    SetWindowTextW(air_mouse_sensitivity_y_value_label_, std::to_wstring(ay_pos).c_str());
    Relayout();
}

void InteractionSettingsDialog::RestoreDefaults() {
    settings_ = defaults_;
    LoadSettingsIntoControls();
}

void InteractionSettingsDialog::SaveSettings() {
    InteractionSettings edited;
    int wake_idx = static_cast<int>(SendMessageW(imu_wake_sensitivity_combo_, CB_GETCURSEL, 0, 0));
    edited.imu_wake_sensitivity = (wake_idx == 2) ? ImuWakeSensitivity::kHigh
                              : (wake_idx == 1) ? ImuWakeSensitivity::kMedium
                                                : ImuWakeSensitivity::kLow;
    edited.tap_to_arrow = SendMessageW(tap_to_arrow_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    edited.tap_sensitivity = static_cast<int>(SendMessageW(tap_sensitivity_trackbar_, TBM_GETPOS, 0, 0));
    edited.air_mouse_sensitivity_x = static_cast<int>(SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_GETPOS, 0, 0));
    edited.air_mouse_sensitivity_y = static_cast<int>(SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_GETPOS, 0, 0));
    settings_ = edited;
    EndDialog(hwnd_, IDOK);
    if (on_settings_changed) {
        // 与全局默认一致 → nullopt（调用方清除覆盖，回落默认）。
        if (settings_ == defaults_) {
            on_settings_changed(device_id_, std::nullopt);
        } else {
            on_settings_changed(device_id_, settings_);
        }
    }
}

int InteractionSettingsDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

void InteractionSettingsDialog::Relayout() {
    if (!hwnd_) return;
    const int label_w = Dp(180);
    const int ctrl_x = Dp(200);
    const int ctrl_w = Dp(kClientWidth - 230);
    int y = Dp(20);
    for (const auto& row : rows_) {
        const bool vis = !row.visible || row.visible();
        for (HWND control : row.controls) {
            if (!control) continue;
            if (control == tap_sensitivity_value_label_ ||
                control == air_mouse_sensitivity_x_value_label_ ||
                control == air_mouse_sensitivity_y_value_label_) {
                SetWindowPos(control, nullptr, ctrl_x + ctrl_w - Dp(40), y + Dp(5),
                             Dp(30), Dp(20), SWP_NOZORDER | SWP_NOACTIVATE);
            } else if (std::find(label_controls_.begin(), label_controls_.end(), control) !=
                       label_controls_.end()) {
                SetWindowPos(control, nullptr, Dp(10), y + Dp(3), label_w, Dp(20),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                SetWindowPos(control, nullptr, ctrl_x, y, ctrl_w, Dp(28),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ShowWindow(control, vis ? SW_SHOW : SW_HIDE);
        }
        if (vis) y += row.advance;
    }
    const int btn_y = Dp(kClientHeight) - Dp(30) - Dp(15);
    if (restore_defaults_button_) {
        SetWindowPos(restore_defaults_button_, nullptr, Dp(10), btn_y, Dp(110), Dp(30),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (save_button_) {
        SetWindowPos(save_button_, nullptr, Dp(kClientWidth - 200), btn_y, Dp(80), Dp(30),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (cancel_button_) {
        SetWindowPos(cancel_button_, nullptr, Dp(kClientWidth - 105), btn_y, Dp(80), Dp(30),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

} // namespace voicestick
