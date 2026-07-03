#include "air_mouse_kin.h"

#include <cmath>

namespace voicestick {

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params) {
    // stale 时 omega 视为 0：手停后定时器继续 step，让 v 按 tau 衰减到 0，实现缓停。
    const double ox = omega_is_stale ? 0.0 : static_cast<double>(omega_x);
    double oy = omega_is_stale ? 0.0 : static_cast<double>(omega_y);
    if (params.invert_y) oy = -oy;

    // 幂律加速曲线：v_target = sign(omega) × |omega|^gamma × gain。
    // gamma>1 使小输入更稳、大输入更猛（慢稳快猛）。
    const auto target_velocity = [](double omega, double gamma, double gain) -> double {
        if (omega == 0.0) return 0.0;
        const double mag = std::pow(std::fabs(omega), gamma) * gain;
        return omega < 0.0 ? -mag : mag;
    };
    const double v_target_x = target_velocity(ox, params.gamma, params.gain);
    const double v_target_y = target_velocity(oy, params.gamma, params.gain);

    // 一阶低通跟踪：v += (v_target - v) × (1 - exp(-dt/tau))。
    // 等价于 P 速度环 dv/dt = (v_target - v)/tau，dv/dt 即加速度；阻尼由 tau 单参数控制，
    // 离散稳定无超调。dt 抖动下仍正确积分（用实际帧间隔）。
    const double alpha = 1.0 - std::exp(-dt_seconds / params.tau);
    state.vx += (v_target_x - state.vx) * alpha;
    state.vy += (v_target_y - state.vy) * alpha;

    // 输出位移 = v × dt，四舍五入到整像素，供 SendInput 相对移动。
    AirMouseStepResult result;
    result.dx = static_cast<int>(std::lround(state.vx * dt_seconds));
    result.dy = static_cast<int>(std::lround(state.vy * dt_seconds));
    return result;
}

} // namespace voicestick
