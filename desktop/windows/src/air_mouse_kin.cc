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

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                const AirMouseInput& input,
                                double dt_seconds,
                                bool input_is_stale,
                                const AirMouseParams& params) {
    // stale 时输入视为 0：手停后 v_target=0，v 经 tau 快速归零（手停即停）。
    const double ix = input_is_stale ? 0.0 : static_cast<double>(input.value_x);
    double iy = input_is_stale ? 0.0 : static_cast<double>(input.value_y);
    if (params.invert_y) iy = -iy;

    // 速度控制：目标速度 = value × gain × factor(|value|)（三段线性增益曲线，慢稳快猛）。
    // 输入为 0 即 v_target=0（手停即停）。
    const double v_target_x = ix * params.gain_x * AirMouseGainFactor(ix, params.curve);
    const double v_target_y = iy * params.gain_y * AirMouseGainFactor(iy, params.curve);

    // 速度环（一阶低通）：平滑噪声，手停后 v 经 tau 衰减归零（滑行 ≈ 3×tau）。
    const double alpha = 1.0 - std::exp(-dt_seconds / params.tau);
    state.vx += (v_target_x - state.vx) * alpha;
    state.vy += (v_target_y - state.vy) * alpha;

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
