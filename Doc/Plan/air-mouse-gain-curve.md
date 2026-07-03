# 体感鼠标三段线性增益曲线 + 固件死区下调

## 1. 背景与目标

当前体感鼠标（air mouse）速度控制链路为**纯线性增益**：

```
固件: out = clamp((ω-bias) × 0.6, ±127)          # bmi270.c:893, AIR_MOUSE_SCALE=0.6
Windows: v_target = omega × gain                   # air_mouse_kin.cc:19, gain=sensitivity×16
         v += (v_target - v) × (1 - exp(-dt/tau))  # 速度环一阶低通
         fx += v × dt; dx = trunc(fx)              # 亚像素累积
```

线性增益存在经典矛盾：**大了微调崩，小了甩不动**。一个 `gain` 无法同时满足"慢动精准对位"与"快动跨屏"。

真机标定反馈（见 `imu-air-mouse-impl.md` 记忆"SCALE/死区/轴向待真机标定"）：微调段光标要么不动（被固件死区截断），要么一过死区就冲过头；甩动段又跨屏不够快。

### 与历史方案的关系

- `air-mouse-acceleration-pid.md` 曾提出**幂律曲线** `v_target = sign(ω)·|ω|^gamma × gain`（gamma=1.35）。后被 `air-mouse-velocity-control.md` 重构为速度控制时移除（当前代码无 gamma）。
- 幂律曲线平滑但**整段连续放大**，微调段（|ω|<1）被 `|ω|^1.35` 压得更扁，且幂律在 |ω|>1 后单调放大无上限，甩动段易失控；参数只有一个 gamma，形状难精确控制。
- 本方案改用**三段线性**：微调段、中段、甩动段各有明确斜率与拐点，形状可控、边界可测，且中段线性过渡保证典型转动速度不会过度放大。`core_tests.cc` 易覆盖三档斜率与拐点连续性。

## 2. 链路耦合：固件死区必须同降

固件 `AIR_MOUSE_DEADZONE_DPS=4.0`（`bmi270.c:887`）在源头把 `|ω-bias|<4 dps` 归零。增益曲线的微调段（|out|<5，对应 ≈8.3 dps）若不降死区，**信号在固件端就被截断**，微调段永远收不到输入——增益曲线白做。

故本次必须**联动下调固件死区 4→3 dps**：
- 3 dps 仍盖住 BMI270 静止噪声主体（1~3 dps），且现有 **jerk 静止判据**（`STILL_JERK_DPS=3`，相邻帧差<3 dps 视为静止）+ **静止帧硬归零**（`bmi270.c:876`）+ **零偏 EMA 自愈**（`bmi270.c:870`）三重兜底防漂移，降死区不会引入静止抖动。
- 2 dps 噪声漏过风险大（依赖 jerk 兜底），4 dps 微调段饿死，3 dps 为折中。

## 3. 设计

### 3.1 三段线性增益曲线

替换 `air_mouse_kin.cc:19` 的 `v_target = ox × gain`，改为：

```cpp
// 三段线性增益因子（作用于 |omega|），base_gain 提供整体缩放（sensitivity 滑杆）。
//   微调段：|ox| < kLowThresh   → factor = kLowFactor   （压低，精准对位）
//   中段：  kLowThresh ≤ |ox| < kHighThresh → factor 线性插值 kLowFactor→kHighFactor
//   甩动段：|ox| ≥ kHighThresh  → factor = kHighFactor  （放大，跨屏）
constexpr double kAirMouseLowThresh  = 5.0;   // ≈8.6dps≈0.15rad/s（用户原方案微调上限）
constexpr double kAirMouseHighThresh = 40.0;  // ≈66dps≈1.16rad/s（用户原方案甩动下限）
constexpr double kAirMouseLowFactor  = 0.3;   // 微调段相对增益
constexpr double kAirMouseHighFactor = 4.0;   // 甩动段相对增益

double v_target = ox × base_gain × factor(|ox|)
```

