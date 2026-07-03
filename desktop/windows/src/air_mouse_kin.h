#pragma once

#include <cstdint>

namespace voicestick {

// 体感鼠标运动学状态（纯数据，无时钟——omega 历史与时间戳由协调器管理）。
// v 为光标速度，单位像素/秒。
struct AirMouseKinState {
    double vx = 0.0;
    double vy = 0.0;
    double fx = 0.0;  // 亚像素位移累积：小 v 时保留小数，够 1px 才输出，避免 round 丢失精细移动
    double fy = 0.0;
};

// 体感鼠标加速度/PD 参数（由配置项填充，真机标定）。
struct AirMouseParams {
    double tau = 0.10;      // 速度跟踪时间常数（秒），越大惯性/缓停越长
    double gamma = 1.35;    // 加速曲线幂律指数，>1 慢稳快猛
    double gain = 4.0;      // 速度增益，omega → v_target
    bool invert_y = false;  // 是否反转 Y 轴
};

// 单次 step 的输出位移（像素），供 SendInput 相对移动。
struct AirMouseStepResult {
    int dx = 0;
    int dy = 0;
};

// 体感鼠标速度环单次 step：
//   v_target = sign(omega) × |omega|^gamma × gain
//   v += (v_target - v) × (1 - exp(-dt/tau))     // 一阶低通跟踪，dv/dt 即加速度
//   dx = round(v × dt)
// state 原地更新。omega_is_stale=true 时 omega 视为 0（手停后定时器继续衰减 v 到 0，
// 实现缓停）。等价于 P 速度环，阻尼由 tau 单参数控制，离散稳定无超调。
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params);

} // namespace voicestick
