#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <windows.h>

#include "air_mouse_kin.h"

namespace voicestick {

// 热调参状态：档位 + 曲线参数（调参窗口操作，可派生 AirMouseParams）。
// 档位与 config_.air_mouse_sensitivity_x/y 对应（gain = 档位 × 16）。
struct AirMouseTuningState {
    int sensitivity_x = 5;
    int sensitivity_y = 5;
    double tau = 0.05;
    bool invert_y = false;
    AirMouseCurveParams curve;
    double neutral_deadzone = 3.0;  // 方向锁中立区死区，范围 1.0~10.0

    AirMouseParams ToParams() const {
        AirMouseParams p;
        p.gain_x = static_cast<double>(sensitivity_x) * 16.0;
        p.gain_y = static_cast<double>(sensitivity_y) * 16.0;
        p.tau = tau;
        p.invert_y = invert_y;
        p.curve = AirMouseCurveClamp(curve);
        p.neutral_deadzone = neutral_deadzone;
        return p;
    }
    static AirMouseTuningState FromParams(const AirMouseParams& p) {
        AirMouseTuningState s;
        s.sensitivity_x = std::clamp(static_cast<int>(std::lround(p.gain_x / 16.0)), 1, 10);
        s.sensitivity_y = std::clamp(static_cast<int>(std::lround(p.gain_y / 16.0)), 1, 10);
        s.tau = p.tau;
        s.invert_y = p.invert_y;
        s.curve = p.curve;
        s.neutral_deadzone = p.neutral_deadzone;
        return s;
    }
};

// 体感鼠标热调参窗口（非模态）：滑块即时改参数，AirMouseTick 下个 tick 生效。
// 即时改 → on_params_changed → coordinator_->UpdateAirMouseParams（轻量路径，不重建 LLM）。
// 保存 → on_save_requested → win32_app 写 config_ + Save + UpdateConfig（持久化）。
// 实时曲线显示为后续扩展（当前仅控件热调参）。
class AirMouseTuningWindow {
public:
    AirMouseTuningWindow(HINSTANCE instance, HWND parent, const AirMouseParams& initial_params);
    ~AirMouseTuningWindow();
    void Show();
    bool IsOpen() const { return hwnd_ != nullptr; }
    // 即时热调参（每滑块/复选改动触发）。
    std::function<void(const AirMouseTuningState&)> on_params_changed;
    // 保存到配置（持久化）。
    std::function<void(const AirMouseTuningState&)> on_save_requested;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnCreate();
    void OnHScroll();
    void OnCommand(WPARAM w_param);
    void ApplyParams();          // 读控件 → state_ → on_params_changed
    void SaveParams();           // on_save_requested(state_)
    void ResetDefault();
    void SyncControlsFromState();
    void UpdateLabels();
    AirMouseTuningState StateFromControls() const;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    AirMouseTuningState state_;
    // 控件句柄
    HWND gain_x_track_ = nullptr, gain_y_track_ = nullptr, tau_track_ = nullptr;
    HWND low_thresh_track_ = nullptr, high_thresh_track_ = nullptr;
    HWND low_factor_track_ = nullptr, high_factor_track_ = nullptr;
    HWND neutral_deadzone_track_ = nullptr;
    HWND invert_y_check_ = nullptr;
    HWND save_btn_ = nullptr, reset_btn_ = nullptr;
    HWND gain_x_label_ = nullptr, gain_y_label_ = nullptr, tau_label_ = nullptr;
    HWND low_thresh_label_ = nullptr, high_thresh_label_ = nullptr;
    HWND low_factor_label_ = nullptr, high_factor_label_ = nullptr;
    HWND neutral_deadzone_label_ = nullptr;
};

} // namespace voicestick
