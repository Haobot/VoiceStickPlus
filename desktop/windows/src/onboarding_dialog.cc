#include "onboarding_dialog.h"

#include "dpi_util.h"
#include "localization.h"
#include "voice_stick_cloud_api_win.h"

#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

namespace voicestick {

namespace {

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

HWND CreateStatic(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  HINSTANCE instance, DWORD style = SS_LEFT) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, nullptr, instance, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE instance) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance, nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, int h, UINT id,
                HINSTANCE instance, DWORD extra_style = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance, nullptr);
}

HWND CreateCombo(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE instance) {
    return CreateWindowExW(0, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), instance, nullptr);
}

std::wstring FormatText(std::wstring text, std::initializer_list<std::wstring> values) {
    for (const auto& value : values) {
        const auto pos = text.find(L"%s");
        if (pos == std::wstring::npos) break;
        text.replace(pos, 2, value);
    }
    return text;
}

} // namespace

bool NeedsOnboarding(const AppConfig& config) {
    return config.paired_device_ids.empty() || config.ActiveApiKey().empty();
}

OnboardingDialog::OnboardingDialog(HINSTANCE instance, HWND parent, AppConfig config)
    : instance_(instance), parent_(parent), config_(std::move(config)) {}

OnboardingDialog::~OnboardingDialog() {
    if (hwnd_) EndDialog(hwnd_, IDCANCEL);
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

bool OnboardingDialog::Show() {
    config_ = AppConfig::Load();
    language_ = EffectiveUiLanguage(config_.ui_language);
    const INT_PTR result = DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                                                   OnboardingDialog::DialogProc,
                                                   reinterpret_cast<LPARAM>(this));
    hwnd_ = nullptr;
    controls_.clear();
    status_label_ = nullptr;
    provider_combo_ = nullptr;
    api_key_edit_ = nullptr;
    apply_trial_button_ = nullptr;
    resource_label_ = nullptr;
    resource_combo_ = nullptr;
    back_button_ = nullptr;
    next_button_ = nullptr;
    return result == IDOK;
}

INT_PTR CALLBACK OnboardingDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<OnboardingDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<OnboardingDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->RebuildUi();

        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        RECT work_area = GetWorkAreaForWindow(hwnd);
        const int x = work_area.left + ((work_area.right - work_area.left) -
            (window_rect.right - window_rect.left)) / 2;
        const int y = work_area.top + ((work_area.bottom - work_area.top) -
            (window_rect.bottom - window_rect.top)) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR OnboardingDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdPairDevice:
            if (on_pair_device_requested) on_pair_device_requested();
            config_ = AppConfig::Load();
            RebuildUi();
            return TRUE;
        case kIdProviderCombo:
            if (HIWORD(w_param) == CBN_SELCHANGE) {
                int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
                const std::string& key = [&]() -> const std::string& {
                    switch (ProviderAtComboIndex(idx)) {
                        case AsrProvider::kVoiceStickCloud: return config_.voicestick_api_key;
                        case AsrProvider::kTencent: return config_.tencent_secret_id;
                        default: return config_.volcengine_api_key;
                    }
                }();
                SetWindowTextW(api_key_edit_, Utf16(key).c_str());
                UpdateProviderVisibility();
            }
            return TRUE;
        case kIdApiKeyEdit:
            if (HIWORD(w_param) == EN_CHANGE) UpdateProviderVisibility();
            return TRUE;
        case kIdApplyTrial:
            ApplyTrialApiKey();
            return TRUE;
        case kIdBack:
            GoBack();
            return TRUE;
        case kIdNext:
            GoNext();
            return TRUE;
        case kIdCancel:
            EndDialog(hwnd_, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd_, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        hwnd_ = nullptr;
        controls_.clear();
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE OnboardingDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 380;
    dialog_template.cy = 250;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, TrW(StringId::kOnboardingSetupTitle, language_).c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void OnboardingDialog::RebuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    BuildControls();
}

