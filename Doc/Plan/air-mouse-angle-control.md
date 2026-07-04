# 体感鼠标：角度控制（Angle-to-Velocity）改造（路径 1）

## 1. 背景与目标

### 1.1 现状

当前体感鼠标采用**角速度控制模型**（`Doc/Plan/air-mouse-velocity-control.md`）：

```
固件: out = clamp((ω-bias) × SCALE, ±127)    // 20ms 一帧，上报的是角速度代理
Windows: v_target = omega × gain × factor(|omega|)
         v += (v_target - v) × (1 - exp(-dt/tau))
         fx += v × dt; dx = trunc(fx)
```

特性：手腕转动 → 光标移动；手腕停住 → `omega=0` → `v_target=0` → 光标经 `tau` 后停止。

### 1.2 问题

用户反馈："保持一个转角以后，光标能够一直持续移动，而不是只移动一段距离。"

角速度模型下，手腕**保持某个固定倾斜角**时，陀螺仪读数接近 0（因为没有继续转动），光标会停下来。用户必须持续转动手腕才能跨屏，累且不符合直觉。

### 1.3 目标

改为**角度控制模型（angle-to-velocity）**：

- 设备偏离**中立姿态**的角度 → 光标速度（角度越大速度越快）。
- 保持某个偏离角度 → 光标持续同向移动。
- 设备回到中立姿态附近 → 角度快速归零 → 光标停止。

路径 1 优先在桌面端实现，不改固件、不改 BLE 协议，快速验证手感。

---

## 2. 现状链路回顾

```text
BMI270 gyro(dps) ─20ms─► bmi270_air_mouse_poll
                              │
                              ▼
            去偏 → 死区(3dps) → SCALE(0.6) → clamp(±127)
                              │
                              ▼
            BLE state_tx(0x5102) type=0x11 [6B: version+type+int16 dx+int16 dy]
                              │
                              ▼
            Windows: ble_central_win ValueChanged → ParseMotionFrame
                     → DispatchToUiThread → HandleMotionEvent
                     → 更新 last_omega_x/y
                     → 60Hz AirMouseTick → AirMouseStep → MoveMouse
```

- 当前 `MotionEvent.dx/dy` 是固件处理后的**角速度代理**（int16，±127）。
- `HandleMotionEvent` 只更新 `last_omega`，不做积分。
- `AirMouseStep` 把 `omega` 当速度源。

路径 1 中，我们**把 `omega` 在桌面端积分成相对角度 `theta`**，再映射为光标速度。

---

## 3. 设计方案

### 3.1 核心模型

桌面端维护相对角度 `theta_x`、`theta_y`（单位与固件上报单位一致，即 `omega` 的单位乘以秒）：

```
// HandleMotionEvent 收到新 omega 时积分（dt = 当前帧与上一帧时间差）
theta += omega * dt

// AirMouseTick 每 16.67ms (60Hz) 调用 AirMouseStep
v_target = theta * gain * factor(|theta|)
v += (v_target - v) * (1 - exp(-dt/tau))
dx = round(v * dt)
```

与角速度模型的区别：输入从 `omega` 变成 `theta`。

### 3.2 中立姿态与快速归零

**中立姿态**：进入体感模式瞬间，`theta_x = theta_y = 0`。之后每次收到的 `omega` 都相对该起点积分。

**快速归零条件**（同时满足）：
1. `omega` 持续 stale（超过 `kAirMouseOmegaStaleAge = 30ms` 无新帧），或
2. 最新 `|omega_x|` 和 `|omega_y|` 都小于死区阈值 `kAirMouseAngleDeadzone`（初值 0.5，对应固件侧约 1~2 dps 经 SCALE 后的值）。

满足时，将 `theta` 直接归零（不是慢衰减）。

> 为什么敢直接归零？因为固件侧已有零偏校准、死区 3dps、静止判据滞回，静止时 `omega` 本来就是 0。桌面端再对积分角度做一次死区归零，不会导致正常移动中断。

