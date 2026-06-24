#include "wifi_settings_dialog.h"
#include "dpi_util.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace voicestick {

namespace {

enum : int {
    kIdSsid = 3001,
    kIdPassword,
    kIdShowPassword,
    kIdApply,
    kIdClear,
    kIdRefresh,
    kIdOtaUrl,
    kIdOtaSha,
    kIdOtaStart,
    kIdOtaCommit,
    kIdClose,
};

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
    if (!text) { AppendDialogWord(buffer, 0); return; }
    while (*text) AppendDialogWord(buffer, static_cast<WORD>(*text++));
    AppendDialogWord(buffer, 0);
}

bool IsHttpUrl(std::string_view value) { return value.starts_with("http://"); }

bool IsSha256Hex(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

} // namespace

WifiSettingsDialog::WifiSettingsDialog(HINSTANCE instance, HWND parent, Options options, Callbacks callbacks)
    : instance_(instance), parent_(parent), options_(std::move(options)), callbacks_(std::move(callbacks)) {
    status_ = options_.status;
}

WifiSettingsDialog::~WifiSettingsDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void WifiSettingsDialog::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        return;
    }
    auto tmpl = BuildDialogTemplate();
    hwnd_ = CreateDialogIndirectParamW(instance_, tmpl, parent_, DialogProc, reinterpret_cast<LPARAM>(this));
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
    }
}

void WifiSettingsDialog::UpdateStatus(const WifiStatusSnapshot& status) {
    status_ = status;
    if (hwnd_) RefreshStatusText();
}

INT_PTR CALLBACK WifiSettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<WifiSettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<WifiSettingsDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildControls();
        dialog->LoadInitialValues();
        dialog->RefreshStatusText();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR WifiSettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdApply: OnApplyWifi(); return TRUE;
        case kIdClear: OnClearWifi(); return TRUE;
        case kIdRefresh: if (callbacks_.refresh_status) callbacks_.refresh_status(); return TRUE;
        case kIdOtaStart: OnStartOta(); return TRUE;
        case kIdOtaCommit: OnCommitOta(); return TRUE;
        case kIdShowPassword: ToggleShowPassword(); return TRUE;
        case kIdClose:
        case IDCANCEL:
            DestroyWindow(hwnd_); hwnd_ = nullptr; return TRUE;
        default: break;
        }
        break;
    case WM_DPICHANGED:
        dpi_ = HIWORD(w_param);
        LayoutControls();
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(hwnd_); hwnd_ = nullptr; return TRUE;
    case WM_DESTROY:
        hwnd_ = nullptr; return TRUE;
    }
    return FALSE;
}

LPCDLGTEMPLATE WifiSettingsDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    DLGTEMPLATE tmpl{};
    tmpl.style = WS_POPUP | WS_BORDER | WS_SYSMENU | DS_MODALFRAME | WS_CAPTION;
    tmpl.dwExtendedStyle = 0;
    tmpl.cdit = 0;
    tmpl.x = 0; tmpl.y = 0; tmpl.cx = 560; tmpl.cy = 520;
    AppendDialogData(&dialog_template_, &tmpl, sizeof(tmpl));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"Wi-Fi 与 OTA");
    AlignDialogData(&dialog_template_, 4);
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void WifiSettingsDialog::BuildControls() {
    auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 instance_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    };
    device_label_ = make(L"STATIC", L"", 0, 0);
    ssid_edit_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, kIdSsid);
    password_edit_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, kIdPassword);
    show_password_check_ = make(L"BUTTON", L"显示密码", BS_AUTOCHECKBOX, kIdShowPassword);
    apply_button_ = make(L"BUTTON", L"应用并连接", BS_PUSHBUTTON, kIdApply);
    clear_button_ = make(L"BUTTON", L"清空凭据", BS_PUSHBUTTON, kIdClear);
    refresh_button_ = make(L"BUTTON", L"刷新状态", BS_PUSHBUTTON, kIdRefresh);
    status_label_ = make(L"STATIC", L"", 0, 0);
    ip_label_ = make(L"STATIC", L"", 0, 0);
    error_label_ = make(L"STATIC", L"", 0, 0);
    ota_url_edit_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, kIdOtaUrl);
    ota_sha_edit_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, kIdOtaSha);
    ota_button_ = make(L"BUTTON", L"开始 OTA Pull", BS_PUSHBUTTON, kIdOtaStart);
    commit_button_ = make(L"BUTTON", L"确认健康 (Commit)", BS_PUSHBUTTON, kIdOtaCommit);
    ota_status_label_ = make(L"STATIC", L"", 0, 0);
    close_button_ = make(L"BUTTON", L"关闭", BS_PUSHBUTTON, kIdClose);
    LayoutControls();
}

