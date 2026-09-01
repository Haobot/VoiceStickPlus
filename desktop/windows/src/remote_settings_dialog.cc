#include "remote_settings_dialog.h"

#include "dpi_util.h"
#include "log.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cstdio>
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

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring GetEditText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
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

std::wstring FormatGain(double gain_db) {
    char buffer[32]{};
    snprintf(buffer, sizeof(buffer), "%g", gain_db);
    return Utf16(buffer);
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

HWND CreateSectionTitle(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst,
                DWORD extra_style = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

} // namespace

RemoteSettingsDialog::RemoteSettingsDialog(HINSTANCE instance, HWND parent,
                                           std::string device_id,
                                           XiaomiSettings current,
                                           XiaomiSettings defaults,
                                           UiLanguage language)
    : instance_(instance), parent_(parent), device_id_(std::move(device_id)),
      current_(std::move(current)), defaults_(std::move(defaults)),
      language_(language) {}

RemoteSettingsDialog::~RemoteSettingsDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
    if (title_font_) {
        DeleteObject(title_font_);
        title_font_ = nullptr;
    }
}

void RemoteSettingsDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            RemoteSettingsDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK RemoteSettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                  LPARAM l_param) {
    auto* dialog = reinterpret_cast<RemoteSettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<RemoteSettingsDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        SetWindowTextW(hwnd, FormatText(TrW(StringId::kRemoteSettingsTitle,
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

INT_PTR RemoteSettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
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

LPCDLGTEMPLATE RemoteSettingsDialog::BuildDialogTemplate() {
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

void RemoteSettingsDialog::BuildControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ui_font_ = CreateUiFont(dpi_);
    title_font_ = CreateUiFontBold(dpi_);
    const auto language = EffectiveUiLanguage(language_);

    auto remember = [&](HWND control) {
        if (control) all_controls_.push_back(control);
        return control;
    };
    auto remember_label = [&](HWND control) {
        if (control) label_controls_.push_back(control);
        return remember(control);
    };
    auto remember_title = [&](HWND control) {
        if (control) title_controls_.push_back(control);
        return remember_label(control);
    };

    auto label_text = [&](StringId id) {
        return TrW(id, language) + L":";
    };

    const int label_w = Dp(200);
    const int ctrl_x = Dp(220);
    const int ctrl_w = Dp(kClientWidth - 250);
    const int row_h = Dp(28);
    const int label_h = Dp(20);
    const int title_h = Dp(20);

    // ===== 遥控器设置（分组标题复用菜单文案） =====
    {
        HWND title = remember_title(CreateSectionTitle(
            hwnd_, TrW(StringId::kMenuRemoteSettings, language).c_str(),
            0, 0, ctrl_x + ctrl_w - Dp(10), title_h, instance_));
        Row row;
        row.advance = title_h + Dp(4);
        row.controls = {title};
        rows_.push_back(std::move(row));
    }

    // 增益（gain_db）
    {
        HWND label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsRemoteGainDb).c_str(),
            0, 0, label_w, label_h, instance_));
        gain_db_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                            kIdGainDb, instance_));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, gain_db_edit_};
        rows_.push_back(std::move(row));
    }

    // 双击窗口（double_click_ms）
    {
        HWND label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsRemoteDoubleClickMs).c_str(),
            0, 0, label_w, label_h, instance_));
        double_click_ms_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                    kIdDoubleClickMs, instance_, ES_NUMBER));
        Row row;
        row.advance = row_h + Dp(10);
        row.controls = {label, double_click_ms_edit_};
        rows_.push_back(std::move(row));
    }

    // 生效时机提示（设置由 XiaomiAtvvSession::Options 在会话创建时消费，
    // 热更不重配已连接会话）——SS_LEFT 静态文本；进 label_controls_ 拿
    // WM_CTLCOLORSTATIC 背景处理，Relayout 特判左对齐跨宽。
    {
        hint_label_ = remember_label(CreateSectionTitle(
            hwnd_, TrW(StringId::kRemoteSettingsEffectiveNextConnect, language).c_str(),
            0, 0, ctrl_x + ctrl_w - Dp(10), label_h, instance_));
        Row row;
        row.advance = label_h + Dp(4);
        row.controls = {hint_label_};
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
    for (HWND control : title_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    }
}

void RemoteSettingsDialog::LoadSettingsIntoControls() {
    SetWindowTextW(gain_db_edit_, FormatGain(current_.gain_db).c_str());
    SetWindowTextW(double_click_ms_edit_, std::to_wstring(current_.double_click_ms).c_str());
    Relayout();
}

void RemoteSettingsDialog::RestoreDefaults() {
    current_ = defaults_;
    LoadSettingsIntoControls();
}

void RemoteSettingsDialog::SaveSettings() {
    XiaomiSettings edited = current_;

    // 数字字段：解析失败时回落默认值，越界 clamp 到消费侧允许范围。
    {
        auto text = Utf8(GetEditText(gain_db_edit_));
        try {
            edited.gain_db = std::clamp(std::stod(text), -24.0, 24.0);
        } catch (...) {
            edited.gain_db = defaults_.gain_db;
        }
    }
    {
        auto text = Utf8(GetEditText(double_click_ms_edit_));
        try {
            edited.double_click_ms = std::clamp(std::stoi(text), 200, 600);
        } catch (...) {
            edited.double_click_ms = defaults_.double_click_ms;
        }
    }

    current_ = edited;
    EndDialog(hwnd_, IDOK);
    if (on_settings_changed) {
        // 与全局默认一致 → nullopt（调用方清除覆盖，回落默认）。
        if (current_ == defaults_) {
            on_settings_changed(device_id_, std::nullopt);
        } else {
            on_settings_changed(device_id_, current_);
        }
    }
}

int RemoteSettingsDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

void RemoteSettingsDialog::Relayout() {
    if (!hwnd_) return;
    const int label_w = Dp(200);
    const int ctrl_x = Dp(220);
    const int ctrl_w = Dp(kClientWidth - 250);
    int y = Dp(20);
    for (const auto& row : rows_) {
        for (HWND control : row.controls) {
            if (!control) continue;
            if (control == hint_label_) {
                // 提示行：左对齐，跨标签+控件区宽度（常规字重，不加粗）。
                SetWindowPos(control, nullptr, Dp(10), y,
                             ctrl_x + ctrl_w - Dp(10), Dp(20),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else if (std::find(title_controls_.begin(), title_controls_.end(), control) !=
                       title_controls_.end()) {
                // 分组标题：左对齐，跨标签+控件区宽度。
                SetWindowPos(control, nullptr, Dp(10), y,
                             ctrl_x + ctrl_w - Dp(10), Dp(20),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else if (std::find(label_controls_.begin(), label_controls_.end(), control) !=
                       label_controls_.end()) {
                SetWindowPos(control, nullptr, Dp(10), y + Dp(3), label_w, Dp(20),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                SetWindowPos(control, nullptr, ctrl_x, y, ctrl_w, Dp(24),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        y += row.advance;
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
