#include "air_mouse_kin.h"

#include <cmath>

namespace voicestick {

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params) {
    // stale 时 omega 视为 0：手停后 v_target=0，v 经 tau 快速归零（手停即停）。
    const double ox = omega_is_stale ? 0.0 : static_cast<double>(omega_x);
    double oy = omega_is_stale ? 0.0 : static_cast<double>(omega_y);
    if (params.invert_y) oy = -oy;

    // 速度控制：目标速度直接跟随角速度，omega=0 即 v_target=0（手停即停）。
    const double v_target_x = ox * params.gain_x;
    const double v_target_y = oy * params.gain_y;

    // 速度环（一阶低通）：平滑陀螺仪噪声，手停后 v 经 tau 衰减归零（滑行 ≈ 3×tau）。
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