void WifiSettingsDialog::DestroyControls() {}

void WifiSettingsDialog::LayoutControls() {
    RECT rc{}; GetClientRect(hwnd_, &rc);
    int x = Dp(18), y = Dp(16), w = rc.right - Dp(36);
    auto place = [&](HWND h, int px, int py, int pw, int ph) {
        SetWindowPos(h, nullptr, px, py, pw, ph, SWP_NOZORDER);
    };
    place(device_label_, x, y, w, Dp(24)); y += Dp(34);
    place(CreateWindowExW(0, L"STATIC", L"SSID", WS_CHILD | WS_VISIBLE, x, y+Dp(4), Dp(90), Dp(22), hwnd_, nullptr, instance_, nullptr), x, y+Dp(4), Dp(90), Dp(22));
    place(ssid_edit_, x+Dp(100), y, w-Dp(100), Dp(26)); y += Dp(34);
    place(CreateWindowExW(0, L"STATIC", L"密码", WS_CHILD | WS_VISIBLE, x, y+Dp(4), Dp(90), Dp(22), hwnd_, nullptr, instance_, nullptr), x, y+Dp(4), Dp(90), Dp(22));
    place(password_edit_, x+Dp(100), y, w-Dp(100), Dp(26)); y += Dp(34);
    place(show_password_check_, x+Dp(100), y, Dp(120), Dp(24));
    place(apply_button_, x+Dp(225), y, Dp(110), Dp(28));
    place(clear_button_, x+Dp(345), y, Dp(100), Dp(28));
    place(refresh_button_, x+Dp(455), y, Dp(90), Dp(28)); y += Dp(46);
    place(status_label_, x, y, w, Dp(24)); y += Dp(26);
    place(ip_label_, x, y, w, Dp(24)); y += Dp(26);
    place(error_label_, x, y, w, Dp(24)); y += Dp(38);
    place(CreateWindowExW(0, L"STATIC", L"OTA URL", WS_CHILD | WS_VISIBLE, x, y+Dp(4), Dp(90), Dp(22), hwnd_, nullptr, instance_, nullptr), x, y+Dp(4), Dp(90), Dp(22));
    place(ota_url_edit_, x+Dp(100), y, w-Dp(100), Dp(26)); y += Dp(34);
    place(CreateWindowExW(0, L"STATIC", L"SHA256", WS_CHILD | WS_VISIBLE, x, y+Dp(4), Dp(90), Dp(22), hwnd_, nullptr, instance_, nullptr), x, y+Dp(4), Dp(90), Dp(22));
    place(ota_sha_edit_, x+Dp(100), y, w-Dp(100), Dp(26)); y += Dp(36);
    place(ota_button_, x+Dp(100), y, Dp(120), Dp(30));
    place(commit_button_, x+Dp(230), y, Dp(150), Dp(30)); y += Dp(42);
    place(ota_status_label_, x, y, w, Dp(48));
    place(close_button_, rc.right - Dp(100), rc.bottom - Dp(44), Dp(80), Dp(28));
}

void WifiSettingsDialog::LoadInitialValues() {
    SetText(device_label_, Utf16("设备 VS-" + options_.device_id +
        (options_.firmware_version.empty() ? "" : "  固件 " + options_.firmware_version)));
    SetText(ssid_edit_, Utf16(options_.profile.ssid));
    if (options_.saved_password) SetText(password_edit_, *options_.saved_password);
    SetText(ota_url_edit_, Utf16(options_.profile.ota_url));
    SetText(ota_sha_edit_, Utf16(options_.profile.ota_sha256_hex));
}

