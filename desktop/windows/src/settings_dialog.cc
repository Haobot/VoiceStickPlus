#include "settings_dialog.h"
#include "dpi_util.h"
#include "llm_refinement_client.h"
#include "localization.h"
#include "log.h"
#include "voice_stick_cloud_api_win.h"

#include <ShlObj.h>
#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <iterator>
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

std::wstring GetWindowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

std::string JoinHotwords(const std::vector<std::string>& hotwords) {
    std::string text;
    for (const auto& hotword : hotwords) {
        if (!text.empty()) text += "\r\n";
        text += hotword;
    }
    return text;
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateLeftLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst,
                DWORD extra_style = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

HWND CreateMultilineEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                               ES_AUTOVSCROLL | ES_WANTRETURN,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
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

// 分组标题：左对齐静态文本，由调用方记入 title_controls_ 套用加粗字体。
HWND CreateSectionTitle(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

// 组间水平分隔线：SS_ETCHEDHORZ 自绘背景，不应被 WM_CTLCOLORSTATIC 设为透明。
HWND CreateSeparator(HWND parent, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

} // namespace

SettingsDialog::SettingsDialog(HINSTANCE instance, HWND parent, AppConfig config)
    : instance_(instance), parent_(parent), config_(std::move(config)) {}

SettingsDialog::~SettingsDialog() {
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

void SettingsDialog::Show() {
    config_ = AppConfig::Load();

    if (hwnd_) {
        LoadConfigIntoControls();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }

    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            SettingsDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK SettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<SettingsDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);

        dialog->RebuildUi();

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

INT_PTR SettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdSave:
            SaveSettings();
            return TRUE;
        case kIdCancel:
            EndDialog(hwnd_, IDCANCEL);
            return TRUE;
        case kIdChooseDir:
            ChooseDebugDirectory();
            return TRUE;
        case kIdApplyTrialApiKey:
            ApplyTrialApiKey();
            return TRUE;
        case kIdProviderCombo:
            if (HIWORD(w_param) == CBN_SELCHANGE) {
                int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
                const std::string& key = [&]() -> const std::string& {
                    switch (idx) {
                        case 0: return config_.voicestick_api_key;
                        case 2: return config_.tencent_secret_id;
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
        case kIdRefineText:
            if (HIWORD(w_param) == BN_CLICKED) UpdateRefinePromptVisibility();
            return TRUE;
        case kIdOutputTarget:
            if (HIWORD(w_param) == CBN_SELCHANGE) UpdateOutputTargetVisibility();
            return TRUE;
        case kIdTriggerModeHold:
        case kIdTriggerModeClick:
            if (HIWORD(w_param) == BN_CLICKED) OnTriggerModeChanged();
            return TRUE;
        }
        break;
    case WM_HSCROLL:
        // 滑块拖动时实时刷新右侧档位数值。
        if (reinterpret_cast<HWND>(l_param) == tap_sensitivity_trackbar_) {
            UpdateTapSensitivityLabel();
        } else if (reinterpret_cast<HWND>(l_param) == air_mouse_sensitivity_x_trackbar_) {
            UpdateAirMouseSensitivityXLabel();
        } else if (reinterpret_cast<HWND>(l_param) == air_mouse_sensitivity_y_trackbar_) {
            UpdateAirMouseSensitivityYLabel();
        }
        return TRUE;
    case WM_VSCROLL: {
        // 垂直滚动条/滚轮：更新 scroll_pos 后重定位所有控件。
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        const int prev = si.nPos;
        switch (LOWORD(w_param)) {
            case SB_LINEUP:         si.nPos -= Dp(20); break;
            case SB_LINEDOWN:       si.nPos += Dp(20); break;
            case SB_PAGEUP:         si.nPos -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN:       si.nPos += static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:  si.nPos = si.nTrackPos; break;
            case SB_TOP:            si.nPos = si.nMin; break;
            case SB_BOTTOM:         si.nPos = si.nMax; break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd_, SB_VERT, &si);
        if (si.nPos != prev) {
            scroll_pos_ = si.nPos;
            Relayout();
        }
        return TRUE;
    }
    case WM_MOUSEWHEEL: {
        // 滚轮：delta 转像素，正值=向前滚=内容上移=scroll_pos 减少。
        const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
        UINT wheel_lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheel_lines, 0);
        const int dy = (delta * static_cast<int>(wheel_lines) * Dp(38)) / WHEEL_DELTA;
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        const int prev = si.nPos;
        si.nPos -= dy;
        SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd_, SB_VERT, &si);
        if (si.nPos != prev) {
            scroll_pos_ = si.nPos;
            Relayout();
        }
        return TRUE;
    }
    case WM_CLOSE:
        EndDialog(hwnd_, IDCANCEL);
        return TRUE;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(w_param);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            auto* rect = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RebuildUi();
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const auto control = reinterpret_cast<HWND>(l_param);
        if (IsLabelControl(control)) {
            auto dc = reinterpret_cast<HDC>(w_param);
            // 不透明 BTNFACE 背景：标签自擦除旧位置，滚动 SetWindowPos 重定位时
            // 无透明 STATIC 文本残留，无需全量 RedrawWindow。
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        provider_combo_ = nullptr;
        api_key_edit_ = nullptr;
        apply_trial_button_ = nullptr;
        resource_combo_ = nullptr;
        hotwords_edit_ = nullptr;
        llm_base_url_edit_ = nullptr;
        llm_api_key_edit_ = nullptr;
        llm_model_edit_ = nullptr;
        launch_at_login_check_ = nullptr;
        refine_prompt_label_ = nullptr;
        refine_prompt_edit_ = nullptr;
        debug_audio_check_ = nullptr;
        show_imu_debug_check_ = nullptr;
        imu_wake_sensitivity_combo_ = nullptr;
        tap_to_arrow_check_ = nullptr;
        debug_dir_edit_ = nullptr;
        resource_label_ = nullptr;
        output_target_combo_ = nullptr;
        wechat_hotkey_edit_ = nullptr;
        wechat_hotkey_label_ = nullptr;
        trigger_mode_label_ = nullptr;
        trigger_mode_hold_radio_ = nullptr;
        trigger_mode_click_radio_ = nullptr;
        all_controls_.clear();
        label_controls_.clear();
        title_controls_.clear();
        layout_.clear();
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE SettingsDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    // WS_CLIPCHILDREN：父窗口重绘时排除子窗口区域，避免擦除背景覆盖按钮等子控件
    // （DWM 合成下缺该项时滚动会令按钮区域被对话框背景覆盖且按钮不重绘，表现为按钮消失）。
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VSCROLL | WS_CLIPCHILDREN;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 300;
    dialog_template.cy = 210;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"VoiceStick Settings");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void SettingsDialog::RebuildUi() {
    DestroyControls();
    // 重新创建控件时滚动位置归零，从顶部开始。
    scroll_pos_ = 0;

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    BuildControls();
    LoadConfigIntoControls();
}

void SettingsDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    label_controls_.clear();
    title_controls_.clear();
    layout_.clear();
    provider_combo_ = nullptr;
    api_key_edit_ = nullptr;
    apply_trial_button_ = nullptr;
    resource_combo_ = nullptr;
    hotwords_edit_ = nullptr;
    llm_base_url_edit_ = nullptr;
    llm_api_key_edit_ = nullptr;
    llm_model_edit_ = nullptr;
    refine_prompt_label_ = nullptr;
    refine_prompt_edit_ = nullptr;
    debug_audio_check_ = nullptr;
    show_imu_debug_check_ = nullptr;
    imu_wake_sensitivity_combo_ = nullptr;
    tap_to_arrow_check_ = nullptr;
    tap_sensitivity_trackbar_ = nullptr;
    tap_sensitivity_value_label_ = nullptr;
    air_mouse_sensitivity_x_trackbar_ = nullptr;
    air_mouse_sensitivity_x_value_label_ = nullptr;
    air_mouse_sensitivity_y_trackbar_ = nullptr;
    air_mouse_sensitivity_y_value_label_ = nullptr;
    debug_dir_edit_ = nullptr;
    resource_label_ = nullptr;
    output_target_combo_ = nullptr;
    wechat_hotkey_edit_ = nullptr;
    wechat_hotkey_label_ = nullptr;
    trigger_mode_label_ = nullptr;
    trigger_mode_hold_radio_ = nullptr;
    trigger_mode_click_radio_ = nullptr;
    save_button_ = nullptr;
    cancel_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
    if (title_font_) {
        DeleteObject(title_font_);
        title_font_ = nullptr;
    }
}

void SettingsDialog::BuildControls() {
    // 注册 trackbar (滑块) 控件类，供敲击灵敏度 1~10 档使用。
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    ui_font_ = CreateUiFont(dpi_);
    title_font_ = CreateUiFontBold(dpi_);
    const HFONT font = ui_font_;
    const HFONT title_font = title_font_;

    auto remember = [&](HWND control) {
        if (control) all_controls_.push_back(control);
        return control;
    };
    auto remember_label = [&](HWND control) {
        if (control) label_controls_.push_back(control);
        return remember(control);
    };
    // 分组标题同时记入 title_controls_，在字体应用阶段套用加粗字体。
    auto remember_title = [&](HWND control) {
        if (control) title_controls_.push_back(control);
        return remember_label(control);
    };

    // 标签列缩窄到 180 Dp：删去体感鼠标/灵敏度描述后最长标签（语音热键 / Voice
    // Hotkey）约 142 Dp，180 留余量。控件左移到 ctrl_x=200、ctrl_w 加宽到 kClientWidth-230=350，
    // 减少复选框行左侧空白；内容右边界 x=550（右边距 30）。
    const int label_w = Dp(180);
    const int ctrl_x = Dp(200);
    const int ctrl_w = Dp(kClientWidth - 230);
    const int row_h = Dp(28);
    const int title_h = Dp(20);
    const int sep_h = Dp(2);
    const UiLanguage language = EffectiveUiLanguage(config_.ui_language);
    SetWindowTextW(hwnd_, TrW(StringId::kSettingsTitle, language).c_str());

    auto label_text = [&](StringId id) {
        return TrW(id, language) + L":";
    };

    // 注册一个布局条目（一行或一个多行块），含可见性谓词（空=始终可见）。
    auto add = [&](int advance, std::vector<LayoutPart> parts,
                   std::function<bool()> vis = std::function<bool()>()) {
        layout_.push_back({advance, std::move(parts), std::move(vis)});
    };
    // 分组标题：左对齐加粗文本，宽度与“标签+控件”区域对齐。
    auto section_title = [&](StringId id) {
        HWND t = remember_title(CreateSectionTitle(hwnd_, TrW(id, language).c_str(),
                                                    0, 0, ctrl_x + ctrl_w - Dp(10), title_h, instance_));
        add(title_h + Dp(4), {{t, Dp(10), 0, ctrl_x + ctrl_w - Dp(10), title_h}});
    };
    // 组间分隔线：宽度同标题，推进为线高 + 下方间距。
    auto separator = [&]() {
        HWND s = remember(CreateSeparator(hwnd_, 0, 0, ctrl_x + ctrl_w - Dp(10), sep_h, instance_));
        add(sep_h + Dp(12), {{s, Dp(10), 0, ctrl_x + ctrl_w - Dp(10), sep_h}});
    };

    // ===== 通用 =====
    section_title(StringId::kSettingsSectionGeneral);
    {
        HWND lang_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsLanguage).c_str(),
                                                     0, 0, label_w, Dp(20), instance_));
        language_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(140),
                                                kIdLanguageCombo, instance_));
        SendMessageW(language_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsLanguageSystem, language).c_str()));
        SendMessageW(language_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsLanguageEnglish, language).c_str()));
        SendMessageW(language_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsLanguageChineseSimplified, language).c_str()));
        add(row_h + Dp(10), {
            {lang_label, Dp(10), Dp(3), label_w, Dp(20)},
            {language_combo_, ctrl_x, 0, ctrl_w, Dp(140)},
        });
    }
    separator();

    // ===== 语音识别 =====
    section_title(StringId::kSettingsSectionAsr);
    {
        HWND prov_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsProvider).c_str(),
                                                     0, 0, label_w, Dp(20), instance_));
        provider_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(200),
                                               kIdProviderCombo, instance_));
        SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"VoiceStick Cloud"));
        SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Volcengine"));
        SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tencent Cloud ASR"));
        add(row_h + Dp(10), {
            {prov_label, Dp(10), Dp(3), label_w, Dp(20)},
            {provider_combo_, ctrl_x, 0, ctrl_w, Dp(200)},
        });
    }
    {
        HWND api_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsApiKey).c_str(),
                                                    0, 0, label_w, Dp(20), instance_));
        const int apply_btn_w = Dp(102);
        // api_key_edit 宽度先按占满注册，实际宽度由 ApplyApiKeyLayout 在 Relayout 末尾按
        // apply_trial_button 显隐动态调整。
        api_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                            kIdApiKeyEdit, instance_, ES_PASSWORD));
        apply_trial_button_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsApplyTrial, language).c_str(),
                                                    0, 0, apply_btn_w, Dp(24),
                                                    kIdApplyTrialApiKey, instance_));
        add(row_h + Dp(10), {
            {api_label, Dp(10), Dp(3), label_w, Dp(20)},
            {api_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
            // apply_trial_button 行内条件：defer_visibility 让显隐交给 ApplyApiKeyLayout。
            {apply_trial_button_, ctrl_x + ctrl_w - apply_btn_w, 0, apply_btn_w, Dp(24), true},
        });
    }
    {
        // 资源 ID 行：仅 Volcengine 显示，隐藏时不占位。
        resource_label_ = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsResourceId).c_str(),
                                                     0, 0, label_w, Dp(20), instance_));
        resource_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(200),
                                               kIdResourceCombo, instance_));
        for (const auto& id : AppConfig::SupportedResourceIds()) {
            SendMessageW(resource_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(Utf16(id).c_str()));
        }
        add(row_h + Dp(10), {
            {resource_label_, Dp(10), Dp(3), label_w, Dp(20)},
            {resource_combo_, ctrl_x, 0, ctrl_w, Dp(200)},
        }, [this]() {
            int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
            return idx == 1;  // Volcengine
        });
    }
    {
        // 热词块：label + 多行 edit + 提示行，作为一个整体推进。
        HWND hot_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsHotwords).c_str(),
                                                    0, 0, label_w, Dp(20), instance_));
        hotwords_edit_ = remember(CreateMultilineEdit(hwnd_, 0, 0, ctrl_w, Dp(74),
                                                      kIdHotwordsEdit, instance_));
        HWND hot_hint = remember_label(CreateLeftLabel(hwnd_, TrW(StringId::kSettingsHotwordsHint, language).c_str(),
                                                       0, 0, ctrl_w, Dp(16), instance_));
        add(Dp(80) + Dp(26), {
            {hot_label, Dp(10), Dp(3), label_w, Dp(20)},
            {hotwords_edit_, ctrl_x, 0, ctrl_w, Dp(74)},
            {hot_hint, ctrl_x, Dp(80), ctrl_w, Dp(16)},
        });
    }
    separator();

    // ===== 文本精修 =====
    section_title(StringId::kSettingsSectionRefine);
    {
        HWND bu_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsLlmBaseUrl).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        llm_base_url_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                 kIdLlmBaseUrlEdit, instance_));
        add(row_h + Dp(10), {
            {bu_label, Dp(10), Dp(3), label_w, Dp(20)},
            {llm_base_url_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        HWND lak_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsLlmApiKey).c_str(),
                                                    0, 0, label_w, Dp(20), instance_));
        llm_api_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                kIdLlmApiKeyEdit, instance_, ES_PASSWORD));
        add(row_h + Dp(10), {
            {lak_label, Dp(10), Dp(3), label_w, Dp(20)},
            {llm_api_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        HWND lm_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsLlmModel).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        llm_model_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                              kIdLlmModelEdit, instance_));
        add(row_h + Dp(10), {
            {lm_label, Dp(10), Dp(3), label_w, Dp(20)},
            {llm_model_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        HWND refine_lbl = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        refine_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsRefineText, language).c_str(),
                                               0, 0, ctrl_w, Dp(22), kIdRefineText, instance_,
                                               BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {refine_lbl, Dp(10), Dp(3), label_w, Dp(20)},
            {refine_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        // 精修提示词块：仅 refine_check 勾选时显示，隐藏时不占位。
        refine_prompt_label_ = remember_label(CreateLabel(hwnd_,
            label_text(StringId::kSettingsRefinePrompt).c_str(),
            0, 0, label_w, Dp(20), instance_));
        refine_prompt_edit_ = remember(CreateMultilineEdit(hwnd_, 0, 0, ctrl_w, Dp(64),
                                                           kIdRefinePromptEdit, instance_));
        add(Dp(70), {
            {refine_prompt_label_, Dp(10), Dp(3), label_w, Dp(20)},
            {refine_prompt_edit_, ctrl_x, 0, ctrl_w, Dp(64)},
        }, [this]() {
            return SendMessageW(refine_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        });
    }
    separator();

    // ===== 输出 =====
    section_title(StringId::kSettingsSectionOutput);
    {
        // 输出目标：当前应用 / 字幕 / 第三方输入法。
        HWND ot_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsOutputTarget).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        output_target_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(200),
                                                    kIdOutputTarget, instance_));
        SendMessageW(output_target_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsOutputTargetFocusedApp, language).c_str()));
        SendMessageW(output_target_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsOutputTargetSubtitle, language).c_str()));
        SendMessageW(output_target_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsOutputTargetWechatInputMethod, language).c_str()));
        add(row_h + Dp(10), {
            {ot_label, Dp(10), Dp(3), label_w, Dp(20)},
            {output_target_combo_, ctrl_x, 0, ctrl_w, Dp(200)},
        });
    }
    {
        // 第三方输入法：语音热键（与第三方输入法设置中保持一致，如 ctrl+win）。
        // 仅输出目标=第三方输入法时显示，隐藏时不占位。
        wechat_hotkey_label_ = remember_label(CreateLabel(hwnd_,
            label_text(StringId::kSettingsWechatHotkey).c_str(),
            0, 0, label_w, Dp(20), instance_));
        wechat_hotkey_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                  kIdWechatHotkey, instance_));
        add(row_h + Dp(10), {
            {wechat_hotkey_label_, Dp(10), Dp(3), label_w, Dp(20)},
            {wechat_hotkey_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        }, [this]() {
            int idx = static_cast<int>(SendMessageW(output_target_combo_, CB_GETCURSEL, 0, 0));
            return idx == 2;  // 第三方输入法
        });
    }
    {
        // 触发方式：长按式=hold_to_talk，点按式联动全局 interaction_mode=click_to_talk。
        // Typeless 等点按式输入法靠右ALT(ralt)单键 toggle 录音开关，仅第三方输入法模式显示。
        trigger_mode_label_ = remember_label(CreateLabel(hwnd_,
            label_text(StringId::kSettingsTriggerMode).c_str(),
            0, 0, label_w, Dp(20), instance_));
        const int hold_w = Dp(180);
        const int click_w = Dp(160);
        trigger_mode_hold_radio_ = remember(CreateButton(hwnd_,
            TrW(StringId::kSettingsTriggerModeHold, language).c_str(),
            0, 0, hold_w, Dp(20), kIdTriggerModeHold, instance_,
            BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP));
        trigger_mode_click_radio_ = remember(CreateButton(hwnd_,
            TrW(StringId::kSettingsTriggerModeClick, language).c_str(),
            0, 0, click_w, Dp(20), kIdTriggerModeClick, instance_,
            BS_AUTORADIOBUTTON));
        add(row_h + Dp(10), {
            {trigger_mode_label_, Dp(10), Dp(3), label_w, Dp(20)},
            {trigger_mode_hold_radio_, ctrl_x, 0, hold_w, Dp(20)},
            {trigger_mode_click_radio_, ctrl_x + hold_w + Dp(10), 0, click_w, Dp(20)},
        }, [this]() {
            int idx = static_cast<int>(SendMessageW(output_target_combo_, CB_GETCURSEL, 0, 0));
            return idx == 2;  // 第三方输入法
        });
    }
    separator();

    // ===== 设备交互 =====
    section_title(StringId::kSettingsSectionDevice);
    {
        HWND iw_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsImuWakeSensitivity).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        imu_wake_sensitivity_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(120),
                                                           kIdImuWakeSensitivity, instance_));
        SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityLow, language).c_str()));
        SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityMedium, language).c_str()));
        SendMessageW(imu_wake_sensitivity_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsImuWakeSensitivityHigh, language).c_str()));
        add(row_h + Dp(10), {
            {iw_label, Dp(10), Dp(3), label_w, Dp(20)},
            {imu_wake_sensitivity_combo_, ctrl_x, 0, ctrl_w, Dp(120)},
        });
    }
    {
        HWND tta_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        tap_to_arrow_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsTapToArrow, language).c_str(),
                                                     0, 0, ctrl_w, Dp(22), kIdTapToArrow, instance_,
                                                     BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {tta_label, Dp(10), Dp(3), label_w, Dp(20)},
            {tap_to_arrow_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        // 敲击灵敏度 1~10 档滑块：1=最不灵敏（需大力敲），10=最灵敏（轻触即发）。
        HWND ts_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsTapSensitivity).c_str(),
                                                    0, 0, label_w, Dp(20), instance_));
        tap_sensitivity_trackbar_ = remember(CreateTrackbar(hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28),
                                                            kIdTapSensitivity, instance_));
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(tap_sensitivity_trackbar_, TBM_SETPAGESIZE, 0, 1);
        // 右侧静态文本实时显示当前档位数值。
        tap_sensitivity_value_label_ = remember(CreateLeftLabel(hwnd_, L"5", 0, 0, Dp(30), Dp(20), instance_));
        add(row_h + Dp(10), {
            {ts_label, Dp(10), Dp(3), label_w, Dp(20)},
            {tap_sensitivity_trackbar_, ctrl_x, 0, ctrl_w - Dp(50), Dp(28)},
            {tap_sensitivity_value_label_, ctrl_x + ctrl_w - Dp(40), Dp(5), Dp(30), Dp(20)},
        });
    }
    {
        // 体感鼠标左右灵敏度 1~10 档滑块（yaw）。
        HWND ax_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsAirMouseSensitivityX).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        air_mouse_sensitivity_x_trackbar_ = remember(CreateTrackbar(hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28),
                                                                    kIdAirMouseSensitivityX, instance_));
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETPAGESIZE, 0, 1);
        air_mouse_sensitivity_x_value_label_ = remember(CreateLeftLabel(hwnd_, L"5", 0, 0, Dp(30), Dp(20), instance_));
        add(row_h + Dp(10), {
            {ax_label, Dp(10), Dp(3), label_w, Dp(20)},
            {air_mouse_sensitivity_x_trackbar_, ctrl_x, 0, ctrl_w - Dp(50), Dp(28)},
            {air_mouse_sensitivity_x_value_label_, ctrl_x + ctrl_w - Dp(40), Dp(5), Dp(30), Dp(20)},
        });
    }
    {
        // 体感鼠标上下灵敏度 1~10 档滑块（pitch）。
        HWND ay_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsAirMouseSensitivityY).c_str(),
                                                   0, 0, label_w, Dp(20), instance_));
        air_mouse_sensitivity_y_trackbar_ = remember(CreateTrackbar(hwnd_, 0, 0, ctrl_w - Dp(50), Dp(28),
                                                                    kIdAirMouseSensitivityY, instance_));
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETRANGEMAX, TRUE, 10);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETPAGESIZE, 0, 1);
        air_mouse_sensitivity_y_value_label_ = remember(CreateLeftLabel(hwnd_, L"5", 0, 0, Dp(30), Dp(20), instance_));
        add(row_h + Dp(10), {
            {ay_label, Dp(10), Dp(3), label_w, Dp(20)},
            {air_mouse_sensitivity_y_trackbar_, ctrl_x, 0, ctrl_w - Dp(50), Dp(28)},
            {air_mouse_sensitivity_y_value_label_, ctrl_x + ctrl_w - Dp(40), Dp(5), Dp(30), Dp(20)},
        });
    }
    separator();

    // ===== 系统 =====
    section_title(StringId::kSettingsSectionSystem);
    {
        HWND lal_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        launch_at_login_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsLaunchAtLogin, language).c_str(),
                                                       0, 0, ctrl_w, Dp(22), kIdLaunchAtLogin, instance_,
                                                       BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {lal_label, Dp(10), Dp(3), label_w, Dp(20)},
            {launch_at_login_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        HWND da_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        debug_audio_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsDebugAudio, language).c_str(),
                                                   0, 0, ctrl_w, Dp(22), kIdDebugAudio, instance_,
                                                   BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {da_label, Dp(10), Dp(3), label_w, Dp(20)},
            {debug_audio_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        HWND sid_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        show_imu_debug_check_ = remember(CreateButton(hwnd_, TrW(StringId::kSettingsShowImuDebug, language).c_str(),
                                                      0, 0, ctrl_w, Dp(22), kIdShowImuDebug, instance_,
                                                      BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {sid_label, Dp(10), Dp(3), label_w, Dp(20)},
            {show_imu_debug_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        HWND dd_label = remember_label(CreateLabel(hwnd_, label_text(StringId::kSettingsDebugDir).c_str(),
                                                    0, 0, label_w, Dp(20), instance_));
        debug_dir_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w - Dp(80),
                                             Dp(24), kIdDebugDirEdit, instance_, ES_READONLY));
        HWND choose_btn = remember(CreateButton(hwnd_, TrW(StringId::kSettingsChooseDir, language).c_str(),
                                                0, 0, Dp(75), Dp(24), kIdChooseDir, instance_));
        add(row_h + Dp(20), {
            {dd_label, Dp(10), Dp(3), label_w, Dp(20)},
            {debug_dir_edit_, ctrl_x, 0, ctrl_w - Dp(80), Dp(24)},
            {choose_btn, ctrl_x + ctrl_w - Dp(75), 0, Dp(75), Dp(24)},
        });
    }

    const int btn_w = Dp(80);
    const int btn_h = Dp(30);
    save_button_ = remember(CreateButton(hwnd_, TrW(StringId::kSave, language).c_str(),
                                         0, 0, btn_w, btn_h, kIdSave, instance_));
    cancel_button_ = remember(CreateButton(hwnd_, TrW(StringId::kCancel, language).c_str(),
                                           0, 0, btn_w, btn_h, kIdCancel, instance_));

    for (HWND control : all_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    // 分组标题套用加粗字体，覆盖上面的普通字体设置。
    for (HWND control : title_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(title_font), TRUE);
    }
    // 按声明式布局表应用定位并按可见行数动态调整窗口高度。
    Relayout();
}