void OnboardingDialog::DestroyControls() {
    for (HWND control : controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    controls_.clear();
    status_label_ = nullptr;
    provider_combo_ = nullptr;
    api_key_edit_ = nullptr;
    apply_trial_button_ = nullptr;
    resource_label_ = nullptr;
    resource_combo_ = nullptr;
    back_button_ = nullptr;
    next_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void OnboardingDialog::BuildControls() {
    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    auto remember = [&](HWND control) {
        if (control) controls_.push_back(control);
        return control;
    };

    remember(CreateStatic(hwnd_, TrW(StringId::kOnboardingSetupTitle, language_).c_str(),
                          Dp(28), Dp(22), Dp(300), Dp(30), instance_));
    remember(CreateStatic(hwnd_, TrW(StringId::kOnboardingDeviceStep, language_).c_str(),
                          Dp(32), Dp(86), Dp(140), Dp(22), instance_));
    remember(CreateStatic(hwnd_, TrW(StringId::kOnboardingAsrStep, language_).c_str(),
                          Dp(32), Dp(124), Dp(140), Dp(22), instance_));
    remember(CreateStatic(hwnd_, TrW(StringId::kOnboardingReadyStep, language_).c_str(),
                          Dp(32), Dp(162), Dp(140), Dp(22), instance_));

    const int content_x = Dp(210);
    const int content_y = Dp(76);
    const int content_w = Dp(430);
    switch (step_) {
    case Step::kDevice:
        BuildDeviceStep(content_x, content_y, content_w);
        break;
    case Step::kAsr:
        BuildAsrStep(content_x, content_y, content_w);
        break;
    case Step::kReady:
        BuildReadyStep(content_x, content_y, content_w);
        break;
    }

    status_label_ = remember(CreateStatic(hwnd_, L"", Dp(28), Dp(374), Dp(430), Dp(22),
                                          instance_));
    back_button_ = remember(CreateButton(hwnd_, TrW(StringId::kOnboardingBack, language_).c_str(),
                                         Dp(kClientWidth - 250), Dp(400),
                                         Dp(76), Dp(30), kIdBack, instance_));
    next_button_ = remember(CreateButton(hwnd_, step_ == Step::kReady
                                             ? TrW(StringId::kOnboardingFinish, language_).c_str()
                                             : TrW(StringId::kOnboardingNext, language_).c_str(),
                                         Dp(kClientWidth - 164), Dp(400),
                                         Dp(76), Dp(30), kIdNext, instance_));
    remember(CreateButton(hwnd_, TrW(StringId::kCancel, language_).c_str(),
                          Dp(kClientWidth - 78), Dp(400),
                          Dp(60), Dp(30), kIdCancel, instance_));

    EnableWindow(back_button_, step_ != Step::kDevice);
    for (HWND control : controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (step_ == Step::kDevice && HasDevice()) {
        SetStatus(TrW(StringId::kOnboardingDevicePairedContinue, language_));
    }
    if (step_ == Step::kAsr) LoadConfigIntoControls();
}

void OnboardingDialog::BuildDeviceStep(int x, int y, int w) {
    controls_.push_back(CreateStatic(hwnd_, TrW(StringId::kOnboardingPairDeviceTitle, language_).c_str(),
                                     x, y, w, Dp(28), instance_));
    controls_.push_back(CreateStatic(hwnd_, DeviceSummary().c_str(), x, y + Dp(42), w, Dp(24),
                                     instance_));
    controls_.push_back(CreateStatic(hwnd_,
        TrW(StringId::kOnboardingPairDeviceDescription, language_).c_str(),
        x, y + Dp(76), w, Dp(40), instance_));
    controls_.push_back(CreateButton(hwnd_, HasDevice()
                                                 ? TrW(StringId::kOnboardingPairAnotherDevice, language_).c_str()
                                                 : TrW(StringId::kOnboardingPairDeviceButton, language_).c_str(),
                                     x, y + Dp(132), Dp(160), Dp(30),
                                     kIdPairDevice, instance_));
}

void OnboardingDialog::BuildAsrStep(int x, int y, int w) {
    controls_.push_back(CreateStatic(hwnd_, TrW(StringId::kOnboardingChooseAsr, language_).c_str(),
                                     x, y, w, Dp(24), instance_));
    controls_.push_back(CreateStatic(hwnd_, TrW(StringId::kOnboardingProvider, language_).c_str(),
                                     x, y + Dp(48), Dp(92), Dp(22),
                                     instance_, SS_RIGHT));
    provider_combo_ = CreateCombo(hwnd_, x + Dp(104), y + Dp(44), w - Dp(104),
                                  Dp(200), kIdProviderCombo, instance_);
    controls_.push_back(provider_combo_);
    // VoiceStick Cloud 已从下拉框下线；仅老配置仍为 cloud 时临时插入该项（0 号位）。
    provider_combo_has_cloud_ = (config_.asr_provider == AsrProvider::kVoiceStickCloud);
    if (provider_combo_has_cloud_) {
        SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"VoiceStick Cloud"));
    }
    SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Volcengine"));
    SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tencent Cloud ASR"));

    controls_.push_back(CreateStatic(hwnd_, TrW(StringId::kOnboardingApiKey, language_).c_str(),
                                     x, y + Dp(88), Dp(92), Dp(22),
                                     instance_, SS_RIGHT));
    api_key_edit_ = CreateEdit(hwnd_, x + Dp(104), y + Dp(84), w - Dp(222), Dp(24),
                               kIdApiKeyEdit, instance_, ES_PASSWORD);
    controls_.push_back(api_key_edit_);
    apply_trial_button_ = CreateButton(hwnd_, TrW(StringId::kSettingsApplyTrial, language_).c_str(),
                                       x + w - Dp(108),
                                       y + Dp(84), Dp(108), Dp(24),
                                       kIdApplyTrial, instance_);
    controls_.push_back(apply_trial_button_);

    resource_label_ = CreateStatic(hwnd_, TrW(StringId::kOnboardingResource, language_).c_str(),
                                   x, y + Dp(128), Dp(92), Dp(22),
                                   instance_, SS_RIGHT);
    controls_.push_back(resource_label_);
    resource_combo_ = CreateCombo(hwnd_, x + Dp(104), y + Dp(124), w - Dp(104),
                                  Dp(200), kIdResourceCombo, instance_);
    controls_.push_back(resource_combo_);
    for (const auto& id : AppConfig::SupportedResourceIds()) {
        SendMessageW(resource_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf16(id).c_str()));
    }
}

