#include "air_mouse_tuning_window.h"

#include "log.h"

#include <commctrl.h>
#include <string>

#pragma comment(lib, "Comctl32.lib")

namespace voicestick {

namespace {

constexpr const wchar_t* kWindowTitle = L"体感鼠标调参（热调参，即时生效）";
constexpr const wchar_t* kWindowClass = L"VoiceStickAirMouseTuning";
constexpr int kWindowWidth = 500;
constexpr int kWindowHeight = 528;  // 容纳角度/飞行摇杆两组参数 + 按钮
constexpr int kLabelW = 130;
constexpr int kTrackW = 250;
constexpr int kValueW = 60;
constexpr int kRowH = 28;
constexpr int kMargin = 10;

// 控件命令 ID
constexpr UINT_PTR kBtnSave = 2001;
constexpr UINT_PTR kBtnReset = 2002;
constexpr UINT_PTR kCmbMode = 3001;

std::wstring WFromDouble(double v, int precision) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.*f", precision, v);
    return buf;
}

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

} // namespace

AirMouseTuningWindow::AirMouseTuningWindow(HINSTANCE instance, HWND parent,
                                           const std::string& device_id,
                                           const AirMouseParams& initial_params)
    : instance_(instance), parent_(parent), device_id_(device_id),
      state_(AirMouseTuningState::FromParams(initial_params)) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = &AirMouseTuningWindow::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);  // 已注册时返回 0，忽略
    // 标题带设备 ID，多设备时区分调参目标。
    std::wstring title = kWindowTitle + std::wstring(L" - VS-") + Utf16(device_id);
    hwnd_ = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight,
                            parent, nullptr, instance, this);
    if (!hwnd_) {
        LogApp("AirMouseTuningWindow CreateWindowEx failed: " + std::to_string(GetLastError()));
    }
    // WM_CREATE 已设 GWLP_USERDATA，无需再设
}

