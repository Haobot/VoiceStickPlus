#pragma once

#include <cstdint>

namespace voicestick {

// 体感鼠标运动学状态（角度控制：积分角速度得相对偏转角 θ，保持 θ 光标持续移动）。
struct AirMouseKinState {
    double theta_x = 0.0;  // 相对偏转角（积分角速度得）
    double theta_y = 0.0;
    double vx = 0.0;       // 光标速度（像素/秒）
    double vy = 0.0;
    double fx = 0.0;       // 亚像素位移累积
    double fy = 0.0;
};

// 体感鼠标角度控制参数（由配置项填充，真机标定）。
struct AirMouseParams {
    double tau = 0.10;         // 速度环时间常数（秒），越大惯性/缓停越长
    double gain_x = 10.0;      // 左右（yaw）角度→速度增益
    double gain_y = 10.0;      // 上下（pitch）角度→速度增益
    double decay_tau = 8.0;    // θ 慢衰减防漂移（秒），短期保持长期回中
    bool invert_y = false;     // 是否反转 Y 轴
};

struct AirMouseStepResult {
    int dx = 0;
    int dy = 0;
};

// 体感鼠标角度控制单次 step：
//   θ += omega × dt                     // 积分角速度得相对偏转角
//   θ *= exp(-dt/decay_tau)             // 慢衰减防漂移
//   v_target = θ × gain                 // 角度→目标速度（保持 θ 光标持续移动）
//   v += (v_target - v) × (1-exp(-dt/tau))
//   fx += v × dt; dx = trunc(fx)        // 亚像素累积
// state 原地更新。omega_is_stale=true 时 omega 视为 0（手停后 θ 仅衰减，不积分）。
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params);

} // namespace voicestick