### 3.3 状态字段变更

`AirMouseDeviceState`（`voice_stick_coordinator.h`）新增：

```cpp
struct AirMouseDeviceState {
    // ... 现有字段 ...
    double theta_x = 0.0;
    double theta_y = 0.0;
};
```

`AirMouseKinState`（`air_mouse_kin.h`）是否需要改？

**不改**。`AirMouseKinState` 继续维护 `vx/vy/fx/fy`，速度环纯函数仍然只负责 `omega/theta → v_target → dx` 的映射。角度积分放在协调器层，因为需要 `last_omega_t` 算 `dt` 和 stale 判断，而这些上下文在协调器已有。

### 3.4 `AirMouseStep` 改造

当前签名：

```cpp
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x, std::int16_t omega_y,
                                double dt_seconds, bool omega_is_stale,
                                const AirMouseParams& params);
```

改为同时支持两种输入源：

```cpp
struct AirMouseInput {
    std::int16_t value_x = 0;   // omega 或 theta，取决于 mode
    std::int16_t value_y = 0;
    bool is_angle = false;      // false=速度模型(omega), true=角度模型(theta)
};

AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                const AirMouseInput& input,
                                double dt_seconds,
                                bool input_is_stale,
                                const AirMouseParams& params);
```

内部逻辑：

```cpp
const double ix = input_is_stale ? 0.0 : input.value_x;
const double iy = input_is_stale ? 0.0 : input.value_y;

// 角度模式下，静止时 input 已为 0（协调器层归零后传入），速度自然归零。
const double v_target_x = ix * params.gain_x * AirMouseGainFactor(ix, params.curve);
const double v_target_y = iy * params.gain_y * AirMouseGainFactor(iy, params.curve);
```

> 也可保留原签名，把角度值直接作为 `omega_x/y` 传入（`AirMouseStep` 本身不区分语义）。但为了代码可读性和未来扩展，推荐用 `AirMouseInput` 显式区分。

### 3.5 协调器层改造

`HandleMotionEvent`：

```cpp
void VoiceStickCoordinator::HandleMotionEvent(const MotionEvent& event,
                                              const std::string& device_id) {
    if (!IsAirMouseActive(device_id)) return;
    auto& state = air_mouse_states_[device_id];

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - state.last_omega_t).count();

    // 角度模型：积分 omega 得 theta。
    state.theta_x += event.dx * dt;
    state.theta_y += event.dy * dt;

    state.last_omega_x = event.dx;
    state.last_omega_y = event.dy;
    state.last_omega_t = now;
}
```

`AirMouseTick`：

```cpp
for (auto& [device_id, state] : air_mouse_states_) {
    const double omega_age = std::chrono::duration<double>(now - state.last_omega_t).count();
    const bool stale = omega_age > stale_age_sec;

    // 快速归零：stale 或 omega 很小时，theta 归 0。
    if (stale || (std::abs(state.last_omega_x) < kAngleDeadzone &&
                  std::abs(state.last_omega_y) < kAngleDeadzone)) {
        state.theta_x = 0.0;
        state.theta_y = 0.0;
    }

    AirMouseInput input;
    input.value_x = static_cast<std::int16_t>(ClampAngleToInt16(state.theta_x));
    input.value_y = static_cast<std::int16_t>(ClampAngleToInt16(state.theta_y));
    input.is_angle = true;

    const auto result = AirMouseStep(state.kin, input, dt, stale, params);
    if (result.dx != 0 || result.dy != 0) {
        input_injector_->MoveMouse(result.dx, result.dy);
    }
}
```

`ClampAngleToInt16`：防止 `theta` 异常累积超过 int16 范围，限制到 ±32767。

### 3.6 参数调整

| 参数 | 当前值 | 新值/说明 |
|---|---|---|
| `gain_x/y` | `sensitivity × 16` | 角度模型下需重新标定，初值建议 `sensitivity × 80`（角度值小，需要更高增益补偿） |
| `tau` | 0.05 | 可保持，角度模型下手停即停主要靠 theta 归零 |
| `curve` | 15/50/0.15/4.0 | 作用于 `theta` 的绝对值 |
| 新增 `kAirMouseAngleDeadzone` | — | 0.5（静态常量，不归入配置项） |