void SettingsDialog::Relayout() {
    if (!hwnd_) return;

    // 1. 累加可见条目得到逻辑内容高度（不含按钮区）。
    int content_h = Dp(20);  // 顶部起始
    for (const auto& entry : layout_) {
        if (!entry.visible || entry.visible()) content_h += entry.advance;
    }

    // 2. 按钮区高度（顶部间距 + 按钮 + 底部间距）。
    const int btn_h = Dp(30);
    const int btn_area = Dp(20) + btn_h + Dp(20);  // Dp(70)

    // 3. 窗口高度上限 = 屏幕工作区高度 - 边距；自然高度 = 内容 + 按钮区。
    RECT work = GetWorkAreaForWindow(hwnd_);
    const int max_visible = (work.bottom - work.top) - Dp(40);
    const int natural_h = content_h + btn_area;
    const int client_h = std::min(natural_h, max_visible);

    // 4. 内容可视高度 = 客户区 - 按钮区；滚动范围。
    const int content_area_h = client_h - btn_area;
    const int scroll_range = std::max(0, content_h - content_area_h);
    scroll_pos_ = std::clamp(scroll_pos_, 0, scroll_range);

    // 5. 设置滚动条：nPage>=nMax+1 时滚动条自动禁用，SIF_DISABLENOSCROLL 保持占位。
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = content_h - 1;
    si.nPage = static_cast<UINT>(content_area_h);
    si.nPos = scroll_pos_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

    // 6. 窗口高度。
    ResizeWindow(client_h);

    // 7. 内容控件按滚动偏移定位。起始 y >= content_area_h（按钮区）的条目整体隐藏，
    //    避免内容溢出按钮区与保存/取消重叠（替代不透明背景遮挡，更稳健）。
    int y = Dp(20) - scroll_pos_;
    for (const auto& entry : layout_) {
        const bool vis = !entry.visible || entry.visible();
        const bool in_view = vis && (y + entry.advance > Dp(0)) && (y < content_area_h);
        for (const auto& p : entry.parts) {
            if (!p.control) continue;
            if (in_view) {
                SetWindowPos(p.control, nullptr, p.x, y + p.y_off, p.w, p.h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (!p.defer_visibility) ShowWindow(p.control, SW_SHOW);
            } else if (!p.defer_visibility) {
                ShowWindow(p.control, SW_HIDE);
            }
        }
        if (vis) y += entry.advance;
    }

    // 8. 按钮钉底：y = client_h - btn_h - Dp(20)。
    //    不超高时 client_h=natural_h，代入得 y=content_h+Dp(20)（紧跟内容下方）；
    //    超高时钉在窗口底部。两种情况统一。
    //    用 SWP_NOZORDER 保持创建顺序的 z 序（取消在保存之上），避免反复 HWND_TOP
    //    在 DWM 下间歇触发按钮合成异常。
    const int btn_w = Dp(80);
    const int btn_y = client_h - btn_h - Dp(20);
    if (save_button_) {
        SetWindowPos(save_button_, nullptr, Dp(kClientWidth - 200), btn_y, btn_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(save_button_, SW_SHOW);
    }
    if (cancel_button_) {
        SetWindowPos(cancel_button_, nullptr, Dp(kClientWidth - 105), btn_y, btn_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(cancel_button_, SW_SHOW);
    }
    // 行内条件：apply_trial_button 显隐 + api_key_edit 宽度，需在 Relayout 定位后修正。
    ApplyApiKeyLayout();

    // 全量 invalidate + 擦除背景 + 重绘所有子窗口：WS_CLIPCHILDREN 下父窗口擦除排除
    // 子窗口区域，按钮等子控件不被擦除覆盖；重绘让按钮保持可见。标签已用不透明 BTNFACE
    // 背景自擦除，滚动无残留。
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void SettingsDialog::ResizeWindow(int client_h) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), client_h};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void SettingsDialog::ApplyApiKeyLayout() {
    if (!api_key_edit_ || !apply_trial_button_ || !provider_combo_) return;
    int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    const bool is_cloud = (idx == 0);
    const bool is_tencent = (idx == 2);
    const bool api_key_empty = GetWindowText(api_key_edit_).empty();
    // VoiceStick Cloud 且 API Key 为空时显示试用按钮；腾讯云不需要试用按钮。
    bool show_trial = is_cloud && api_key_empty;
    if (is_tencent) show_trial = false;
    ShowWindow(apply_trial_button_, show_trial ? SW_SHOW : SW_HIDE);
    const int ctrl_w = Dp(kClientWidth - 230);
    const int apply_btn_w = Dp(102);
    const int api_key_w = show_trial ? ctrl_w - apply_btn_w - Dp(8) : ctrl_w;
    SetWindowPos(api_key_edit_, nullptr, 0, 0, api_key_w, Dp(24),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void SettingsDialog::LoadConfigIntoControls() {
    int language_index = 0;
    if (config_.ui_language == UiLanguage::kEnglish) language_index = 1;
    if (config_.ui_language == UiLanguage::kSimplifiedChinese) language_index = 2;
    SendMessageW(language_combo_, CB_SETCURSEL, language_index, 0);

    int provider_idx = 0;
    if (config_.asr_provider == AsrProvider::kVolcengine) provider_idx = 1;
    if (config_.asr_provider == AsrProvider::kTencent) provider_idx = 2;
    SendMessageW(provider_combo_, CB_SETCURSEL, provider_idx, 0);

    const auto& key = [&]() -> const std::string& {
        switch (config_.asr_provider) {
            case AsrProvider::kVoiceStickCloud: return config_.voicestick_api_key;
            case AsrProvider::kVolcengine: return config_.volcengine_api_key;
            case AsrProvider::kTencent: return config_.tencent_secret_id;
        }
        return config_.voicestick_api_key;
    }();
    SetWindowTextW(api_key_edit_, Utf16(key).c_str());

    auto resource_wide = Utf16(config_.resource_id);
    int idx = static_cast<int>(SendMessageW(resource_combo_, CB_FINDSTRINGEXACT, -1,
                                            reinterpret_cast<LPARAM>(resource_wide.c_str())));
    SendMessageW(resource_combo_, CB_SETCURSEL, idx >= 0 ? idx : 0, 0);

    SetWindowTextW(hotwords_edit_, Utf16(JoinHotwords(config_.asr_hotwords)).c_str());
    SetWindowTextW(llm_base_url_edit_, Utf16(config_.llm_base_url).c_str());
    SetWindowTextW(llm_api_key_edit_, Utf16(config_.llm_api_key).c_str());
    SetWindowTextW(llm_model_edit_, Utf16(config_.llm_model).c_str());

    SendMessageW(refine_check_, BM_SETCHECK, config_.refine_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    {
        std::string src = config_.refine_prompt.empty()
            ? LLMRefinementClient::BuildRefinePrompt("")
            : config_.refine_prompt;
        SetWindowTextW(refine_prompt_edit_, Utf16(src).c_str());
    }
    UpdateRefinePromptVisibility();

    SendMessageW(launch_at_login_check_, BM_SETCHECK, config_.launch_at_login ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(debug_audio_check_, BM_SETCHECK, config_.debug_audio_cache ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(show_imu_debug_check_, BM_SETCHECK, config_.show_imu_debug ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tap_to_arrow_check_, BM_SETCHECK, config_.tap_to_arrow ? BST_CHECKED : BST_UNCHECKED, 0);
    int sensitivity_index = 0;
    if (config_.imu_wake_sensitivity == ImuWakeSensitivity::kMedium) sensitivity_index = 1;
    if (config_.imu_wake_sensitivity == ImuWakeSensitivity::kHigh) sensitivity_index = 2;
    SendMessageW(imu_wake_sensitivity_combo_, CB_SETCURSEL, sensitivity_index, 0);
    SendMessageW(tap_sensitivity_trackbar_, TBM_SETPOS, TRUE, config_.tap_sensitivity);
    UpdateTapSensitivityLabel();
    SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_SETPOS, TRUE, config_.air_mouse_sensitivity_x);
    UpdateAirMouseSensitivityXLabel();
    SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_SETPOS, TRUE, config_.air_mouse_sensitivity_y);
    UpdateAirMouseSensitivityYLabel();

    SetWindowTextW(debug_dir_edit_, config_.debug_audio_directory.c_str());

    int output_target_idx = 0;
    if (config_.default_output_profile.target == OutputTarget::kSubtitle) output_target_idx = 1;
    if (config_.default_output_profile.target == OutputTarget::kWechatInputMethod) output_target_idx = 2;
    SendMessageW(output_target_combo_, CB_SETCURSEL, output_target_idx, 0);
    // 热键编辑框显示当前触发模式对应的热键（长按式/点按式各自记忆）。
    loaded_hotkey_mode_ = config_.wechat_input_method.trigger_mode;
    SetWindowTextW(wechat_hotkey_edit_,
                   Utf16(config_.wechat_input_method.ActiveHotkey(loaded_hotkey_mode_)).c_str());
    // 触发方式读写 wechat 专属 trigger_mode（与全局 interaction_mode 解耦，不污染 focused_app/字幕）。
    const bool click_trigger = config_.wechat_input_method.trigger_mode == InteractionMode::kClickToTalk;
    SendMessageW(trigger_mode_hold_radio_, BM_SETCHECK,
                 click_trigger ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(trigger_mode_click_radio_, BM_SETCHECK,
                 click_trigger ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateOutputTargetVisibility();

    UpdateProviderVisibility();
}

void SettingsDialog::SaveSettings() {
    int language_idx = static_cast<int>(SendMessageW(language_combo_, CB_GETCURSEL, 0, 0));
    if (language_idx == 1) {
        config_.ui_language = UiLanguage::kEnglish;
    } else if (language_idx == 2) {
        config_.ui_language = UiLanguage::kSimplifiedChinese;
    } else {
        config_.ui_language = UiLanguage::kSystem;
    }

    int provider_idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    AsrProvider new_provider = AsrProvider::kVoiceStickCloud;
    if (provider_idx == 1) new_provider = AsrProvider::kVolcengine;
    if (provider_idx == 2) new_provider = AsrProvider::kTencent;

    auto api_key = Utf8(GetWindowText(api_key_edit_));
    switch (new_provider) {
        case AsrProvider::kVoiceStickCloud: config_.voicestick_api_key = api_key; break;
        case AsrProvider::kVolcengine: config_.volcengine_api_key = api_key; break;
        case AsrProvider::kTencent: config_.tencent_secret_id = api_key; break;
    }
    config_.asr_provider = new_provider;
    config_.llm_base_url = Utf8(GetWindowText(llm_base_url_edit_));
    config_.llm_api_key = Utf8(GetWindowText(llm_api_key_edit_));
    config_.llm_model = Utf8(GetWindowText(llm_model_edit_));
    config_.refine_enabled = SendMessageW(refine_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    {
        auto prompt = Utf8(GetWindowText(refine_prompt_edit_));
        // 归一化 \r\n → \n（编辑控件返回 CRLF，LLM 用 LF）。
        std::string normalized;
        normalized.reserve(prompt.size());
        for (std::size_t i = 0; i < prompt.size(); ++i) {
            if (prompt[i] == '\r' && i + 1 < prompt.size() && prompt[i + 1] == '\n') {
                normalized.push_back('\n');
                ++i;
            } else if (prompt[i] == '\r') {
                normalized.push_back('\n');
            } else {
                normalized.push_back(prompt[i]);
            }
        }
        auto default_prompt = LLMRefinementClient::BuildRefinePrompt("");
        config_.refine_prompt = (normalized == default_prompt) ? std::string() : normalized;
    }
    config_.asr_hotwords = ParseHotwordList(Utf8(GetWindowText(hotwords_edit_)));

    wchar_t resource_buf[256]{};
    int res_idx = static_cast<int>(SendMessageW(resource_combo_, CB_GETCURSEL, 0, 0));
    if (res_idx >= 0) {
        SendMessageW(resource_combo_, CB_GETLBTEXT, res_idx,
                     reinterpret_cast<LPARAM>(resource_buf));
        config_.resource_id = Utf8(resource_buf);
    }

    config_.launch_at_login = SendMessageW(launch_at_login_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config_.debug_audio_cache = SendMessageW(debug_audio_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config_.show_imu_debug = SendMessageW(show_imu_debug_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config_.tap_to_arrow = SendMessageW(tap_to_arrow_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int sensitivity_idx = static_cast<int>(SendMessageW(imu_wake_sensitivity_combo_, CB_GETCURSEL, 0, 0));
    if (sensitivity_idx == 1) {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    } else if (sensitivity_idx == 2) {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    } else {
        config_.imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    }
    int tap_level = static_cast<int>(SendMessageW(tap_sensitivity_trackbar_, TBM_GETPOS, 0, 0));
    config_.tap_sensitivity = TapSensitivityClamp(tap_level);
    int air_x_level = static_cast<int>(SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_GETPOS, 0, 0));
    config_.air_mouse_sensitivity_x = AirMouseSensitivityClamp(air_x_level);
    int air_y_level = static_cast<int>(SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_GETPOS, 0, 0));
    config_.air_mouse_sensitivity_y = AirMouseSensitivityClamp(air_y_level);

    auto dir = GetWindowText(debug_dir_edit_);
    if (!dir.empty()) config_.debug_audio_directory = dir;

    int output_target_idx = static_cast<int>(SendMessageW(output_target_combo_, CB_GETCURSEL, 0, 0));
    if (output_target_idx == 1) {
        config_.default_output_profile.target = OutputTarget::kSubtitle;
    } else if (output_target_idx == 2) {
        config_.default_output_profile.target = OutputTarget::kWechatInputMethod;
    } else {
        config_.default_output_profile.target = OutputTarget::kFocusedApp;
    }
    // 热键编辑框值存回当前显示模式对应的字段（长按式/点按式各自记忆）。
    const std::string edited_hotkey = Utf8(GetWindowText(wechat_hotkey_edit_));
    if (loaded_hotkey_mode_ == InteractionMode::kClickToTalk) {
        config_.wechat_input_method.hotkey_click = edited_hotkey;
    } else {
        config_.wechat_input_method.hotkey_hold = edited_hotkey;
    }
    // 触发方式写入 wechat 专属 trigger_mode（不影响全局 interaction_mode，focused_app/字幕
    // 的触发方式由托盘菜单的全局 interaction_mode 控制）。
    const bool click_trigger =
        SendMessageW(trigger_mode_click_radio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config_.wechat_input_method.trigger_mode =
        click_trigger ? InteractionMode::kClickToTalk : InteractionMode::kHoldToTalk;

    // config_.Save() 可能在 config.toml 被其他进程占用（如 OneDrive 同步/杀毒扫描）
    // 时抛 runtime_error，或路径含 ACP 无法表示字符时抛 system_error。此处无 try-catch
    // 会直接 std::terminate 闪退。捕获后提示用户，保留对话框不关闭，便于重试。
    try {
        config_.Save();
    } catch (const std::exception& e) {
        LogApp(std::string("SaveSettings: config_.Save failed: ") + e.what());
        const auto language = EffectiveUiLanguage(config_.ui_language);
        MessageBoxW(hwnd_, TrW(StringId::kSettingsSaveFailed, language).c_str(),
                    TrW(StringId::kSettingsTitle, language).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    EndDialog(hwnd_, IDOK);
    if (on_config_changed) on_config_changed(config_);
}

void SettingsDialog::UpdateOutputTargetVisibility() {
    // 微信两行的显隐与定位交由 Relayout 统一处理。
    Relayout();
}

void SettingsDialog::OnTriggerModeChanged() {
    // 切换触发方式时，把编辑框当前值存回旧模式字段，再填入新模式对应的热键。
    const InteractionMode new_mode =
        (SendMessageW(trigger_mode_click_radio_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            ? InteractionMode::kClickToTalk
            : InteractionMode::kHoldToTalk;
    if (new_mode == loaded_hotkey_mode_) return;
    const std::string edited = Utf8(GetWindowText(wechat_hotkey_edit_));
    if (loaded_hotkey_mode_ == InteractionMode::kClickToTalk) {
        config_.wechat_input_method.hotkey_click = edited;
    } else {
        config_.wechat_input_method.hotkey_hold = edited;
    }
    loaded_hotkey_mode_ = new_mode;
    SetWindowTextW(wechat_hotkey_edit_,
                   Utf16(config_.wechat_input_method.ActiveHotkey(new_mode)).c_str());
}

void SettingsDialog::UpdateProviderVisibility() {
    // 资源 ID 行显隐、apply_trial_button 显隐与 api_key_edit 宽度均由 Relayout 统一处理。
    Relayout();
}

void SettingsDialog::ApplyTrialApiKey() {
    int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    if (idx != 0) return;
    const UiLanguage language = EffectiveUiLanguage(config_.ui_language);

    EnableWindow(apply_trial_button_, FALSE);
    SetWindowTextW(apply_trial_button_, TrW(StringId::kSettingsApplyingTrial, language).c_str());
    UpdateWindow(apply_trial_button_);

    const std::string device_id = config_.paired_device_ids.empty()
                                      ? std::string()
                                      : config_.paired_device_ids.front();
    auto result = ApplyVoiceStickCloudTrialApiKey(config_.voicestick_cloud_url, device_id);

    SetWindowTextW(apply_trial_button_, TrW(StringId::kSettingsApplyTrial, language).c_str());
    EnableWindow(apply_trial_button_, TRUE);

    if (!result.api_key.empty()) {
        SetWindowTextW(api_key_edit_, Utf16(result.api_key).c_str());
        UpdateProviderVisibility();
        return;
    }

    if (!result.url.empty()) {
        const auto wide_url = Utf16(result.url);
        auto* shell_result = ShellExecuteW(hwnd_, L"open", wide_url.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(shell_result) <= 32) {
            MessageBoxW(hwnd_, TrW(StringId::kSettingsTrialFailedMessage, language).c_str(),
                        TrW(StringId::kSettingsTrialFailedTitle, language).c_str(), MB_ICONERROR | MB_OK);
        }
        UpdateProviderVisibility();
        return;
    }

    MessageBoxW(hwnd_, Utf16(result.error.empty()
                             ? Tr(StringId::kSettingsTrialFailedMessage, language)
                             : result.error).c_str(),
                TrW(StringId::kSettingsTrialFailedTitle, language).c_str(), MB_ICONERROR | MB_OK);
    UpdateProviderVisibility();
}

void SettingsDialog::ChooseDebugDirectory() {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        return;
    }
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    auto current_dir = GetWindowText(debug_dir_edit_);
    if (!current_dir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(current_dir.c_str(), nullptr,
                                                  IID_IShellItem, reinterpret_cast<void**>(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                SetWindowTextW(debug_dir_edit_, path);
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }
    dialog->Release();
}

bool SettingsDialog::IsLabelControl(HWND control) const {
    return std::find(label_controls_.begin(), label_controls_.end(), control) !=
           label_controls_.end();
}

void SettingsDialog::UpdateRefinePromptVisibility() {
    // 精修提示词块的显隐与定位交由 Relayout 统一处理。
    Relayout();
}

void SettingsDialog::UpdateTapSensitivityLabel() {
    if (!tap_sensitivity_trackbar_ || !tap_sensitivity_value_label_) return;
    const int level = static_cast<int>(SendMessageW(tap_sensitivity_trackbar_, TBM_GETPOS, 0, 0));
    SetWindowTextW(tap_sensitivity_value_label_, std::to_wstring(level).c_str());
}

void SettingsDialog::UpdateAirMouseSensitivityXLabel() {
    if (!air_mouse_sensitivity_x_trackbar_ || !air_mouse_sensitivity_x_value_label_) return;
    const int level = static_cast<int>(SendMessageW(air_mouse_sensitivity_x_trackbar_, TBM_GETPOS, 0, 0));
    SetWindowTextW(air_mouse_sensitivity_x_value_label_, std::to_wstring(level).c_str());
}

void SettingsDialog::UpdateAirMouseSensitivityYLabel() {
    if (!air_mouse_sensitivity_y_trackbar_ || !air_mouse_sensitivity_y_value_label_) return;
    const int level = static_cast<int>(SendMessageW(air_mouse_sensitivity_y_trackbar_, TBM_GETPOS, 0, 0));
    SetWindowTextW(air_mouse_sensitivity_y_value_label_, std::to_wstring(level).c_str());
}

int SettingsDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

} // namespace voicestick