中段插值：`factor = kLowFactor + (kHighFactor - kLowFactor) × (|ox| - kLowThresh) / (kHighThresh - kLowThresh)`

拐点连续性：`|ox|=kLowThresh` 时 factor=kLowFactor；`|ox|=kHighThresh` 时 factor=kHighFactor，无跳变。

### 3.2 参数推导（基于当前链路）

`ox` 是固件离散化后的 int16（≈ mv_dps × 0.6）。用户原方案阈值 0.15/1.2 rad/s 换算：
- 0.15 rad/s = 8.6 dps → out = 8.6 × 0.6 = 5.16 ≈ **5**
- 1.2 rad/s = 68.8 dps → out = 68.8 × 0.6 = 41.3 ≈ **40**

数值验证（base_gain=160，sensitivity=10 档）：
- 微调段顶 ox=5：v_target = 5 × 160 × 0.3 = 240 px/s（慢，对位图标）
- 中段 ox=24（真机典型手腕速度，日志实测）：factor=0.3+3.7×19/35=2.31，v_target=24×160×2.31=8873 px/s
- 甩动段 ox=40：v_target = 40 × 160 × 4.0 = 25600 px/s（每帧≈410px，跨 4K 屏）
- 甩动段 ox=127（满量程）：v_target = 127 × 160 × 4.0 = 81280 px/s

典型转动（ox=24）被适度放大 2.3 倍（vs 线性 3840），甩动段（ox≥40）放大 4 倍跨屏，微调段（ox<5）压到 0.3 倍精准。符合"慢稳快猛"诉求。

### 3.3 常量位置

曲线形状参数（阈值/倍率）放 `air_mouse_kin.h` 的 `constexpr`，**不进配置项**。理由：
- 曲线形状是标定参数，非用户日常调参；sensitivity 滑杆（1-10 档 → base_gain）已提供整体增益调节。
- 放 header 使纯函数测试可直接引用常量算期望值，标定改常量时测试自动同步，避免测试与实现数值漂移。

### 3.4 固件死区下调

`firmware/components/bmi270/bmi270.c`：
- `AIR_MOUSE_DEADZONE_DPS` 4.0 → 3.0，更新注释（噪声余量从"4 dps 留足"改为"3 dps，依赖 jerk 静止判据+静止归零兜底"）。

## 4. TDD 测试计划（`tests/core_tests.cc`）

### 4.1 红灯（新增，覆盖三段形状）

1. `TestAirMouseStepGainCurveLowRange`：omega=3（微调段），稳态 vx ≈ 3×gain×kLowFactor。断言 vx < 3×gain（低于线性）。
2. `TestAirMouseStepGainCurveHighRange`：omega=80（甩动段），稳态 vx ≈ 80×gain×kHighFactor。断言 vx > 80×gain（高于线性）。
3. `TestAirMouseStepGainCurveMidRange`：omega=20（中段），稳态 vx ≈ 20×gain×factor(20)，factor 用常量算。断言在微调段与甩动段之间。
4. `TestAirMouseStepGainCurveShape`：同 gain 下，omega=3 的 vx < omega=20 的 vx < omega=80 的 vx（单调），且微调段斜率 < 中段 < 甩动段。
5. `TestAirMouseStepGainCurveContinuousAtLowThreshold`：omega=kLowThresh-1 与 kLowThresh+1 的稳态 vx 差值小（拐点连续，无跳变）。
6. `TestAirMouseStepGainCurveNegative`：omega=-80，vx 为负且 |vx|≈80×gain×kHighFactor（负向对称）。

### 4.2 需调整的冲突旧测试

