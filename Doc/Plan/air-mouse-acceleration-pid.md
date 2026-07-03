# 体感鼠标加速度 + PD 速度环改造（路径 1：仅桌面端）

## 1. 背景与目标

体感鼠标（air mouse）当前为"角速度 → 光标速度"的线性映射，缺乏加速曲线与惯性，手感生硬：慢动不够稳、快动不够猛、停手瞬停。用户希望改为"加速度控制 + 类 PID"——把光标当有质量的物理对象，小动作精准、大动作加速跨屏、停手缓停。

本期采用**路径 1：仅改桌面端**，不动固件、不烧录，快速验证手感改善。若效果不足或 clamp 限制明显，再走路径 2（改固件协议上报原始角速度，另立 RFC）。

## 2. 现状分析（代码证据）

### 2.1 完整链路（50Hz）

```
BMI270 gyro(dps) ─20ms─► bmi270_air_mouse_poll ─► voice_ble_send_motion
                              │                          │
                              ▼                          ▼
            去偏→死区→SCALE→clamp(±127)        BLE state_tx(0x5102) type=0x11 [6B]
                                                         │
                                                         ▼
                          Windows: ble_central_win ValueChanged → ParseMotionFrame
                                   → DispatchToUiThread → HandleMotionEvent → MoveMouse
```

- 固件每帧输出 `out = clamp((ω-bias)×0.6, ±127)`（`firmware/components/bmi270/bmi270.c:806-807`，`SCALE=0.6`、`MAX_DELTA=127`）。
- Windows `HandleMotionEvent`（`desktop/windows/src/voice_stick_coordinator.cc:513-522`）：`dx=round(event.dx×gain)`，`dy=round(event.dy×gain)`，`MoveMouse`。纯线性，无速度状态、无加速曲线、无惯性。

### 2.2 "开局卡顿后流畅"根因（两点叠加）

- **根因 A：校准期完全不输出**。`bmi270.c:728-761` 校准阶段 `return false` 不发帧，需 `AIR_MOUSE_CALIB_FRAMES=10` 帧连续静止（≥200ms）；`bmi270.c:740-747` 检测到运动即清零重来，进入瞬间手未稳时校准期拉长到 0.5~1s+，期间光标零反应。
- **根因 B：BLE interval 异步协商**。`main.c:1692-1694` 注释自承 `request_fast_interval` 异步，"等真正发帧时多半已切换"是乐观假设。校准 200ms < slow→7.5ms 协商（约 300~500ms），校准完发帧时 interval 未切完，50Hz 被限流到 ~10Hz → 卡顿。

### 2.3 当前控制模型

```
Δs = clamp((ω-bias)×0.6×gain, ±127)   # 每帧独立，光标速度 ∝ 角速度
```

线性速度控制，无加速曲线（慢/快同灵敏度）、无惯性（停手瞬停）、无累积加速感。

## 3. 本期范围与诚实评估

**做（路径 1 桌面端）**：
- PD 速度环（一阶低通跟踪）+ 幂律加速曲线，改善流畅期手感：慢速精准、快速跨屏、持续转动加速感。
- 60Hz 固定 tick 定时器，实现停手缓停滑行与帧抖动平滑。
- 进入/退出体感时复位速度状态。

**不做（留给路径 2）**：
- **不根治开局卡顿**：根因 A（校准期零反应）与根因 B（interval 协商）均在固件侧，桌面端无法消除。桌面端能做的仅是用实际 `dt` 积分避免抖动放大，以及定时器在帧稀疏时维持平滑（缓解根因 B 的卡顿感，但无法消除校准期零反应）。
- 不改 motion 帧语义（仍为 clamp 后的位移代理），故大幅转动被 `±127` clamp 限制信息。

> 若真机验证后"开局卡顿"仍不可接受，即触发路径 2（改固件校准逻辑 + motion 帧上报原始角速度）。

## 4. 设计

### 4.1 控制模型：一阶低通速度跟踪 + 幂律加速曲线

每轴独立维护光标速度 `v`（像素/秒）。把去偏角速度代理 `omega`（= `event.dx/dy`，范围 ±127）映射为目标速度 `v_target`，用一阶低通（PT1）让 `v` 跟踪 `v_target`。这等价于 P 速度环，`dv/dt` 即"加速度"，阻尼由时间常数 `tau` 单参数控制，离散稳定无超调。

```
# 加速曲线：幂律。gamma>1 使小输入更稳、大输入更猛
v_target = sign(omega) × |omega|^gamma × gain        # 像素/秒

# 一阶低通跟踪（离散，dt 为实际 tick 间隔，秒）
alpha = 1 - exp(-dt / tau)                            # tau 大→惯性大、缓停长
v += (v_target - v) × alpha

# 输出位移
dx_out = round(v × dt)                                # 像素，→ SendInput 相对移动
```