AirMouseTuningWindow::~AirMouseTuningWindow() {
    if (hwnd_) {
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void AirMouseTuningWindow::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
}

LRESULT CALLBACK AirMouseTuningWindow::WndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    AirMouseTuningWindow* self = reinterpret_cast<AirMouseTuningWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        self = reinterpret_cast<AirMouseTuningWindow*>(
            reinterpret_cast<CREATESTRUCT*>(l_param)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        switch (msg) {
        case WM_CREATE: self->hwnd_ = hwnd; self->OnCreate(); return 0;
        case WM_HSCROLL: self->OnHScroll(); return 0;
        case WM_COMMAND: self->OnCommand(w_param); return 0;
        case WM_CLOSE: DestroyWindow(hwnd); self->hwnd_ = nullptr; return 0;
        case WM_DESTROY: self->hwnd_ = nullptr; return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void AirMouseTuningWindow::OnCreate() {
    // 注册 trackbar 和 combobox 控件类。
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    int y = kMargin;
    auto make_row = [&](const wchar_t* text, HWND& track, HWND& label) {
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                        kMargin, y + 5, kLabelW, 18, hwnd_, nullptr, instance_, nullptr);
        track = CreateWindowExW(0, L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                kMargin + kLabelW, y, kTrackW, kRowH, hwnd_, nullptr, instance_, nullptr);
        label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                kMargin + kLabelW + kTrackW, y + 5, kValueW, 18, hwnd_, nullptr, instance_, nullptr);
        y += kRowH + 6;
    };

    // 控制模式下拉框
    mode_label_ = CreateWindowExW(0, L"STATIC", L"控制模式", WS_CHILD | WS_VISIBLE,
                                  kMargin, y + 5, kLabelW, 18, hwnd_, nullptr, instance_, nullptr);
    mode_combo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                  kMargin + kLabelW, y, 160, 120, hwnd_, reinterpret_cast<HMENU>(kCmbMode), instance_, nullptr);
    SendMessageW(mode_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"飞行摇杆（变化率）"));
    SendMessageW(mode_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"角度控制"));
    y += kRowH + 10;

    make_row(L"左右灵敏度 (1-10)", gain_x_track_, gain_x_label_);
    make_row(L"上下灵敏度 (1-10)", gain_y_track_, gain_y_label_);
    make_row(L"中立区死区", neutral_deadzone_track_, neutral_deadzone_label_);

    // 角度控制模式参数
    make_row(L"tau 时间常数", tau_track_, tau_label_);
    make_row(L"微调段阈值", low_thresh_track_, low_thresh_label_);
    make_row(L"甩动段阈值", high_thresh_track_, high_thresh_label_);
    make_row(L"微调段增益", low_factor_track_, low_factor_label_);
    make_row(L"甩动段增益", high_factor_track_, high_factor_label_);

    // 飞行摇杆模式参数
    make_row(L"变化率增益", rate_gain_track_, rate_gain_label_);
    make_row(L"摩擦系数", rate_friction_track_, rate_friction_label_);
    make_row(L"速度上限", rate_max_speed_track_, rate_max_speed_label_);

    invert_y_check_ = CreateWindowExW(0, L"BUTTON", L"反转 Y 轴", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      kMargin, y, 200, 22, hwnd_, nullptr, instance_, nullptr);
    y += kRowH + 8;

    save_btn_ = CreateWindowExW(0, L"BUTTON", L"保存到配置", WS_CHILD | WS_VISIBLE,
                                kMargin, y, 120, 28, hwnd_, reinterpret_cast<HMENU>(kBtnSave), instance_, nullptr);
    reset_btn_ = CreateWindowExW(0, L"BUTTON", L"重置默认", WS_CHILD | WS_VISIBLE,
                                 kMargin + 130, y, 100, 28, hwnd_, reinterpret_cast<HMENU>(kBtnReset), instance_, nullptr);

    // trackbar 范围（整数 pos，按比例换算 double）
    SendMessageW(gain_x_track_, TBM_SETRANGEMIN, FALSE, 1);
    SendMessageW(gain_x_track_, TBM_SETRANGEMAX, TRUE, 10);
    SendMessageW(gain_y_track_, TBM_SETRANGEMIN, FALSE, 1);
    SendMessageW(gain_y_track_, TBM_SETRANGEMAX, TRUE, 10);
    SendMessageW(neutral_deadzone_track_, TBM_SETRANGEMIN, FALSE, 10);  // 1.0 (×10)
    SendMessageW(neutral_deadzone_track_, TBM_SETRANGEMAX, TRUE, 100);  // 10.0
    SendMessageW(tau_track_, TBM_SETRANGEMIN, FALSE, 2);       // 0.02
    SendMessageW(tau_track_, TBM_SETRANGEMAX, TRUE, 50);       // 0.50
    SendMessageW(low_thresh_track_, TBM_SETRANGEMIN, FALSE, 2);   // 1.0 (×2)
    SendMessageW(low_thresh_track_, TBM_SETRANGEMAX, TRUE, 60);   // 30.0
    SendMessageW(high_thresh_track_, TBM_SETRANGEMIN, FALSE, 30);
    SendMessageW(high_thresh_track_, TBM_SETRANGEMAX, TRUE, 80);
    SendMessageW(low_factor_track_, TBM_SETRANGEMIN, FALSE, 5);   // 0.05 (×100)
    SendMessageW(low_factor_track_, TBM_SETRANGEMAX, TRUE, 50);   // 0.50
    SendMessageW(high_factor_track_, TBM_SETRANGEMIN, FALSE, 20); // 2.0 (×10)
    SendMessageW(high_factor_track_, TBM_SETRANGEMAX, TRUE, 60);  // 6.0
    SendMessageW(rate_gain_track_, TBM_SETRANGEMIN, FALSE, 10);   // 10.0
    SendMessageW(rate_gain_track_, TBM_SETRANGEMAX, TRUE, 500);   // 500.0
    SendMessageW(rate_friction_track_, TBM_SETRANGEMIN, FALSE, 0);   // 0.0 (×1000)
    SendMessageW(rate_friction_track_, TBM_SETRANGEMAX, TRUE, 500);  // 0.5
    SendMessageW(rate_max_speed_track_, TBM_SETRANGEMIN, FALSE, 50);   // 500.0 (÷10)
    SendMessageW(rate_max_speed_track_, TBM_SETRANGEMAX, TRUE, 800);   // 8000.0

    SyncControlsFromState();
    UpdateModeVisibility();
}

AirMouseTuningState AirMouseTuningWindow::StateFromControls() const {
    AirMouseTuningState s;
    s.control_mode = (SendMessageW(mode_combo_, CB_GETCURSEL, 0, 0) == 1)
                         ? AirMouseControlMode::kAngle
                         : AirMouseControlMode::kRate;
    s.sensitivity_x = static_cast<int>(SendMessageW(gain_x_track_, TBM_GETPOS, 0, 0));
    s.sensitivity_y = static_cast<int>(SendMessageW(gain_y_track_, TBM_GETPOS, 0, 0));
    s.neutral_deadzone = SendMessageW(neutral_deadzone_track_, TBM_GETPOS, 0, 0) / 10.0;
    s.tau = SendMessageW(tau_track_, TBM_GETPOS, 0, 0) / 100.0;
    s.curve.low_thresh = SendMessageW(low_thresh_track_, TBM_GETPOS, 0, 0) / 2.0;
    s.curve.high_thresh = static_cast<double>(SendMessageW(high_thresh_track_, TBM_GETPOS, 0, 0));
    s.curve.low_factor = SendMessageW(low_factor_track_, TBM_GETPOS, 0, 0) / 100.0;
    s.curve.high_factor = SendMessageW(high_factor_track_, TBM_GETPOS, 0, 0) / 10.0;
    s.rate_gain = static_cast<double>(SendMessageW(rate_gain_track_, TBM_GETPOS, 0, 0));
    s.rate_friction = SendMessageW(rate_friction_track_, TBM_GETPOS, 0, 0) / 1000.0;
    s.rate_max_speed = static_cast<double>(SendMessageW(rate_max_speed_track_, TBM_GETPOS, 0, 0)) * 10.0;
    s.invert_y = (SendMessageW(invert_y_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    return s;
}

void AirMouseTuningWindow::SyncControlsFromState() {
    SendMessageW(mode_combo_, CB_SETCURSEL, state_.control_mode == AirMouseControlMode::kAngle ? 1 : 0, 0);
    SendMessageW(gain_x_track_, TBM_SETPOS, TRUE, state_.sensitivity_x);
    SendMessageW(gain_y_track_, TBM_SETPOS, TRUE, state_.sensitivity_y);
    SendMessageW(neutral_deadzone_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.neutral_deadzone * 10));
    SendMessageW(tau_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.tau * 100));
    SendMessageW(low_thresh_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.curve.low_thresh * 2));
    SendMessageW(high_thresh_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.curve.high_thresh));
    SendMessageW(low_factor_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.curve.low_factor * 100));
    SendMessageW(high_factor_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.curve.high_factor * 10));
    SendMessageW(rate_gain_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.rate_gain));
    SendMessageW(rate_friction_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.rate_friction * 1000));
    SendMessageW(rate_max_speed_track_, TBM_SETPOS, TRUE, static_cast<int>(state_.rate_max_speed / 10.0));
    SendMessageW(invert_y_check_, BM_SETCHECK, state_.invert_y ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateLabels();
}