- `TestAirMouseStepVelocityProportionalToOmega`（2269）：原断言 v≈omega×gain（纯线性）。增益曲线下 omega=10 落中段 factor=0.83，v≈132≠160。**改写**为断言 v≈omega×gain×factor(omega)，或由 4.1 的 Low/High/Mid 三个测试取代后**删除**。本方案选择删除（语义被 4.1 覆盖更清晰）。
- `TestCoordinatorAirMouseSustainedRunBounded`（1766）：omega=24 落中段，新稳态 v≈8873 > 旧上限 5000。**调整上限**为 12000 并更新注释（典型转动速度上限随曲线放大）。本测试仍约束"长转不过快"，但阈值随曲线语义更新。
- 其余测试（`VelocityFollowsOmega`/`StopsWhenStale`/`AxisGain`/`InvertY`/`DtJitterRobust`/`SubPixelAccumulation`/`HighSensitivityRealisticSpeed`/`StopsOnZeroOmega`）：增益曲线不破坏其语义，保留。`HighSensitivityRealisticSpeed`（≥1500）新曲线下 total_dx≈7000 仍满足。

### 4.3 绿灯（最小实现）

1. `air_mouse_kin.h`：加 4 个 `constexpr` 阈值/倍率常量。
2. `air_mouse_kin.cc`：新增 `AirMouseGainFactor(double omega_abs)` 纯函数（三段线性），`AirMouseStep` 中 `v_target = ox × gain_x × factor(|ox|)`（y 轴同理）。
3. `bmi270.c`：`AIR_MOUSE_DEADZONE_DPS` 4.0→3.0 + 注释。

### 4.4 重构

- `AirMouseGainFactor` 抽为独立纯函数（可单测拐点连续性），而非内联在 `AirMouseStep`。
- 常量命名对齐现有 `kAirMouse*` 风格（若有）或 `AIR_MOUSE_*` 宏风格——查现有 header 用 `snake_case` 字段 + 无前缀常量，故用 `kAirMouseLowThresh` 等。

## 5. 文件改动清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/air_mouse_kin.h` | 加 4 个 constexpr 曲线常量 |
| `desktop/windows/src/air_mouse_kin.cc` | 新增 `AirMouseGainFactor`，`AirMouseStep` 用曲线算 v_target |
| `desktop/windows/tests/core_tests.cc` | 新增 6 个增益曲线测试，删除 `VelocityProportionalToOmega`，调 `SustainedRunBounded` 上限 |
| `firmware/components/bmi270/bmi270.c` | `AIR_MOUSE_DEADZONE_DPS` 4.0→3.0 + 注释 |
| `CLAUDE.md` / `AGENTS.md` | 体感鼠标段补增益曲线与死区 3 dps 说明 |

不改：BLE 协议、motion 帧语义、settings_dialog UI、sensitivity 滑杆（1-10 档不变）、macOS 端（流式精修/体感改造仅 Windows，见记忆 `windows-only-no-macos-streaming`）。

## 6. 风险与回退

- **中段放大过猛**：典型转动 ox=24 被放 2.3 倍，若真机觉太快，降 `kHighFactor`（4→3）或提高 `kHighThresh`（40→50）让典型速度更靠近线性区。常量改了重编译即可，不动配置。
- **甩动段跨屏过冲**：ox=127 时 v=81280 px/s，每帧 1300px，可能跨屏过头。若真机过冲，降 `kHighFactor` 或加 `v_target` 上限 clamp。
- **固件死区 3 dps 静止抖动**：jerk 静止判据 + 静止归零兜底，预期无抖动。若真机静止有轻微漂移，回调 3→3.5。
- **回退**：曲线常量设 `kLowFactor=kHighFactor=1.0` 即退化为纯线性；固件死区改回 4.0。均为单常量改动。

## 7. 实施顺序

1. RFC 确认（本文档）。
2. TDD 红灯：`core_tests.cc` 加 6 个曲线测试 + 调冲突测试，跑 ctest 确认失败。
3. TDD 绿灯：`air_mouse_kin.{h,cc}` 实现曲线，`bmi270.c` 死区下调，跑 ctest 全绿。
4. 构建验证：`build_win.bat` + ctest；固件 `idf.py build`（或 `idf_cli.py -c`）。
5. 文档同步 `CLAUDE.md`/`AGENTS.md` + 提交（Windows 收尾 git add -f）。
6. 真机标定：曲线倍率/阈值、死区，按 6 节回退点微调。