行为：
- **持续转动**：`omega` 持续非零，`v` 按 `tau` 爬升到 `v_target` → 加速感。
- **停手**：固件死区归零且停止发帧 → 定时器侧 `omega` 过期归零 → `v_target=0` → `v` 按 `exp(-dt/tau)` 衰减，定时器继续注入滑行 → 缓停。
- **慢动**：`|omega|` 小，`|omega|^gamma` 更小 → 低灵敏精准。
- **快动**：`|omega|` 大，幂律放大 → 高灵敏跨屏。

参数初值（真机标定）：`tau=0.10`、`gamma=1.35`、`gain=4.0`（使 `omega=127` 时 `v_target≈127^1.35×4≈560 px/s`）；`omega` 过期阈值 `omega_stale_ms=30`。

### 4.2 可测纯函数 AirMouseStep

把 PD 核心抽成纯函数，放 `voicestick_core`（可单测，不依赖 Win32/时钟）：

```cpp
struct AirMouseKinState {
    double vx = 0.0;   // 像素/秒
    double vy = 0.0;
    std::chrono::steady_clock::time_point last_omega_t;  // 最近一次 omega 更新
    std::int16_t last_omega_x = 0;
    std::int16_t last_omega_y = 0;
};

struct AirMouseParams {
    double tau = 0.10;
    double gamma = 1.35;
    double gain = 4.0;
    double omega_stale_ms = 30.0;
    bool invert_y = false;
};

// 单次 step：给定当前 state、本次 omega（来自 motion 帧，或定时器 tick 时传 last_omega）、
// 与 dt，更新 v 并输出本帧位移。omega_stale 时（距 last_omega 超阈值）omega 视为 0。
AirMouseStepResult AirMouseStep(AirMouseKinState& state,
                                std::int16_t omega_x, std::int16_t omega_y,
                                std::chrono::duration<double> dt,
                                bool omega_is_stale,
                                const AirMouseParams& params);
// 返回 { int dx_out, int dy_out }，state 原地更新。
```

`HandleMotionEvent` 与定时器 tick 都调它，区别仅在传入的 `omega` 与 `omega_is_stale`。

### 4.3 60Hz 定时器架构

进入体感时启动 60Hz（≈16ms）UI 线程定时器；退出时停止并复位状态。

- **motion 帧到达**（`HandleMotionEvent`）：更新 `state.last_omega_x/y` + `last_omega_t`，**不直接 MoveMouse**（交给定时器统一注入，避免 50Hz 输入抖动直注）。
- **定时器 tick**：取 `now`，算 `dt = now - last_tick`；判断 `omega_is_stale`（`now - last_omega_t > omega_stale_ms`）；调 `AirMouseStep`（stale 时 omega 传 0）；若 `dx_out/dy_out` 非零则 `MoveMouse`。

为何用定时器而非每帧直注：
1. 固件静止时不发帧，无定时器则 `v` 冻结、光标无缓停滑行（手停即停）。
2. BLE interval 抖动期帧稀疏，60Hz 固定 tick 平滑光标，不放大抖动。
3. 统一注入点避免 motion 回调与点击注入竞态。

线程安全：motion 回调与定时器 tick 均在 UI 线程（现有 `DispatchToUiThread` 已保证），无锁。

### 4.4 每设备状态

`std::map<std::string, AirMouseKinState> air_mouse_kin_states_`，与 `air_mouse_active_devices_` 对齐：`ToggleAirMouse` 进入时初始化、退出时 erase。多设备同时体感时各持独立速度状态（实际同时仅一设备操作，但状态隔离避免串扰）。

### 4.5 dt 时间源

`std::chrono::steady_clock`（单调时钟，不受系统时间回拨影响，跨平台可测）。`last_tick` 在定时器启动时置 `now`，每次 tick 更新。

## 5. 配置项（`app_config.{h,cc}` + `config.example.toml`）

新增（保留 `air_mouse_gain`/`air_mouse_invert_y`，`gain` 复用 `air_mouse_gain` 语义扩展为加速曲线增益）：

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `air_mouse_tau` | double | 0.10 | 速度跟踪时间常数（秒），越大惯性/缓停越长 |
| `air_mouse_gamma` | double | 1.35 | 加速曲线幂律指数，>1 慢稳快猛 |
| `air_mouse_gain` | double | 4.0 | 速度增益（复用现有字段，默认值由 1.0 调为 4.0，`AirMouseGainClamp` 范围相应放宽） |