void OnboardingDialog::BuildReadyStep(int x, int y, int w) {
    controls_.push_back(CreateStatic(hwnd_, TrW(StringId::kOnboardingReadyTitle, language_).c_str(),
                                     x, y, w, Dp(28), instance_));
    controls_.push_back(CreateStatic(hwnd_, DeviceSummary().c_str(), x, y + Dp(46), w, Dp(24),
                                     instance_));
    const auto provider_name = [&]() -> const wchar_t* {
        switch (config_.asr_provider) {
            case AsrProvider::kVoiceStickCloud: return L"VoiceStick Cloud";
            case AsrProvider::kVolcengine: return L"Volcengine";
            case AsrProvider::kTencent: return L"Tencent Cloud ASR";
        }
        return L"VoiceStick Cloud";
    }();
    const auto provider = FormatText(TrW(StringId::kOnboardingAsrSummary, language_), {provider_name});
    controls_.push_back(CreateStatic(hwnd_, provider.c_str(), x, y + Dp(82), w, Dp(24), instance_));
    controls_.push_back(CreateStatic(hwnd_,
        TrW(StringId::kOnboardingReadyInstruction, language_).c_str(),
        x, y + Dp(124), w, Dp(40), instance_));
}

void OnboardingDialog::LoadConfigIntoControls() {
    if (!provider_combo_) return;
    SendMessageW(provider_combo_, CB_SETCURSEL, ComboIndexForProvider(config_.asr_provider), 0);
    const auto& key = [&]() -> const std::string& {
        switch (config_.asr_provider) {
            case AsrProvider::kVoiceStickCloud: return config_.voicestick_api_key;
            case AsrProvider::kVolcengine: return config_.volcengine_api_key;
            case AsrProvider::kTencent: return config_.tencent_secret_id;
        }
        return config_.voicestick_api_key;
    }();
    SetWindowTextW(api_key_edit_, Utf16(key).c_str());
    const auto resource = Utf16(config_.resource_id);
    int idx = static_cast<int>(SendMessageW(resource_combo_, CB_FINDSTRINGEXACT, -1,
                                            reinterpret_cast<LPARAM>(resource.c_str())));
    SendMessageW(resource_combo_, CB_SETCURSEL, idx >= 0 ? idx : 0, 0);
    UpdateProviderVisibility();
}