`gain` 初值待定，以真机标定为准。路径 1 先硬编码或复用 `air_mouse_sensitivity_x/y` 但调整映射系数。

---

## 4. 数据模型与 API 契约

### 4.1 新增/修改的结构

```cpp
// air_mouse_kin.h
struct AirMouseInput {
    std::int16_t value_x = 0;
    std::int16_t value_y = 0;
    bool is_angle = false;
};

// voice_stick_coordinator.h
struct AirMouseDeviceState {
    AirMouseKinState kin;
    std::chrono::steady_clock::time_point last_omega_t;
    std::int16_t last_omega_x = 0;
    std::int16_t last_omega_y = 0;
    double theta_x = 0.0;   // 新增
    double theta_y = 0.0;   // 新增
};
```

### 4.2 `AirMouseStep` 契约

- 输入：`AirMouseInput`（含 `value_x/y` 与 `is_angle` 标志）、`dt_seconds`、`input_is_stale`、`AirMouseParams`。
- 输出：`AirMouseStepResult{dx, dy}`。
- 行为：`v_target = value × gain × factor(|value|)`；`input_is_stale` 时 `value=0`；速度环平滑后输出整数位移。
- `is_angle` 当前仅用于日志/调试区分语义，不改变计算（计算对 omega/theta 无差别）。

### 4.3 协调器层契约

- `HandleMotionEvent`：只更新该设备的 `last_omega` 和 `theta`（积分）。
- `AirMouseTick`：负责 stale 判断、theta 归零、调用 `AirMouseStep`、注入光标位移。
- `ToggleAirMouse`：进入时初始化 `theta=0`；退出时清理状态。

---

## 5. 边界条件与异常流

| 场景 | 行为 | 原因 |
|---|---|---|
| 进入体感瞬间 | `theta=0`，当前姿态为中立姿态 | 用户自然握持姿势就是零点 |
| 缓慢转动手腕保持 30° | `theta` 稳定在 30° 对应值 → 光标持续移动 | 核心需求 |
| 回正到中立姿态 | `omega` 接近 0 → `theta` 归零 → 光标停止 | 替代旧方案的 2s 慢衰减 |
| 快速甩动手腕后停住 | `theta` 先冲到较大值，停手后快速归零 | `tau` 提供缓停，theta 归零提供最终停止 |
| BLE 丢帧/卡顿 | `omega_is_stale=true` → 本 tick theta 归零 → 光标停止 | 防断连/卡顿后光标乱跑 |
| theta 异常累积 | `ClampAngleToInt16` 限制在 int16 范围 | 防止积分溢出 |
| 多设备切换 | 每设备独立 `AirMouseDeviceState`，`theta` 互不干扰 | 现有按 device_id 隔离 |

---

## 6. 测试计划（TDD）

### 6.1 纯函数 `AirMouseStep`

- `TestAirMouseStepAngleModeFollowsTheta`：固定 `theta=10` 连续 step，`v` 收敛到 `theta×gain×factor(theta)`，输出稳定持续移动。
- `TestAirMouseStepAngleModeStopsOnZeroTheta`：`theta=0` 连续 step，断言 `|vx|<1.0`。
- `TestAirMouseStepAngleModeNegative`：`theta=-20`，`vx` 为负且对称。
- `TestAirMouseStepAngleModeCurveShape`：`theta=5/20/80` 三段输出符合曲线（低/中/高）。

### 6.2 协调器集成

- `TestCoordinatorAngleIntegratesToSustainedMovement`：模拟连续 1s 的 `omega=10` motion 帧，断言 AirMouseTick 期间光标持续向同方向移动，总位移 > 仅角速度模型下的位移。
- `TestCoordinatorAngleResetsWhenOmegaZero`：转动 0.5s 后连续 0.5s 上报 `omega=0`，断言后续 tick 位移快速衰减到 0。
- `TestCoordinatorAngleResetsOnStale`：转动后停止发帧，stale 后 theta 归零，光标停止。
- `TestCoordinatorAngleResetOnToggle`：退出再进入体感，`theta` 从 0 开始。

