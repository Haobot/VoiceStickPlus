import Foundation

// 体感鼠标运动学（自 Windows air_mouse_kin.cc 1:1 移植，纯逻辑无平台依赖）。
// 输入为固件预处理后的陀螺角速率（state_tx 0x11 motion 帧，dps×REPORT_GAIN=4），
// 输出为光标整数位移；速度环一阶低通 + sigmoid 增益曲线 + 方向锁 + 亚像素累积。

/// 方向锁：防止手腕经过中立区时直接切换到反向光标。
/// 每个轴独立维护，只有 |theta| 越过中立区死区才能锁定该方向，回到死区内才释放。
public enum AirMouseDirectionLock: Equatable {
    case none      // 未锁定，光标停
    case negative  // 锁定负方向
    case positive  // 锁定正方向
}

/// 体感鼠标运动学状态（速度环：目标速度由输入决定，输入为 0 即停）。
public struct AirMouseKinState: Equatable {
    public var vx: Double = 0.0   // 光标速度（像素/秒）
    public var vy: Double = 0.0
    public var fx: Double = 0.0   // 亚像素位移累积
    public var fy: Double = 0.0
    public var lockX: AirMouseDirectionLock = .none
    public var lockY: AirMouseDirectionLock = .none

    public init() {}
}

/// 平滑（sigmoid）增益曲线参数（运行期可变；默认值=真机标定值）。
/// factor 为 |x| 的 sigmoid：|x|→0 趋近 low_factor（压低，精准对位），
/// |x|→∞ 趋近 high_factor（放大，跨屏），全程连续可微、无折角感。
/// x 为固件上报的缩放角速率（dps × 4），默认 100/333 对应约 25/83 dps。
public struct AirMouseCurveParams: Equatable {
    public var lowThresh: Double = 100.0
    public var highThresh: Double = 333.0
    public var lowFactor: Double = 0.25
    public var highFactor: Double = 4.0

    public init() {}
    public init(lowThresh: Double, highThresh: Double, lowFactor: Double, highFactor: Double) {
        self.lowThresh = lowThresh
        self.highThresh = highThresh
        self.lowFactor = lowFactor
        self.highFactor = highFactor
    }
}

/// 体感鼠标控制模式。
/// angle：速度命令由瞬时角速率 omega 直接映射为光标速度（匀速转=匀速移，停转即停）。
/// rate：飞行摇杆/变化率控制，theta 映射为光标速度的变化率（加速度），回中后速度保持
/// 并由摩擦衰减。
public enum AirMouseControlMode: Equatable {
    case angle
    case rate

    public init?(name: String) {
        switch name {
        case "angle": self = .angle
        case "rate": self = .rate
        default: return nil
        }
    }

    /// 配置持久化用名称（对齐 Windows AirMouseControlModeName/FromName：未知名回 rate）。
    public var name: String {
        switch self {
        case .angle: return "angle"
        case .rate: return "rate"
        }
    }

    public static func fromName(_ name: String) -> AirMouseControlMode {
        AirMouseControlMode(name: name) ?? .rate
    }
}

/// 体感鼠标速度控制参数（由配置项填充，真机标定）。
public struct AirMouseParams: Equatable {
    public var controlMode: AirMouseControlMode = .angle
    public var tau: Double = 0.05              // 速度环时间常数（秒）
    public var gainX: Double = 16.0            // 左右（yaw）输入→速度增益
    public var gainY: Double = 16.0            // 上下（pitch）输入→速度增益
    public var invertY: Bool = false
    public var curve = AirMouseCurveParams()
    public var neutralDeadzone: Double = 3.0   // 方向锁中立区死区
    // 飞行摇杆模式（rate）参数。
    public var rateGain: Double = 80.0         // theta → 加速度增益
    public var rateFriction: Double = 0.05     // 速度摩擦系数（1/s）
    public var rateMaxSpeed: Double = 4000.0   // 速度上限（像素/秒）

    public init() {}
}

/// 单次 step 的输入：可为角速度（速度控制模型）或相对角度（角度控制模型）。
/// value 用 Int 而非 Int16（对齐 Windows：避免调用点大量转换）。
public struct AirMouseInput {
    public var valueX: Int = 0
    public var valueY: Int = 0
    public var isAngle: Bool = false

    public init(valueX: Int = 0, valueY: Int = 0, isAngle: Bool = false) {
        self.valueX = valueX
        self.valueY = valueY
        self.isAngle = isAngle
    }
}

public struct AirMouseStepResult: Equatable {
    public var dx: Int = 0
    public var dy: Int = 0

    public init(dx: Int = 0, dy: Int = 0) {
        self.dx = dx
        self.dy = dy
    }
}

public enum AirMouseKin {
    /// 平滑 sigmoid 增益因子：微调段压低、甩动段放大（慢稳快猛），全程连续可微。
    /// factor = low + (high-low)·0.5·(1 + tanh((|x|-mid)/width))。
    public static func gainFactor(_ xAbs: Double, curve: AirMouseCurveParams) -> Double {
        let a = abs(xAbs)
        let mid = 0.5 * (curve.lowThresh + curve.highThresh)
        var half = 0.5 * (curve.highThresh - curve.lowThresh)
        if half < 1e-6 { half = 1.0 }  // low≈high 退化保护（正常由 clamp 保证 low<high）
        return curve.lowFactor +
            (curve.highFactor - curve.lowFactor) * 0.5 * (1.0 + tanh((a - mid) / half))
    }