`invert_y` 在 `AirMouseStep` 内对 `omega_y` 取反生效。`tau`/`gamma` 加 `Clamp` 辅助函数防非法值（`tau∈[0.02,0.5]`、`gamma∈[1.0,2.0]`）。

## 6. TDD 测试计划（`tests/core_tests.cc`，目标 `voicestick_windows_tests`）

遵循红-绿-重构，先写失败测试：

### 6.1 纯函数 AirMouseStep（核心，可测）
- `TestAirMouseStep_SteadyStateTracksTarget`：固定 `omega` 连续 step，`v` 收敛到 `sign×|omega|^gamma×gain`，`dx_out` 稳定。
- `TestAirMouseStep_AccelerationRamp`：从静止突变到 `omega=100`，前几帧 `v` 逐步爬升（验证加速感，非瞬变）。
- `TestAirMouseStep_StaleOmegaDecaysToZero`：`omega_is_stale=true` 连续 step，`v` 按 `exp(-dt/tau)` 衰减到 0，`dx_out` 递减至 0（验证缓停）。
- `TestAirMouseStep_GammaShape`：`omega=10` 与 `omega=100` 的 `v_target` 比值 > 线性比（验证幂律慢稳快猛）。
- `TestAirMouseStep_InvertY`：`invert_y=true` 时 `dy_out` 符号反转。
- `TestAirMouseStep_DtJitterRobust`：`dt` 抖动（16/24/8ms 交替）下 `v` 平滑无跳变。

### 6.2 协调器集成
- `TestCoordinatorMotionUpdatesKinStateNotDirectMove`：motion 帧 → 不直接 `MoveMouse`，仅更新 `last_omega`（需扩展 FakeInputInjector 记录调用时机）。
- `TestCoordinatorAirMouseTimerMovesCursor`：进入体感后定时器 tick → `MoveMouse` 被调用；退出后停止。
- `TestCoordinatorAirMouseStateResetOnToggle`：进入→退出→再进入，`v` 从 0 开始（无残留）。
- 既有 `TestCoordinatorMotionMovesCursorOnlyWhenActive` 需调整：motion 帧不再直注，改为 tick 后断言。

## 7. 文件改动清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/voice_stick_coordinator.{h,cc}` | 新增 `AirMouseKinState`/`AirMouseParams`/`AirMouseStep`、`air_mouse_kin_states_`、60Hz 定时器、`HandleMotionEvent` 改为只更新 omega |
| `desktop/windows/src/app_config.{h,cc}` | 新增 `air_mouse_tau`/`air_mouse_gamma`，调 `air_mouse_gain` 默认与 Clamp，Save/Load |
| `desktop/macos/Config/config.example.toml` | 补新配置项示例 |
| `desktop/windows/tests/core_tests.cc` | 新增 6.1/6.2 测试，调整既有 motion 测试 |
| `CLAUDE.md`/`AGENTS.md` | 配置项段补 `air_mouse_tau`/`air_mouse_gamma` |

固件、BLE 协议、`ble_protocol`、`ble_central_win`、`input_injector_win` **不改**。

## 8. 风险与回退

- **手感不达标**：参数 `tau`/`gamma`/`gain` 需真机标定，初值可能偏。提供配置项可热调，不改代码即可调。
- **60Hz 定时器 UI 线程负载**：60Hz `MoveMouse` 在 UI 线程，若与悬浮窗渲染/消息泵冲突可能抖动。回退：降频到 30Hz 或改独立线程。先 UI 线程验证。
- **clamp ±127 限制**：路径 1 输入受固件 clamp，大幅快转 `omega` 饱和 127，`v_target` 封顶。若跨屏不够快 → 触发路径 2。
- **回退**：所有改动以新增为主（新状态、新定时器、新配置项），关闭 `air_mouse` 后行为与现状一致；`tau` 调小到极限即退化为近线性映射。

## 9. 后续路径 2 触发条件

真机验证后若出现下列任一，另立路径 2 RFC（改固件）：
1. 开局卡顿（校准期零反应）不可接受 → 固件校准逻辑改为临时零偏先输出。
2. clamp ±127 限制快速跨屏 → motion 帧改报原始角速度 ×10。
3. interval 协商期卡顿明显 → 固件 interval 策略或预请求。

## 10. 实施顺序

1. RFC 确认（本文档）。
2. TDD：`AirMouseStep` 纯函数 + 测试全绿（6.1）。
3. 协调器集成：定时器 + `HandleMotionEvent` 改造 + 测试（6.2）。
4. 配置项 + Save/Load + `config.example.toml`。
5. `build_win.bat` 构建 + CTest 全绿。
6. 真机标定 `tau`/`gamma`/`gain`。
7. 文档同步（`CLAUDE.md`/`AGENTS.md`）+ 提交（按 Windows 收尾约定）。