### 6.3 需调整的旧测试

- 现有角速度模型测试若断言 `v≈omega×gain`，需改为传入 `theta` 作为输入，或保留 omega 语义但新增 angle 模式测试。
- `AirMouseStep` 签名变更后，所有调用点同步更新。

---

## 7. 文件改动清单

| 文件 | 改动 |
|---|---|
| `Doc/Plan/air-mouse-angle-control.md` | 新增本文档 |
| `desktop/windows/src/air_mouse_kin.h` | 新增 `AirMouseInput`；`AirMouseStep` 签名改为接收 `AirMouseInput` |
| `desktop/windows/src/air_mouse_kin.cc` | `AirMouseStep` 适配新签名（计算逻辑不变） |
| `desktop/windows/src/voice_stick_coordinator.h` | `AirMouseDeviceState` 新增 `theta_x/y` |
| `desktop/windows/src/voice_stick_coordinator.cc` | `HandleMotionEvent` 积分 theta；`AirMouseTick` 归零 theta 并传入 angle 模式 |
| `desktop/windows/tests/core_tests.cc` | 新增/调整 angle 模式测试 |
| `CLAUDE.md` / `AGENTS.md` | 体感鼠标段更新为角度控制描述 |

**不改**：固件、`ble_protocol`、BLE 协议、`settings_dialog` UI、macOS 端。

---

## 8. 风险与回退

| 风险 | 说明 | 缓解/回退 |
|---|---|---|
| 漂移 | 桌面端积分受 BLE 帧率抖动影响，长时间可能有微小漂移 | 快速归零兜底；若不可接受则升级到路径 2（固件姿态融合） |
| 回正误归零 | 用户想极慢移动时，`omega` 很小可能被归零，光标不动 | 调大 `kAirMouseAngleDeadzone` 或改为 stale-only 归零 |
| gain 标定 | 角度模型 gain 需要重新标定，初值可能偏 | 热调参面板可即时调；先给保守值 |
| 旧测试冲突 | `AirMouseStep` 签名变更影响现有测试 | 同步更新调用点 |
| 手感不如预期 | 角度模型需要用户适应 | 路径 1 快速验证，不行则保留配置切换两种模式 |

**回退开关**：若路径 1 验证失败，把 `AirMouseTick` 中传入 `AirMouseStep` 的 `input.value_x/y` 改回 `state.last_omega_x/y`、`is_angle=false`，并移除 theta 归零，即可恢复角速度模型。

---

## 9. 实施顺序

1. **RFC 确认**（本文档）。
2. **TDD 红灯**：调整 `AirMouseStep` 签名，新增/改写 angle 模式测试，跑 ctest 确认失败。
3. **TDD 绿灯**：
   - `air_mouse_kin.{h,cc}`：新增 `AirMouseInput`，`AirMouseStep` 适配。
   - `voice_stick_coordinator.{h,cc}`：`theta` 积分、归零、angle 模式调用。
4. **重构**：确保无重复、签名一致、注释清晰。
5. **构建验证**：`build_win.bat` + `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿。
6. **真机标定**：调整 gain 映射，验证保持角度持续移动、回正停止。
7. **文档同步**：`CLAUDE.md` / `AGENTS.md` 更新。
8. **提交**：按项目约定 `git add -f`。

---

## 10. 前置检查清单

- [x] 数据模型：`AirMouseInput`、`AirMouseDeviceState.theta` 已定义。
- [x] 接口/API 契约：`AirMouseStep(AirMouseKinState&, const AirMouseInput&, ...)` 已明确。
- [x] 边界条件与异常流：进入归零、持续移动、回正停止、stale 归零、多设备隔离、溢出限制均已识别。