void AirMouseTuningWindow::UpdateLabels() {
    SetWindowTextW(gain_x_label_, std::to_wstring(state_.sensitivity_x).c_str());
    SetWindowTextW(gain_y_label_, std::to_wstring(state_.sensitivity_y).c_str());
    SetWindowTextW(neutral_deadzone_label_, WFromDouble(state_.neutral_deadzone, 1).c_str());
    SetWindowTextW(tau_label_, WFromDouble(state_.tau, 2).c_str());
    SetWindowTextW(low_thresh_label_, WFromDouble(state_.curve.low_thresh, 1).c_str());
    SetWindowTextW(high_thresh_label_, WFromDouble(state_.curve.high_thresh, 0).c_str());
    SetWindowTextW(low_factor_label_, WFromDouble(state_.curve.low_factor, 2).c_str());
    SetWindowTextW(high_factor_label_, WFromDouble(state_.curve.high_factor, 1).c_str());
    SetWindowTextW(rate_gain_label_, std::to_wstring(static_cast<int>(state_.rate_gain)).c_str());
    SetWindowTextW(rate_friction_label_, WFromDouble(state_.rate_friction, 3).c_str());
    SetWindowTextW(rate_max_speed_label_, std::to_wstring(static_cast<int>(state_.rate_max_speed)).c_str());
}

void AirMouseTuningWindow::OnHScroll() {
    ApplyParams();
}

void AirMouseTuningWindow::OnCommand(WPARAM w_param) {
    switch (LOWORD(w_param)) {
    case kBtnSave: SaveParams(); break;
    case kBtnReset: ResetDefault(); break;
    case kCmbMode:
        state_ = StateFromControls();
        UpdateModeVisibility();
        UpdateLabels();
        if (on_params_changed) on_params_changed(state_);
        break;
    default: break;
    }
}

void AirMouseTuningWindow::UpdateModeVisibility() {
    const bool angle = state_.control_mode == AirMouseControlMode::kAngle;
    // 角度模式参数
    const auto show_angle = angle ? SW_SHOW : SW_HIDE;
    ShowWindow(tau_track_, show_angle);
    ShowWindow(tau_label_, show_angle);
    ShowWindow(low_thresh_track_, show_angle);
    ShowWindow(low_thresh_label_, show_angle);
    ShowWindow(high_thresh_track_, show_angle);
    ShowWindow(high_thresh_label_, show_angle);
    ShowWindow(low_factor_track_, show_angle);
    ShowWindow(low_factor_label_, show_angle);
    ShowWindow(high_factor_track_, show_angle);
    ShowWindow(high_factor_label_, show_angle);
    // 飞行摇杆模式参数
    const auto show_rate = angle ? SW_HIDE : SW_SHOW;
    ShowWindow(rate_gain_track_, show_rate);
    ShowWindow(rate_gain_label_, show_rate);
    ShowWindow(rate_friction_track_, show_rate);
    ShowWindow(rate_friction_label_, show_rate);
    ShowWindow(rate_max_speed_track_, show_rate);
    ShowWindow(rate_max_speed_label_, show_rate);
    // 强制重排，避免隐藏后留下空白区域显示异常
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void AirMouseTuningWindow::ApplyParams() {
    state_ = StateFromControls();
    UpdateLabels();
    if (on_params_changed) on_params_changed(state_);
}

void AirMouseTuningWindow::SaveParams() {
    state_ = StateFromControls();
    if (on_save_requested) on_save_requested(state_);
}

void AirMouseTuningWindow::ResetDefault() {
    state_ = AirMouseTuningState{};  // 默认值：5/5/0.05/false/{15,50,0.15,4.0}
    SyncControlsFromState();
    if (on_params_changed) on_params_changed(state_);
}

} // namespace voicestick