void OnboardingDialog::SaveControlsIntoConfig() {
    if (!provider_combo_) return;
    const int provider_idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    AsrProvider new_provider = ProviderAtComboIndex(provider_idx);
    config_.asr_provider = new_provider;
    const auto api_key = Utf8(GetText(api_key_edit_));
    switch (new_provider) {
        case AsrProvider::kVoiceStickCloud: config_.voicestick_api_key = api_key; break;
        case AsrProvider::kVolcengine: config_.volcengine_api_key = api_key; break;
        case AsrProvider::kTencent: config_.tencent_secret_id = api_key; break;
    }
    wchar_t resource_buf[256]{};
    const int resource_idx = static_cast<int>(SendMessageW(resource_combo_, CB_GETCURSEL, 0, 0));
    if (resource_idx >= 0) {
        SendMessageW(resource_combo_, CB_GETLBTEXT, resource_idx,
                     reinterpret_cast<LPARAM>(resource_buf));
        config_.resource_id = Utf8(resource_buf);
    }
}

AsrProvider OnboardingDialog::ProviderAtComboIndex(int idx) const {
    if (provider_combo_has_cloud_) {
        if (idx == 0) return AsrProvider::kVoiceStickCloud;
        --idx;
    }
    return idx == 1 ? AsrProvider::kTencent : AsrProvider::kVolcengine;
}

int OnboardingDialog::ComboIndexForProvider(AsrProvider provider) const {
    if (provider == AsrProvider::kVoiceStickCloud) return 0;  // 仅 has_cloud 时存在
    const int idx = (provider == AsrProvider::kTencent) ? 1 : 0;
    return provider_combo_has_cloud_ ? idx + 1 : idx;
}

