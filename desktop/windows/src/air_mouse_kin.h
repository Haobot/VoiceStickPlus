#pragma once

#include <cstdint>

namespace voicestick {

// 体感鼠标运动学状态（速度控制：目标速度直接跟随角速度，omega=0 即停）。
struct AirMouseKinState {
    double vx = 0.0;       // 光标速度（像素/秒）
    double vy = 0.0;
    double fx = 0.0;       // 亚像素位移累积
    double fy = 0.0;
};

// 体感鼠标速度控制参数（由配置项填充，真机标定）。
struct AirMouseParams {
    double tau = 0.05;         // 速度环时间常数（秒），手停滑行 ≈ 3×tau
    double gain_x = 16.0;      // 左右（yaw）角速度→速度增益
    double gain_y = 16.0;      // 上下（pitch）角速度→速度增益
    bool invert_y = false;     // 是否反转 Y 轴
};

// 三段线性增益曲线参数（作用于 |omega|，真机标定值）：
//   微调段 |omega| < kAirMouseLowThresh   → factor = kAirMouseLowFactor  （压低，精准对位）
//   中段  kAirMouseLowThresh ≤ |omega| < kAirMouseHighThresh → 线性插值 kLowFactor→kHighFactor
//   甩动段 |omega| ≥ kAirMouseHighThresh  → factor = kAirMouseHighFactor （放大，跨屏）
// 微调段刻意宽（覆盖慢转 ≈0~25dps）+ factor 低：慢转不进中段被放大，精准对位图标。
// 真机标定迭代值（2026-07-04：拓宽微调段+降 factor 修复"慢转难精准对位"），详见 Doc/Plan/air-mouse-gain-curve.md。
constexpr double kAirMouseLowThresh = 15.0;   // ≈25dps 微调段上限（覆盖慢转，勿过窄致慢转进中段放大）
constexpr double kAirMouseHighThresh = 50.0;  // ≈83dps 甩动段下限
constexpr double kAirMouseLowFactor = 0.15;   // 微调段相对增益（压低，精准对位）
constexpr double kAirMouseHighFactor = 4.0;   // 甩动段相对增益（放大，跨屏）

// 三段线性增益因子（输入 |omega|，输出相对增益倍率）。纯函数，可单测拐点连续性。
double AirMouseGainFactor(double omega_abs);

struct AirMouseStepResult {
    int dx = 0;
    int dy = 0;
};

// 体感鼠标速度控制单次 step：
//   v_target = omega × gain × factor(|omega|) // 三段线性增益曲线（慢稳快猛）
//   v += (v_target - v) × (1 - exp(-dt/tau)) // 速度环一阶低通（平滑噪声）
//   fx += v × dt;  dx = trunc(fx)            // 亚像素累积
// state 原地更新。omega_is_stale=true 时 omega 视为 0（手停后 v_target=0，v 经 tau 归零）。
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x,
                                std::int16_t omega_y,
                                double dt_seconds,
                                bool omega_is_stale,
                                const AirMouseParams& params);

} // namespace voicestick
