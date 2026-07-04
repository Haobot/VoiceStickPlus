#include "air_mouse_kin.h"

#include <cmath>

namespace voicestick {

// 三段线性增益因子：微调段压低、中段线性过渡、甩动段放大（慢稳快猛）。
// 拐点 low_thresh/high_thresh 处两侧 factor 连续，无跳变。curve 运行期可变（热调参）。
double AirMouseGainFactor(double x_abs, const AirMouseCurveParams& curve) {
    const double a = std::fabs(x_abs);
    if (a < curve.low_thresh) {
        return curve.low_factor;
    }
    if (a < curve.high_thresh) {
        // 中段线性插值 low_factor → high_factor。
        return curve.low_factor + (curve.high_factor - curve.low_factor) *
               (a - curve.low_thresh) / (curve.high_thresh - curve.low_thresh);
    }
    return curve.high_factor;
}

// 钳位曲线参数到合法范围，保证 low_thresh < high_thresh（防中段除零与曲线退化）。
// 配置解析与热调参 UI 均复用：越界值夹紧到标定上下限，low≥high 时退 low=high-1。
AirMouseCurveParams AirMouseCurveClamp(AirMouseCurveParams curve) {
    if (curve.low_thresh < 1.0) curve.low_thresh = 1.0;
    if (curve.low_thresh > 30.0) curve.low_thresh = 30.0;
    if (curve.high_thresh < 30.0) curve.high_thresh = 30.0;
    if (curve.high_thresh > 80.0) curve.high_thresh = 80.0;
    if (curve.low_thresh >= curve.high_thresh) {
        curve.low_thresh = curve.high_thresh - 1.0;
    }
    if (curve.low_factor < 0.05) curve.low_factor = 0.05;
    if (curve.low_factor > 0.5) curve.low_factor = 0.5;
    if (curve.high_factor < 2.0) curve.high_factor = 2.0;
    if (curve.high_factor > 6.0) curve.high_factor = 6.0;
    return curve;
}

// 方向锁：中立区死区内光标停并释放方向锁；
// 未锁定时越过死区才锁定方向；异常过冲时释放锁而非直接反向。
double AirMouseApplyDirectionLock(double theta, AirMouseDirectionLock& lock, double deadzone) {
    if (std::fabs(theta) <= deadzone) {
        lock = AirMouseDirectionLock::kNone;
        return 0.0;
    }
    if (lock == AirMouseDirectionLock::kNone) {
        lock = theta > 0.0 ? AirMouseDirectionLock::kPositive : AirMouseDirectionLock::kNegative;
        return theta;
    }
    const bool positive = theta > 0.0;
    const bool locked_positive = lock == AirMouseDirectionLock::kPositive;
    if (positive != locked_positive) {
        // 已锁定方向与当前 theta 符号冲突（未经过死区就过冲到反向），释放锁并暂停，
        // 下一 tick 如果仍在反向死区外会重新锁定反向。
        lock = AirMouseDirectionLock::kNone;
        return 0.0;
    }
    return theta;
}

namespace {

// 一阶低通速度环（kAngle 模式）。
void AirMouseStepAngleMode(AirMouseKinState& state,
                           double ix, double iy,
                           double dt_seconds,
                           const AirMouseParams& params) {
    // 速度控制：目标速度 = value × gain × factor(|value|)（三段线性增益曲线，慢稳快猛）。
    const double v_target_x = ix * params.gain_x * AirMouseGainFactor(ix, params.curve);
    const double v_target_y = iy * params.gain_y * AirMouseGainFactor(iy, params.curve);

    // 速度环（一阶低通）：平滑噪声，手停后 v 经 tau 衰减归零（滑行 ≈ 3×tau）。
    const double alpha = 1.0 - std::exp(-dt_seconds / params.tau);
    state.vx += (v_target_x - state.vx) * alpha;
    state.vy += (v_target_y - state.vy) * alpha;
}

// 飞行摇杆/变化率控制（kRate 模式）：theta 控制光标速度变化率，回中后速度保持。
void AirMouseStepRateMode(AirMouseKinState& state,
                          double ix, double iy,
                          double dt_seconds,
                          const AirMouseParams& params) {
    // 三段线性增益曲线也作用于加速度，保留微调段/甩动段的非线性手感。
    const double ax = ix * params.rate_gain * AirMouseGainFactor(ix, params.curve);
    const double ay = iy * params.rate_gain * AirMouseGainFactor(iy, params.curve);

    state.vx += ax * dt_seconds;
    state.vy += ay * dt_seconds;

    // 摩擦衰减：手腕回中后速度不会永远保持，按指数衰减自然停下。
    const double friction_decay = std::exp(-params.rate_friction * dt_seconds);
    state.vx *= friction_decay;
    state.vy *= friction_decay;

    // 速度上限，防止长期偏转导致光标速度无限增长。
    if (state.vx > params.rate_max_speed) state.vx = params.rate_max_speed;
    if (state.vx < -params.rate_max_speed) state.vx = -params.rate_max_speed;
    if (state.vy > params.rate_max_speed) state.vy = params.rate_max_speed;
    if (state.vy < -params.rate_max_speed) state.vy = -params.rate_max_speed;
}

} // namespace

std::string AirMouseControlModeName(AirMouseControlMode mode) {
    switch (mode) {
    case AirMouseControlMode::kAngle: return "angle";
    case AirMouseControlMode::kRate:  return "rate";
    }
    return "rate";
}

AirMouseControlMode AirMouseControlModeFromName(std::string_view name) {
    if (name == "angle") return AirMouseControlMode::kAngle;
    return AirMouseControlMode::kRate;
}

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                const AirMouseInput& input,
                                double dt_seconds,
                                bool input_is_stale,
                                const AirMouseParams& params) {
    // stale 时输入视为 0。
    double ix = input_is_stale ? 0.0 : static_cast<double>(input.value_x);
    double iy = input_is_stale ? 0.0 : static_cast<double>(input.value_y);

    // 方向锁：必须先回到中立区死区，才能切换到反向；避免回转经过水平时误触发反向移动。
    ix = AirMouseApplyDirectionLock(ix, state.lock_x, params.neutral_deadzone);
    iy = AirMouseApplyDirectionLock(iy, state.lock_y, params.neutral_deadzone);

    if (params.invert_y) iy = -iy;

    if (params.control_mode == AirMouseControlMode::kAngle) {
        AirMouseStepAngleMode(state, ix, iy, dt_seconds, params);
    } else {
        AirMouseStepRateMode(state, ix, iy, dt_seconds, params);
    }

    // 亚像素累积：小 v 时保留小数，够 1px 才输出，避免 round 丢失精细移动。
    state.fx += state.vx * dt_seconds;
    state.fy += state.vy * dt_seconds;
    const int dx_int = static_cast<int>(state.fx);
    const int dy_int = static_cast<int>(state.fy);
    state.fx -= dx_int;
    state.fy -= dy_int;

    AirMouseStepResult result;
    result.dx = dx_int;
    result.dy = dy_int;
    return result;
}

} // namespace voicestick