void OnboardingDialog::UpdateProviderVisibility() {
    const int provider_idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    const AsrProvider provider = ProviderAtComboIndex(provider_idx);
    const bool is_cloud = provider == AsrProvider::kVoiceStickCloud;
    const bool is_volcengine = provider == AsrProvider::kVolcengine;
    const bool api_key_empty = GetText(api_key_edit_).empty();
    // 资源 ID 仅火山引擎使用；VoiceStick Cloud 与腾讯云均不显示。
    ShowWindow(resource_label_, is_volcengine ? SW_SHOW : SW_HIDE);
    ShowWindow(resource_combo_, is_volcengine ? SW_SHOW : SW_HIDE);
    ShowWindow(apply_trial_button_, is_cloud && api_key_empty ? SW_SHOW : SW_HIDE);
    const int full_w = Dp(430 - 104);
    const int api_w = is_cloud && api_key_empty ? full_w - Dp(116) : full_w;
    SetWindowPos(api_key_edit_, nullptr, 0, 0, api_w, Dp(24),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void OnboardingDialog::ApplyTrialApiKey() {
    SaveControlsIntoConfig();
    if (config_.asr_provider != AsrProvider::kVoiceStickCloud) return;
    SetStatus(TrW(StringId::kOnboardingApplyingTrial, language_));
    EnableWindow(apply_trial_button_, FALSE);
    UpdateWindow(hwnd_);
    const auto device_id = config_.paired_device_ids.empty() ? std::string() : config_.paired_device_ids.front();
    auto result = ApplyVoiceStickCloudTrialApiKey(config_.voicestick_cloud_url, device_id);
    EnableWindow(apply_trial_button_, TRUE);
    if (!result.api_key.empty()) {
        config_.voicestick_api_key = result.api_key;
        SetWindowTextW(api_key_edit_, Utf16(result.api_key).c_str());
        SetStatus(TrW(StringId::kOnboardingTrialApplied, language_));
        UpdateProviderVisibility();
        return;
    }
    if (!result.url.empty()) {
        const auto wide_url = Utf16(result.url);
        auto* shell_result = ShellExecuteW(hwnd_, L"open", wide_url.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(shell_result) <= 32) {
            SetStatus(TrW(StringId::kOnboardingTrialOpenFailed, language_));
            UpdateProviderVisibility();
            return;
        }
        SetStatus(TrW(StringId::kOnboardingTrialPageOpened, language_));
        UpdateProviderVisibility();
        return;
    }
    SetStatus(result.error.empty()
                  ? TrW(StringId::kOnboardingTrialApplyFailed, language_)
                  : Utf16(result.error));
    UpdateProviderVisibility();
}

void OnboardingDialog::GoBack() {
    if (step_ == Step::kAsr) step_ = Step::kDevice;
    else if (step_ == Step::kReady) step_ = Step::kAsr;
    RebuildUi();
}

void OnboardingDialog::GoNext() {
    if (step_ == Step::kDevice) {
        config_ = AppConfig::Load();
        if (!HasDevice()) {
            SetStatus(TrW(StringId::kOnboardingPairDeviceFirst, language_));
            return;
        }
        // 内置 key（ActiveApiKey 非空）时跳过 kAsr 步直接进入完成步；公开版仍走 kAsr。
        step_ = NeedsAsrStep(config_) ? Step::kAsr : Step::kReady;
        RebuildUi();
        return;
    }
    if (step_ == Step::kAsr) {
        SaveControlsIntoConfig();
        if (!HasApiKey()) {
            SetStatus(TrW(StringId::kOnboardingEnterApiKey, language_));
            return;
        }
        step_ = Step::kReady;
        RebuildUi();
        return;
    }
    Finish();
}

void OnboardingDialog::Finish() {
    SaveControlsIntoConfig();
    config_.Save();
    if (on_config_completed) on_config_completed(config_);
    EndDialog(hwnd_, IDOK);
}

void OnboardingDialog::SetStatus(const std::wstring& text) {
    if (status_label_) SetWindowTextW(status_label_, text.c_str());
}

bool OnboardingDialog::HasDevice() const {
    return !config_.paired_device_ids.empty();
}

bool OnboardingDialog::HasApiKey() const {
    return !config_.ActiveApiKey().empty();
}

std::wstring OnboardingDialog::DeviceSummary() const {
    if (config_.paired_device_ids.empty()) return TrW(StringId::kOnboardingDeviceNotPaired, language_);
    return FormatText(TrW(StringId::kOnboardingDeviceSummary, language_),
                      {Utf16(config_.paired_device_ids.front())});
}

std::wstring OnboardingDialog::Utf16(const std::string& text) const {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

std::string OnboardingDialog::Utf8(const std::wstring& text) const {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring OnboardingDialog::GetText(HWND control) const {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int OnboardingDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

} // namespace voicestick
