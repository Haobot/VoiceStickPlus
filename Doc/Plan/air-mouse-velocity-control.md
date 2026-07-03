# 体感鼠标：角度控制 → 速度控制（手停即停）

## 背景

v1.8.x 体感鼠标采用**角度控制模型**：积分角速度得相对偏转角 θ，保持 θ 时光标持续移动。
真机反馈：10 级灵敏度光标仍慢，且手停后光标继续滑行（停不住）。

## 根因（证据链）

`desktop/windows/src/air_mouse_kin.cc` 当前模型：

```
theta += omega × dt                     // 积分角速度得相对偏转角
theta *= exp(-dt / decay_tau)           // 慢衰减防漂移（decay_tau=2s）
v_target = theta × gain                 // 角度→目标速度
v += (v_target - v) × (1 - exp(-dt/tau))
fx += v × dt;  dx = trunc(fx)
```

问题：
1. **手停不停**：手停后 `omega=0`，但 `theta` 残留，按 `decay_tau=2s` 慢衰减 → 光标持续滑行 2-4s。
   `omega=0` 无法让 `v_target` 即时归零，这是"手停不停"的本质。
2. **不跟手**：转动需积累 θ 才加速，体感有延迟。

日志取证（`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）：
- `motion VS-D010 dx=15,16,21,24,19,17` — motion 帧正常接收（光标能动，非"不动"）。
- `tick VS-D010 theta_x=9.22 vx=176` — 10 级 gain=20 时稳态光标 ~176px/s（慢）。

## 新模型：速度控制

`omega` 直接决定目标速度，`omega=0` 即 `v_target=0`，`v` 经 `tau` 快速归零：

```
v_target = omega × gain                      // 目标速度直接跟随角速度
v += (v_target - v) × (1 - exp(-dt / tau))   // 速度环一阶低通（平滑陀螺仪噪声）
fx += v × dt;  dx = trunc(fx)                // 亚像素累积
```

特性：
- `omega=0 → v_target=0 → v` 经 `tau` 衰减归零，手停滑行 ≈ `3 × tau`。
- 转动即时达目标速度（无需积累），更跟手。
- `omega_is_stale=true` 仍把 `omega` 视为 0（30ms 无 motion 即停），与速度模型天然契合。
- 移除 `theta_x/theta_y` 字段与 `decay_tau` 参数（无角度积分，无需防漂移衰减）。

## 参数

| 参数 | 旧值 | 新值 | 理由 |
|---|---|---|---|
| `gain` 映射系数 | `sensitivity × 8.0` | `sensitivity × 16.0` | 速度模型无 `theta_ss=omega×decay_tau` 的 ×2 放大，需提高系数补偿。10 级=160，omega≈24 → v≈3840px/s。 |
| `tau` 默认 | `0.10` | `0.05` | 手停滑行 0.15s 基本即停，保留轻微平滑压制陀螺仪抖动。`AirMouseTauClamp` 范围 [0.02, 0.5] 不变。 |
| `decay_tau` | 硬编码 2.0 | **移除** | 速度模型无角度积分，不需要。 |
| `invert_y` | 不变 | 不变 | — |

`gain × 16` 与 `tau=0.05` 均为可调初值，待真机标定后再微调。

## 测试计划（TDD）

### 红灯（先写失败测试）

1. `TestAirMouseStepStopsWhenStale`（纯函数）：转动积累 `v` 后 `stale` 100 帧，断言 `|vx|<1.0`。
   旧角度模型 `decay_tau=8s` 慢衰减，`v` 不归零 → 失败。
2. `TestCoordinatorAirMouseStopsOnZeroOmega`（协调器）：转动 0.5s 后上报 `omega=0` 0.5s，断言停手后位移 `<50px`。
   旧角度模型 `theta` 慢衰减，`v` 仍大 → 位移大 → 失败。

> 协调器测试用 `on_motion_event{0,0}` 模拟手停（零 omega），而非依赖 `steady_clock` 的 stale 判断
> （FakeBleCentral 无延迟，stale 不可靠）。stale 路径由纯函数测试覆盖。

### 绿灯（最小实现）

1. `air_mouse_kin.h`：`AirMouseKinState` 移除 `theta_x/theta_y`；`AirMouseParams` 移除 `decay_tau`，默认 `tau=0.05/gain=16`。
2. `air_mouse_kin.cc`：重写 `AirMouseStep` 为速度控制（移除 θ 积分与衰减）。
3. `voice_stick_coordinator.cc`：`AirMouseParamsFromConfig` 改 `gain=sensitivity×16`，移除 `decay_tau` 赋值；tick 日志 `theta_x` 字段改 `vx`。

### 重构（测试全绿后）

- 改写 4 个角度语义纯函数测试为速度语义：
  - `AngleIntegrates` → `VelocityFollowsOmega`
  - `AngleDecaysWhenStale` → 合并入 `StopsWhenStale`（红灯已加）
  - `AngleHoldsShortTerm` → **删除**（角度保持特性不再存在）
  - `VelocityFromAngle` → `VelocityProportionalToOmega`
- 保留 `AxisGain / InvertY / DtJitterRobust / SubPixelAccumulation`（语义仍成立，但 `AxisGain` 改用 `vx/vy` 比较而非 `theta`）。
- 协调器约束调整：
  - `HighSensitivityRealisticSpeed`：`total_dx >= 400` → `>= 1500`（速度模型即时达速）。
  - `SustainedRunBounded`：`avg <= 3000` → `<= 5000`（速度模型恒速不累积，防 gain 过高）。

## 影响范围

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/air_mouse_kin.h` | 模型结构重写（移除 theta/decay_tau） |
| `desktop/windows/src/air_mouse_kin.cc` | `AirMouseStep` 速度控制重写 |
| `desktop/windows/src/voice_stick_coordinator.cc` | `AirMouseParamsFromConfig`（gain×16，移除 decay_tau）、tick 日志字段 |
| `desktop/windows/src/app_config.h` | `air_mouse_tau` 默认 0.05、注释（gain 映射 ×16） |
| `desktop/windows/tests/core_tests.cc` | 改写角度测试为速度测试、调约束、新增 2 个手停即停测试 |
| `CLAUDE.md` / `AGENTS.md` | gain 映射 ×2.0→×16、tau 默认 0.10→0.05、模型描述 |

## 不改

- 配置项名称、`settings_dialog` 滑杆 UI（1-10 档不变）、BLE 协议、固件 motion 上报。
- `decay_tau` 本就非配置项（硬编码），移除无配置影响。
- 临时调试日志（motion/tick）保留到真机验证后移除。

## 验证

1. `ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests` 全绿。
2. `build_win.bat` 构建，启动新 `VoiceStick.exe`。
3. 真机：10 级转动应明显快（约 4 倍），手停后光标 ~0.15s 内停住。
