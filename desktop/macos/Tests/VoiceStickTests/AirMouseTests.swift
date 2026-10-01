import Foundation
import VoiceStickCore

// 体感鼠标单测（对应 Windows core_tests.cc TestCoordinatorAirMouse* 系列中
// 可下沉 core 的纯逻辑断言 + air_mouse_kin 语义；协调器级门控在 App 模块，runner
// 只链接 VoiceStickCore，行为对齐靠移植时逐行对照保证）。

func runAirMouseKinTests() {
    // ---- 增益曲线：单调、有界、特征点 ----
    let curve = AirMouseCurveParams()
    check(
        AirMouseKin.gainFactor(0, curve: curve) < AirMouseKin.gainFactor(100, curve: curve) &&
        AirMouseKin.gainFactor(100, curve: curve) < AirMouseKin.gainFactor(400, curve: curve),
        "gainFactor 单调递增"
    )
    check(
        AirMouseKin.gainFactor(0, curve: curve) >= curve.lowFactor * 0.9 &&
        AirMouseKin.gainFactor(10_000, curve: curve) <= curve.highFactor * 1.1,
        "gainFactor 有界 [low, high]"
    )
    let mid = (curve.lowThresh + curve.highThresh) / 2
    let midFactor = AirMouseKin.gainFactor(mid, curve: curve)
    let expectedMid = (curve.lowFactor + curve.highFactor) / 2
    check(abs(midFactor - expectedMid) < 0.05, "gainFactor 中点≈(low+high)/2")
    // 曲线注入生效（低速段 factor 由 low_factor 主导）。
    let steep = AirMouseCurveParams(lowThresh: 100, highThresh: 333, lowFactor: 0.05, highFactor: 6.0)
    check(
        AirMouseKin.gainFactor(5, curve: steep) < AirMouseKin.gainFactor(5, curve: curve),
        "gainFactor 曲线参数注入生效（低速段 low_factor 更低则压得更低）"
    )

    // ---- 曲线钳位：范围与 low<high 约束 ----
    var clamped = AirMouseKin.curveClamp(AirMouseCurveParams(lowThresh: 0, highThresh: 9999, lowFactor: 0, highFactor: 99))
    check(clamped.lowThresh >= 1.0 && clamped.highThresh <= 800.0, "curveClamp 阈值夹到 [1,800]")
    check(clamped.lowFactor >= 0.05 && clamped.highFactor <= 6.0, "curveClamp 因子夹到 [0.05,6]")
    clamped = AirMouseKin.curveClamp(AirMouseCurveParams(lowThresh: 300, highThresh: 300, lowFactor: 0.25, highFactor: 4.0))
    check(clamped.lowThresh < clamped.highThresh, "curveClamp low>=high 退 low=high-1")
    // 配置钳位：越界回落默认（对齐 Windows「回落默认值」语义而非夹边界）。
    checkEqual(AirMouseKin.tauClamp(0.001), 0.05, "tauClamp 越界回默认")
    checkEqual(AirMouseKin.tauClamp(0.2), 0.2, "tauClamp 合法值保留")
    checkEqual(AirMouseKin.neutralDeadzoneClamp(99), 3.0, "neutralDeadzoneClamp 越界回默认")
    checkEqual(AirMouseKin.rateGainClamp(5), 80.0, "rateGainClamp 越界回默认")
    checkEqual(AirMouseKin.rateFrictionClamp(2.0), 0.05, "rateFrictionClamp 越界回默认")
    checkEqual(AirMouseKin.rateMaxSpeedClamp(100), 4000.0, "rateMaxSpeedClamp 越界回默认")

    // ---- 控制模式名称往返（未知名回落 rate，对齐 Windows）----
    checkEqual(AirMouseControlMode.angle.name, "angle", "controlMode angle 名称")
    checkEqual(AirMouseControlMode.fromName("rate"), .rate, "controlMode rate 解析")
    checkEqual(AirMouseControlMode.fromName("bogus"), .rate, "controlMode 未知名回落 rate")
    checkEqual(AirMouseControlMode.fromName("angle"), .angle, "controlMode angle 解析")

    // ---- 方向锁：死区释放 / 锁定 / 过冲释放 ----
    var lock = AirMouseDirectionLock.none
    checkEqual(AirMouseKin.applyDirectionLock(0.0, lock: &lock, deadzone: 3.0), 0.0, "方向锁死区内返回 0")
    checkEqual(lock, .none, "方向锁死区内释放")
    checkEqual(AirMouseKin.applyDirectionLock(10.0, lock: &lock, deadzone: 3.0), 10.0, "越死区锁定正方向")
    checkEqual(lock, .positive, "锁状态=正")
    checkEqual(AirMouseKin.applyDirectionLock(-10.0, lock: &lock, deadzone: 3.0), 0.0, "同锁反向（过冲）返回 0")
    checkEqual(lock, .none, "过冲释放锁")
    checkEqual(AirMouseKin.applyDirectionLock(-10.0, lock: &lock, deadzone: 3.0), -10.0, "重新锁定负方向")

    // ---- step：angle 模式速度环 ----
    var params = AirMouseParams()
    params.controlMode = .angle
    params.gainX = 10 * 48.0   // 灵敏度 10 档
    params.gainY = 10 * 48.0
    var state = AirMouseKinState()
    let dt = 16.0 / 1000.0
    var totalDx = 0
    for _ in 0..<50 {   // 0.8s @60Hz，omega=160（40dps×4，真机典型手腕转动）
        let r = AirMouseKin.step(&state, input: AirMouseInput(valueX: 160, valueY: 0), dtSeconds: dt, inputIsStale: false, params: params)
        totalDx += r.dx
    }
    // 对齐 Windows TestCoordinatorAirMouseHighSensitivityRealisticSpeed：0.8s 位移充足。
    check(totalDx >= 4000, "angle 模式 0.8s 真机典型角速率位移 ≥4000px（实际 \(totalDx)）")

    // P0 回归：持续匀速转动速度恒定（旧 theta 套增益曲线正反馈失控）。
    for _ in 0..<30 {
        _ = AirMouseKin.step(&state, input: AirMouseInput(valueX: 160, valueY: 0), dtSeconds: dt, inputIsStale: false, params: params)
    }
    let vxEarly = state.vx
    for _ in 0..<270 {   // 再转 4.5s
        _ = AirMouseKin.step(&state, input: AirMouseInput(valueX: 160, valueY: 0), dtSeconds: dt, inputIsStale: false, params: params)
    }
    check(abs(state.vx - vxEarly) / vxEarly < 0.05, "持续匀速转动速度恒定无失控")

    // ---- step：stale 输入归零，速度环经 tau 滑行停止 ----
    var stopState = AirMouseKinState()
    for _ in 0..<30 {
        _ = AirMouseKin.step(&stopState, input: AirMouseInput(valueX: 160, valueY: 0), dtSeconds: dt, inputIsStale: false, params: params)
    }
    check(stopState.vx > 100, "stale 前速度已建立")
    for _ in 0..<50 {   // ~3×tau 后应停
        _ = AirMouseKin.step(&stopState, input: AirMouseInput(valueX: 160, valueY: 0), dtSeconds: dt, inputIsStale: true, params: params)
    }
    check(abs(stopState.vx) < 1.0, "stale 后速度环经 tau 归零")

    // ---- step：invertY ----
    var invertParams = AirMouseParams()
    invertParams.invertY = true
    invertParams.neutralDeadzone = 0.1   // 让小输入也能出锁
    var invertState = AirMouseKinState()
    for _ in 0..<30 {
        _ = AirMouseKin.step(&invertState, input: AirMouseInput(valueX: 0, valueY: 50), dtSeconds: dt, inputIsStale: false, params: invertParams)
    }
    check(invertState.vy < 0, "invertY 翻转 Y 速度符号")

    // ---- step：rate 模式摩擦衰减 + 速度上限 ----
    var rateParams = AirMouseParams()
    rateParams.controlMode = .rate
    rateParams.rateMaxSpeed = 500.0
    var rateState = AirMouseKinState()
    for _ in 0..<600 {   // 10s 持续偏转 theta=50
        _ = AirMouseKin.step(&rateState, input: AirMouseInput(valueX: 50, valueY: 0, isAngle: true), dtSeconds: dt, inputIsStale: false, params: rateParams)
    }
    check(rateState.vx <= rateParams.rateMaxSpeed + 1.0, "rate 模式速度不超上限")
    check(rateState.vx > 100.0, "rate 模式偏转期间速度持续增长")
    rateParams.rateFriction = 0.5   // 满摩擦下回中 10s 内自然停车
    for _ in 0..<600 {   // 回中（theta=0）摩擦衰减自然停下
        _ = AirMouseKin.step(&rateState, input: AirMouseInput(valueX: 0, valueY: 0, isAngle: true), dtSeconds: dt, inputIsStale: false, params: rateParams)
    }
    check(abs(rateState.vx) < 5.0, "rate 模式回中后摩擦衰减停车")

    // ---- 亚像素累积：低速下小数位移不丢 ----
    var subParams = AirMouseParams()
    subParams.gainX = 1.0
    subParams.gainY = 1.0
    subParams.curve = AirMouseCurveParams(lowThresh: 1, highThresh: 800, lowFactor: 0.05, highFactor: 6.0)
    subParams.neutralDeadzone = 0.1
    var subState = AirMouseKinState()
    var subDx = 0
    for _ in 0..<2000 {   // omega=4：v_target≈4×0.05≈0.2px/s，单步位移远小于 1px
        let r = AirMouseKin.step(&subState, input: AirMouseInput(valueX: 4, valueY: 0), dtSeconds: dt, inputIsStale: false, params: subParams)
        subDx += r.dx
    }
    check(subDx > 0, "亚像素累积：低速长时仍有非零位移")
}