    /// 钳位曲线参数到合法范围（对齐 Windows AirMouseCurveClamp）：
    /// 阈值单位为固件缩放角速率，上限放宽到 200/800 容纳自定义标定。
    public static func curveClamp(_ curve: AirMouseCurveParams) -> AirMouseCurveParams {
        var c = curve
        c.lowThresh = min(max(c.lowThresh, 1.0), 200.0)
        c.highThresh = min(max(c.highThresh, 50.0), 800.0)
        if c.lowThresh >= c.highThresh {
            c.lowThresh = c.highThresh - 1.0
        }
        c.lowFactor = min(max(c.lowFactor, 0.05), 0.5)
        c.highFactor = min(max(c.highFactor, 2.0), 6.0)
        return c
    }

    /// 方向锁：|theta| <= deadzone 释放锁并返回 0；未锁定时越过死区才按符号锁定；
    /// 已锁方向与 theta 符号冲突（异常过冲）时释放锁并返回 0。
    public static func applyDirectionLock(
        _ theta: Double, lock: inout AirMouseDirectionLock, deadzone: Double
    ) -> Double {
        if abs(theta) <= deadzone {
            lock = .none
            return 0.0
        }
        if lock == .none {
            lock = theta > 0.0 ? .positive : .negative
            return theta
        }
        let positive = theta > 0.0
        let lockedPositive = lock == .positive
        if positive != lockedPositive {
            lock = .none
            return 0.0
        }
        return theta
    }

    // ---- 配置钳位（对齐 Windows：越界/非法回落默认值，而非夹到边界）----

    /// 速度环时间常数 [0.02, 0.5]，越界回落默认 0.05（手停即停）。
    public static func tauClamp(_ value: Double) -> Double {
        (value > 0.0 && value >= 0.02 && value <= 0.5) ? value : 0.05
    }

    /// 方向锁中立区死区 [1.0, 10.0]，越界回落默认 3.0。
    public static func neutralDeadzoneClamp(_ value: Double) -> Double {
        (value > 0.0 && value >= 1.0 && value <= 10.0) ? value : 3.0
    }

    /// 飞行摇杆加速度增益 [10, 500]，越界回落默认 80。
    public static func rateGainClamp(_ value: Double) -> Double {
        (value > 0.0 && value >= 10.0 && value <= 500.0) ? value : 80.0
    }

    /// 速度摩擦系数 [0, 0.5]，越界回落默认 0.05。
    public static func rateFrictionClamp(_ value: Double) -> Double {
        (value >= 0.0 && value <= 0.5) ? value : 0.05
    }

    /// 速度上限 [500, 8000]，越界回落默认 4000。
    public static func rateMaxSpeedClamp(_ value: Double) -> Double {
        (value > 0.0 && value >= 500.0 && value <= 8000.0) ? value : 4000.0
    }

    /// 体感鼠标速度控制单次 step：
    ///   v_target = input × gain × factor(|input|, curve)
    ///   v += (v_target - v) × (1 - exp(-dt/tau))
    ///   fx += v × dt;  dx = trunc(fx)
    /// input_is_stale=true 时输入视为 0（手停后 v_target=0，v 经 tau 归零）。state 原地更新。
    @discardableResult
    public static func step(
        _ state: inout AirMouseKinState,
        input: AirMouseInput,
        dtSeconds: Double,
        inputIsStale: Bool,
        params: AirMouseParams
    ) -> AirMouseStepResult {
        var ix = inputIsStale ? 0.0 : Double(input.valueX)
        var iy = inputIsStale ? 0.0 : Double(input.valueY)

        // 方向锁：必须先回到中立区死区，才能切换到反向。
        ix = applyDirectionLock(ix, lock: &state.lockX, deadzone: params.neutralDeadzone)
        iy = applyDirectionLock(iy, lock: &state.lockY, deadzone: params.neutralDeadzone)

        if params.invertY { iy = -iy }

        let dt = dtSeconds
        if params.controlMode == .angle {
            let vTargetX = ix * params.gainX * gainFactor(ix, curve: params.curve)
            let vTargetY = iy * params.gainY * gainFactor(iy, curve: params.curve)
            let alpha = 1.0 - exp(-dt / params.tau)
            state.vx += (vTargetX - state.vx) * alpha
            state.vy += (vTargetY - state.vy) * alpha
        } else {
            let ax = ix * params.rateGain * gainFactor(ix, curve: params.curve)
            let ay = iy * params.rateGain * gainFactor(iy, curve: params.curve)
            state.vx += ax * dt
            state.vy += ay * dt
            let frictionDecay = exp(-params.rateFriction * dt)
            state.vx *= frictionDecay
            state.vy *= frictionDecay
            state.vx = min(max(state.vx, -params.rateMaxSpeed), params.rateMaxSpeed)
            state.vy = min(max(state.vy, -params.rateMaxSpeed), params.rateMaxSpeed)
        }

        // 亚像素累积：小 v 时保留小数，够 1px 才输出，避免 round 丢失精细移动。
        state.fx += state.vx * dt
        state.fy += state.vy * dt
        let dxInt = Int(state.fx)
        let dyInt = Int(state.fy)
        state.fx -= Double(dxInt)
        state.fy -= Double(dyInt)

        return AirMouseStepResult(dx: dxInt, dy: dyInt)
    }
}