void WifiSettingsDialog::RefreshStatusText() {
    WifiStatusSnapshot s = status_.value_or(WifiStatusSnapshot{});
    std::ostringstream line;
    line << "Wi-Fi: " << (s.state.empty() ? "unknown" : s.state);
    if (!s.ssid.empty()) line << "  SSID=" << s.ssid;
    SetText(status_label_, Utf16(line.str()));

    std::ostringstream ip;
    ip << "IP: " << (s.ip.empty() ? "-" : s.ip);
    if (s.rssi.has_value()) ip << "  RSSI=" << *s.rssi << " dBm";
    ip << "  Park=" << (s.park_locked ? "locked" : "busy");
    SetText(ip_label_, Utf16(ip.str()));

    std::string err = s.last_error.empty() ? "最近错误：无" : "最近错误：" + s.last_error;
    SetText(error_label_, Utf16(err));

    std::ostringstream ota;
    ota << "OTA: " << (s.ota_pull_state.empty() ? "idle" : s.ota_pull_state)
        << "  " << s.ota_pull_progress_pct.value_or(0) << "%";
    if (!s.running_partition.empty()) ota << "  分区=" << s.running_partition;
    if (!s.ota_pull_last_error.empty()) ota << "  错误=" << s.ota_pull_last_error;
    if (s.ota_pending_verify) ota << "  等待健康确认";
    SetText(ota_status_label_, Utf16(ota.str()));
    EnableWindow(commit_button_, s.ota_pending_verify ? TRUE : FALSE);
}

void WifiSettingsDialog::OnApplyWifi() {
    auto ssid = Utf8(GetText(ssid_edit_));
    auto password = GetText(password_edit_);
    WifiDeviceProfile profile = options_.profile;
    profile.ssid = ssid;
    profile.ota_url = Utf8(GetText(ota_url_edit_));
    profile.ota_sha256_hex = Utf8(GetText(ota_sha_edit_));
    if (callbacks_.save_profile) callbacks_.save_profile(profile);
    if (callbacks_.apply_wifi) callbacks_.apply_wifi(std::move(ssid), std::move(password));
}

void WifiSettingsDialog::OnClearWifi() {
    if (callbacks_.clear_wifi) callbacks_.clear_wifi();
}

void WifiSettingsDialog::OnStartOta() {
    auto url = Utf8(GetText(ota_url_edit_));
    auto sha = Utf8(GetText(ota_sha_edit_));
    if (IsHttpUrl(url) && !IsSha256Hex(sha)) {
        MessageBoxW(hwnd_, L"HTTP OTA 必须填写 64 位 SHA256。", L"VoiceStick", MB_ICONWARNING);
        return;
    }
    WifiDeviceProfile profile = options_.profile;
    profile.ssid = Utf8(GetText(ssid_edit_));
    profile.ota_url = url;
    profile.ota_sha256_hex = sha;
    if (callbacks_.save_profile) callbacks_.save_profile(profile);
    if (callbacks_.start_ota) callbacks_.start_ota(std::move(url), std::move(sha));
}

void WifiSettingsDialog::OnCommitOta() {
    if (callbacks_.commit_ota) callbacks_.commit_ota();
}

void WifiSettingsDialog::ToggleShowPassword() {
    const bool show = SendMessageW(show_password_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SendMessageW(password_edit_, EM_SETPASSWORDCHAR, show ? 0 : L'●', 0);
    InvalidateRect(password_edit_, nullptr, TRUE);
}

std::wstring WifiSettingsDialog::GetText(HWND control) const {
    int len = GetWindowTextLengthW(control);
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(control, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

void WifiSettingsDialog::SetText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

std::string WifiSettingsDialog::Utf8(const std::wstring& text) const {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring WifiSettingsDialog::Utf16(std::string_view text) const {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

int WifiSettingsDialog::Dp(int px) const { return ScalePx(px, dpi_); }

} // namespace voicestick