func runAirMouseProtocolTests() {
    // ---- motion 帧解析（对齐 Windows ParseMotionFrame）----
    func frame(_ dx: Int16, _ dy: Int16) -> Data {
        var data = Data([1, 0x11])
        appendLE16(&data, UInt16(bitPattern: dx))
        appendLE16(&data, UInt16(bitPattern: dy))
        return data
    }
    func appendLE16(_ data: inout Data, _ value: UInt16) {
        data.append(UInt8(value & 0xff))
        data.append(UInt8((value >> 8) & 0xff))
    }

    checkEqual(BleProtocol.parseMotionFrame(frame(100, -50))?.dx, 100, "motion 帧解析 dx")
    checkEqual(BleProtocol.parseMotionFrame(frame(100, -50))?.dy, -50, "motion 帧解析 dy")
    checkEqual(BleProtocol.parseMotionFrame(frame(-32768, 32767))?.dx, -32768, "motion 帧解析 dx 边界")
    checkNil(BleProtocol.parseMotionFrame(Data([1, 0x10, 4, 0, 1, 0])), "motion 帧拒收 type=0x10")
    checkNil(BleProtocol.parseMotionFrame(Data([2, 0x11, 4, 0, 1, 0])), "motion 帧拒收 version=2")
    checkNil(BleProtocol.parseMotionFrame(Data([1, 0x11, 4, 0])), "motion 帧拒收长度不足")
    checkNotNil(BleProtocol.parseMotionFrame(Data([1, 0x11, 0, 0, 1, 0, 99])), "motion 帧容忍多余尾字节（对齐 Windows ≥6 判定）")

    // ---- air_mouse_enabled 下发帧 ----
    let enableData = BleProtocol.airMouseEnabledPayload(enabled: true)
    let enableJSON = (try? JSONSerialization.jsonObject(with: enableData)) as? [String: Any]
    checkEqual(enableJSON?["event"] as? String, "air_mouse_enabled", "air_mouse_enabled 事件名")
    checkEqual(enableJSON?["enabled"] as? Bool, true, "air_mouse_enabled=true")
    let disableData = BleProtocol.airMouseEnabledPayload(enabled: false)
    let disableJSON = (try? JSONSerialization.jsonObject(with: disableData)) as? [String: Any]
    checkEqual(disableJSON?["enabled"] as? Bool, false, "air_mouse_enabled=false")

    // ---- 与 state_tx JSON 链互不串扰：motion 帧不会被 parseStateEvent 误吞 ----
    checkNil(BleProtocol.parseStateEvent(frame(10, 10)), "motion 帧不落入 StateEvent 解析")
}
