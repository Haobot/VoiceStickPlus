#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace voicestick {

// 方向锁：防止手腕经过中立区时直接切换到反向光标。
// 每个轴独立维护，只有 |theta| 越过中立区死区才能锁定该方向，回到死区内才释放。
enum class AirMouseDirectionLock {
    kNone,      // 未锁定，光标停
    kNegative,  // 锁定负方向
    kPositive,  // 锁定正方向
};

// 体感鼠标运动学状态（速度环：目标速度由输入决定，输入为 0 即停）。
struct AirMouseKinState {
    double vx = 0.0;                    // 光标速度（像素/秒）
    double vy = 0.0;
    double fx = 0.0;                    // 亚像素位移累积
    double fy = 0.0;
    AirMouseDirectionLock lock_x = AirMouseDirectionLock::kNone;
    AirMouseDirectionLock lock_y = AirMouseDirectionLock::kNone;
};

// 三段线性增益曲线参数（运行期可变，支持热调参；默认值=真机标定值）。
//   微调段 |x| < low_thresh   → factor = low_factor  （压低，精准对位）
//   中段  low_thresh ≤ |x| < high_thresh → 线性插值 low_factor→high_factor
//   甩动段 |x| ≥ high_thresh  → factor = high_factor （放大，跨屏）
// x 为固件上报的缩放角速率（dps × AIR_MOUSE_REPORT_GAIN=4，见 bmi270.c），故阈值以同单位表达。
// 默认 100/333 对应物理拐点约 25/83 dps（与旧 15/50 @ SCALE=0.6 同一物理角速率，P1 去双重缩放后重标定），
// factor 0.15/4.0 不变。详见 Doc/Plan/air-mouse-gain-curve.md 与 air-mouse-still-tuning.md。热调参面板可实时改。
struct AirMouseCurveParams {
    double low_thresh  = 100.0;
    double high_thresh = 333.0;
    double low_factor  = 0.15;
    double high_factor = 4.0;
};

// 体感鼠标控制模式。
// kAngle：角度控制，速度命令由瞬时角速率 omega 直接映射为光标速度
//   （匀速转=匀速移，停转即停），增益曲线作用于 omega。由协调器在 AirMouseTick
//   把固件上报的 omega 作为 input.value 传入，避免积分转角 theta 无限增长导致失控。
// kRate：飞行摇杆/变化率控制，input.value 为积分转角 theta，映射为光标速度的变化率
//   （加速度），回中后速度保持并由摩擦衰减。
enum class AirMouseControlMode {
    kAngle,
    kRate,
};

// 体感鼠标速度控制参数（由配置项填充，真机标定）。
// 默认构造保持 kAngle，以便现有单元测试不依赖配置即可验证角度控制行为；
// 运行期由 AirMouseParamsFromConfig 根据配置设置为 kRate。
struct AirMouseParams {
    AirMouseControlMode control_mode = AirMouseControlMode::kAngle;
    double tau = 0.05;             // 速度环时间常数（秒），手停滑行 ≈ 3×tau（kAngle 模式有效）
    double gain_x = 16.0;          // 左右（yaw）输入→速度增益
    double gain_y = 16.0;          // 上下（pitch）输入→速度增益
    bool invert_y = false;         // 是否反转 Y 轴
    AirMouseCurveParams curve;     // 三段线性增益曲线（运行期可变，支持热调参）
    double neutral_deadzone = 3.0; // 方向锁中立区死区（角度），|theta| 小于此值时光标停并释放方向锁
    // 飞行摇杆模式（kRate）参数：theta 控制光标速度变化率。
    double rate_gain = 80.0;       // theta → 加速度增益
    double rate_friction = 0.05;   // 速度摩擦系数（1/s），omega=0 时速度衰减
    double rate_max_speed = 4000.0; // 速度上限（像素/秒）
};

// 单次 step 的输入：可为角速度（速度控制模型）或相对角度（角度控制模型）。
// is_angle 仅用于日志/调试语义区分，计算逻辑对两种输入一致：v_target = value × gain × factor(|value|)。
// value 用 int 而非 int16_t，避免测试和调用点大量 static_cast；实际 omega/theta 范围均远小于 int32。
struct AirMouseInput {
    int value_x = 0;
    int value_y = 0;
    bool is_angle = false;
};

// 三段线性增益因子（输入 |x| 与曲线参数，输出相对增益倍率）。纯函数，可单测拐点连续性与 curve 注入。
double AirMouseGainFactor(double x_abs, const AirMouseCurveParams& curve);

// 钳位曲线参数到合法范围（low_thresh<high_thresh、factor 界限、阈值界限）。纯函数。
// 配置解析与热调参 UI 均复用，防越界致曲线退化（如 low_thresh≥high_thresh 除零）。
AirMouseCurveParams AirMouseCurveClamp(AirMouseCurveParams curve);

// 方向锁：根据当前 theta 和已有锁状态，决定有效 theta。
// 若 |theta| <= deadzone 则释放锁并返回 0；
// 若未锁定则按 theta 符号锁定方向；
// 若锁定方向与 theta 符号冲突（异常过冲）则释放锁并返回 0。
double AirMouseApplyDirectionLock(double theta, AirMouseDirectionLock& lock, double deadzone);

// 控制模式名称转换（配置持久化用）。
std::string AirMouseControlModeName(AirMouseControlMode mode);
AirMouseControlMode AirMouseControlModeFromName(std::string_view name);

struct AirMouseStepResult {
    int dx = 0;
    int dy = 0;
};

// 体感鼠标速度控制单次 step：
//   v_target = input.value × gain × factor(|input.value|, curve) // 三段线性增益曲线（慢稳快猛）
//   v += (v_target - v) × (1 - exp(-dt/tau)) // 速度环一阶低通（平滑噪声）
//   fx += v × dt;  dx = trunc(fx)            // 亚像素累积
// state 原地更新。input_is_stale=true 时 input.value 视为 0（手停后 v_target=0，v 经 tau 归零）。
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                const AirMouseInput& input,
                                double dt_seconds,
                                bool input_is_stale,
                                const AirMouseParams& params);

} // namespace voicestick
