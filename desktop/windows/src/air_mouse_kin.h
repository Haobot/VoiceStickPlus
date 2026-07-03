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

// 三段线性增益曲线参数（运行期可变，支持热调参；默认值=真机标定值）。
//   微调段 |omega| < low_thresh   → factor = low_factor  （压低，精准对位）
//   中段  low_thresh ≤ |omega| < high_thresh → 线性插值 low_factor→high_factor
//   甩动段 |omega| ≥ high_thresh  → factor = high_factor （放大，跨屏）
// 默认值（15/50/0.15/4.0）为 2026-07-04 真机标定迭代值（拓宽微调段+降 factor 修复"慢转难精准对位"），
// 详见 Doc/Plan/air-mouse-gain-curve.md 与 air-mouse-still-tuning.md。热调参面板可实时改。
struct AirMouseCurveParams {
    double low_thresh  = 15.0;
    double high_thresh = 50.0;
    double low_factor  = 0.15;
    double high_factor = 4.0;
};

// 体感鼠标速度控制参数（由配置项填充，真机标定）。
struct AirMouseParams {
    double tau = 0.05;         // 速度环时间常数（秒），手停滑行 ≈ 3×tau
    double gain_x = 16.0;      // 左右（yaw）角速度→速度增益
    double gain_y = 16.0;      // 上下（pitch）角速度→速度增益
    bool invert_y = false;     // 是否反转 Y 轴
    AirMouseCurveParams curve; // 三段线性增益曲线（运行期可变，支持热调参）
};

// 三段线性增益因子（输入 |omega| 与曲线参数，输出相对增益倍率）。纯函数，可单测拐点连续性与 curve 注入。
double AirMouseGainFactor(double omega_abs, const AirMouseCurveParams& curve);

// 钳位曲线参数到合法范围（low_thresh<high_thresh、factor 界限、阈值界限）。纯函数。
// 配置解析与热调参 UI 均复用，防越界致曲线退化（如 low_thresh≥high_thresh 除零）。
AirMouseCurveParams AirMouseCurveClamp(AirMouseCurveParams curve);

struct AirMouseStepResult {
    int dx = 0;
    int dy = 0;
};

// 体感鼠标速度控制单次 step：
//   v_target = omega × gain × factor(|omega|, curve) // 三段线性增益曲线（慢稳快猛）
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
