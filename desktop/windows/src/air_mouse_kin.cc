#include "air_mouse_kin.h"

#include <cmath>

namespace voicestick {

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params) {
    // stale 时 omega 视为 0：手停后不积分 θ，仅衰减（光标慢停）。
    const double ox = omega_is_stale ? 0.0 : static_cast<double>(omega_x);
    double oy = omega_is_stale ? 0.0 : static_cast<double>(omega_y);
    if (params.invert_y) oy = -oy;

    // 积分角速度得相对偏转角 θ：手腕转到 θ 停住 → θ 保持 → 光标持续移动。
    state.theta_x += ox * dt_seconds;
    state.theta_y += oy * dt_seconds;

    // 慢衰减防漂移：每帧 θ × exp(-dt/decay_tau)。短期（几秒）θ 基本保持，长期回中，
    // 压制陀螺仪零偏残差累积。decay_tau=8s 时 0.16s 仅衰减 2%。
    const double decay = std::exp(-dt_seconds / params.decay_tau);
    state.theta_x *= decay;
    state.theta_y *= decay;

    // 角度→目标速度（分轴 gain：左右/上下独立灵敏度）。
    const double v_target_x = state.theta_x * params.gain_x;
    const double v_target_y = state.theta_y * params.gain_y;

    // 速度环（一阶低通）：dv/dt=(v_target-v)/tau，平滑光标速度。
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
