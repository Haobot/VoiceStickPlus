# 体感鼠标静止判据滞回 + 真机热调参面板

## 0. 关联

- 上游评估：`Doc/Plan/air-mouse-still-tuning-eval.md`（若未单独存档，见会话结论）。
- 既有方案：`air-mouse-gain-curve.md`（三段线性增益曲线，已落地）、`imu-air-mouse.md`（首版）、`air-mouse-velocity-control.md`（速度控制模型）。
- 记忆：`imu-air-mouse-impl`（实现状态与真机联调结论）、`air-mouse-secondary-conflict`（侧键冲突）。

本方案含两个协同子项：
- **O4 静止判据滞回**（固件）：修复"慢转被误判静止吃掉"的根因，解决慢转一顿一顿。
- **O9 真机热调参面板 + 实时曲线**（桌面端）：降低后续标定成本，可视化 still/active 切换辅助 O4 验证。

二者协同：O4 引入新滞回阈值需真机标定，O9 提供标定工具。但各自独立可实施，O9 不依赖固件协议改动。

---

## 第一部分：O4 静止判据滞回

### 1.1 现状与根因

固件 `bmi270_air_mouse_poll`（`firmware/components/bmi270/bmi270.c:792-902`）每帧（50Hz/20ms）：

```
读 dps → 减零偏 gx,gy,gz → jerk 判静止(三轴帧间差<STILL_JERK_DPS=3)
  ├─ still: EMA 收敛零偏 + return false（不上报位移）
  └─ 非 still: 死区|gx|<3→0 → SCALE 0.6 → clamp ±127 → 上报
```

**根因（比增益曲线更底层）**：静止判据用 **jerk（帧间角速度差）**，但 jerk 小只代表"角速度稳定"，**不代表"角速度为 0"**。

- 匀速慢转（手腕以 10dps 稳定转动）：角速度稳定 → jerk≈0 < 3 → 误判 still → `return false` → **光标不动，慢转被吃掉**。
- 这是 `air-mouse-gain-curve.md` 真机标定时"慢转难精准对位"的底层原因；当时通过拓宽微调段（5→15）缓解了"中段放大冲过头"，但"匀速慢转被 still 吃帧"未根治。
- 附加问题：still 同时承担 **EMA 收敛零偏** 与 **不上报位移** 两个职责，耦合。当前用 jerk 而非"去偏后幅值"判静止（注释 `bmi270.c:853-855`），是为让 EMA 能自愈初始零偏误差——这个设计对 EMA 是对的，但把它复用到"是否上报位移"就错了。

### 1.2 设计：双判据 + 单状态机滞回

**解耦两个职责**：
- **EMA 收敛条件**（保持现状）：jerk 判静止。仅在真静止时收敛零偏，自愈温漂与初始误差。
- **位移上报门控**（新增）：去偏后幅值 `|gx|,|gy|` 经滞回状态机判定 STILL/ACTIVE。ACTIVE 才上报。

**统一 STILL/ACTIVE 状态机**（一个状态变量 `s_air_mouse_is_active`）：

```
ACTIVE → STILL：|gx|<OMEGA_EXIT 且 |gy|<OMEGA_EXIT          （停手即停，不看 jerk）
STILL  → ACTIVE：|gx|>OMEGA_ENTER 或 |gy|>OMEGA_ENTER        （明确意图才动）
STILL 内 EMA 收敛额外要求：jerk<STILL_JERK_DPS（真静止才更新零偏）
```

滞回带 `OMEGA_ENTER > OMEGA_EXIT` 防边界抖动反复横跳。

**各场景验证**：

| 场景 | \|gx\|,jerk | 状态 | 上报 | EMA | 正确性 |
|---|---|---|---|---|---|
| 静止噪声 | <1, <1 | STILL | 否 | 收敛 | ✓ 不漂 |
| 匀速慢转 10dps | 10, ≈0 | ACTIVE | 是 | 不收敛 | ✓ **慢转不丢帧（修复点）** |
| 边界抖动 2-4dps | 2-4 | 滞回保持 | 按滞回 | — | ✓ 不反复横跳 |
| 刚停余震 | <1, 大 | STILL（\|omega\|<EXIT） | 否 | 不收敛（jerk 大） | ✓ 不上报余震，零偏暂不更新 |
| 甩动 | >50 | ACTIVE | 是 | 不收敛 | ✓ |

**与死区的关系**：死区 `AIR_MOUSE_DEADZONE_DPS=3`（`bmi270.c:888`）保留，作为 ACTIVE 帧内残余噪声的硬截断。滞回阈值与死区：`OMEGA_EXIT(2) < 死区(3) < OMEGA_ENTER(4)`，无矛盾——`|omega|` 在死区以下早被归零，滞回状态机只决定"是否进入上报流程"。

### 1.3 阈值初值（真机标定）

编译期常量（沿用现有 `#define` 风格，`bmi270.c` 顶部）：

```c
#define AIR_MOUSE_OMEGA_ENTER_DPS  4.0f   // STILL→ACTIVE 阈值，略高于死区，明确意图才动
#define AIR_MOUSE_OMEGA_EXIT_DPS   2.0f   // ACTIVE→STILL 阈值，低于死区，确实静止才停
// STILL_JERK_DPS=3.0 保持（EMA 收敛条件，不变）
```

标定方向：
- 慢转仍丢帧 → 降 `OMEGA_ENTER`（如 3.5）。
- 静止有微漂 → 升 `OMEGA_EXIT`（如 2.5）或保持。
- 滞回带过窄致边界抖动 → 拉宽 `ENTER-EXIT`。

### 1.4 poll 逻辑改造骨架（伪代码，非实现）

```c
bool bmi270_air_mouse_poll(int16_t *dx, int16_t *dy) {
    // ... 读 dps, 减零偏得 gx,gy,gz（不变）...
    // ... 算 jerk, 更新 prev_raw（不变）...

    // 滞回状态机：位移上报门控（新增）
    const float abs_gx = fabsf(gx), abs_gy = fabsf(gy);
    if (s_air_mouse_is_active) {
        if (abs_gx < AIR_MOUSE_OMEGA_EXIT_DPS && abs_gy < AIR_MOUSE_OMEGA_EXIT_DPS) {
            s_air_mouse_is_active = false;  // 停手即停
        }
    } else {
        if (abs_gx > AIR_MOUSE_OMEGA_ENTER_DPS || abs_gy > AIR_MOUSE_OMEGA_ENTER_DPS) {
            s_air_mouse_is_active = true;   // 明确意图才动
        }
    }

    // EMA 收敛零偏：仅 STILL 且 jerk 静止时（解耦，不再与上报门控耦合）
    if (!s_air_mouse_is_active && still_by_jerk && settling 已过) {
        EMA 收敛零偏; s_air_mouse_bias_dirty = true;
    }

    if (!s_air_mouse_is_active) {
        return false;  // 不上报
    }

    // 死区 + SCALE + clamp（不变）
}
```

**进入体感时**：`s_air_mouse_is_active` 初始化为 `false`（`bmi270_air_mouse_start`，与现有 `s_air_mouse_has_prev=false` 同处重置）。

### 1.5 校准阶段（未定标）不受影响

`!s_air_mouse_has_bias` 时的实时校准逻辑（`bmi270.c:806-844`）独立于运行期滞回，不动。滞回状态机仅在 `has_bias` 后的运行期生效。

---

## 第二部分：O9 真机热调参面板 + 实时曲线

### 2.1 现状与问题

- 现有 `settings_dialog` 是**模态对话框**（`DialogBoxIndirectParamW`，`settings_dialog.cc:148`），改参需确定/重开，无法实时看效果。
- 桌面端参数热调通路已存在：`AirMouseParamsFromConfig()` 每 tick 从 `config_` 读（`coordinator.cc:527-537,542`），`UpdateConfig` 后下个 tick 即生效。但 `UpdateConfig` 会重建 LLM 客户端（`coordinator.cc:160-162`），热调体感参数不该触发此重活。
- 曲线参数 `kAirMouseLowThresh/Factor/HighThresh/HighFactor` 是 `constexpr`（`air_mouse_kin.h:29-32`），**运行期不可变**，无法热调。
- 固件参数（死区、SCALE、`OMEGA_ENTER/EXIT`、`STILL_JERK`）是编译期宏，热调需 BLE `control_rx` 下发，**本次不做**（见 2.5 范围外）。

### 2.2 范围

**范围内（桌面端，本次实现）**：
- 非模态热调参窗口（独立于 settings_dialog）。
- 可热调参数：`gain_x/y`（1-10 档）、`tau`、`invert_y`、**曲线四参数**（运行期化）。
- 实时曲线显示：`omega_x/omega_y/vx/dx` 时序折线 + still/active 状态色带。
- 实时应用（每改动即生效）+ 保存到 config.toml + 重置默认。

**范围外（固件，后续扩展，本次预留协议位但不实现）**：
- 固件死区/SCALE/`OMEGA_ENTER/EXIT`/`STILL_JERK` 热调：需扩 `control_rx` JSON 字段（`main.c:794` 现有 `air_mouse_enabled` 通道）+ 固件改 + 真机验证。O9 面板预留"固件参数"只读区（从协议读或显示编译值），后续接入。

### 2.3 数据模型

**曲线参数运行期化**：`constexpr` 提为 `AirMouseCurveParams` 结构，纳入 `AirMouseParams`。

```cpp
// air_mouse_kin.h（新增）
struct AirMouseCurveParams {
    double low_thresh  = 15.0;   // 原 kAirMouseLowThresh
    double high_thresh = 50.0;   // 原 kAirMouseHighThresh
    double low_factor  = 0.15;   // 原 kAirMouseLowFactor
    double high_factor = 4.0;    // 原 kAirMouseHighFactor
};

// AirMouseParams 增加：
struct AirMouseParams {
    double gain_x, gain_y, tau;
    bool invert_y;
    AirMouseCurveParams curve;   // 新增（替代 constexpr）
};

// AirMouseGainFactor 签名变更（接收曲线参数）：
double AirMouseGainFactor(double omega_abs, const AirMouseCurveParams& curve);

// AirMouseStep 内 v_target = ox * gain * AirMouseGainFactor(|ox|, params.curve);
```

**默认值**：`AirMouseCurveParams` 默认值 = 当前 `constexpr` 真机标定值（15/50/0.15/4.0），保证不调参时行为不变。

**热调轻量路径**（避免 LLM 重建）：

```cpp
// voice_stick_coordinator.h（新增）
void UpdateAirMouseParams(const AirMouseParams& params);  // 仅更新 air_mouse 运行期参数
// AirMouseTick 改为用 live_air_mouse_params_（默认从 config_ 初始化），而非每 tick 调 AirMouseParamsFromConfig
```

`UpdateConfig` 仍负责 config_ 整体更新（含 LLM 重建），热调面板走 `UpdateAirMouseParams` 轻量路径。

### 2.4 实时曲线显示

**非模态窗口**：新建 `air_mouse_tuning_window.{h,cc}`，`CreateWindowExW` 顶层窗口（参考 `overlay_window.cc` 的 D2D 初始化，但用普通窗口样式非分层）。托盘菜单加"体感鼠标调参"入口打开。

**曲线数据**：`AirMouseTick` 内已有 `vx/dx/omx/stale`（`coordinator.cc:551-556`），扩展为 ring buffer（最近 N=200 帧 ≈ 3.3s @60Hz），新增 `still/active` 标志（O4 上报或桌面端 stale 推断）。WM_TIMER（复用 AirMouseTick 或独立 60Hz）触发 `InvalidateRect` → `WM_PAINT` GDI 画折线。

**绘制**（GDI，简单够用，不引入 D2D 复杂度）：
- 三条折线：`omx`（陀螺仪输入）、`vx`（光标速度）、`dx`（每帧位移），归一化到窗口高度。
- 底部色带：STILL（灰）/ACTIVE（绿），可视化滞回切换（O4 验证利器）。
- 当前参数值文字叠加。

### 2.5 配置项扩展

`app_config.h` 新增（带 clamp）：

```cpp
double air_mouse_curve_low_thresh  = 15.0;
double air_mouse_curve_high_thresh = 50.0;
double air_mouse_curve_low_factor  = 0.15;
double air_mouse_curve_high_factor = 4.0;
```

`app_config.cc` 加 `ApplyConfigValue` / `TomlDouble` / `output` 三处（仿 `air_mouse_tau`，`app_config.cc:354,475,550`）。clamp 函数防越界（low_thresh ∈ [1,30], high_thresh ∈ [30,80], low_factor ∈ [0.05,0.5], high_factor ∈ [2,6]）。

---

## 第三部分：TDD 测试计划

### 3.1 O4（固件，无单测——逻辑验证策略）

固件无自动化测试（`CLAUDE.md` 测试策略）。验证三手段：

1. **逻辑抽纯函数 + 代码审查**：滞回状态机判定抽为固件内纯函数 `air_mouse_update_active(bool prev_active, float abs_gx, float abs_gy)`，逻辑清晰可审（无外部依赖，纯输入输出）。
2. **串口日志增强**：`bmi270_air_mouse_poll` 加 `ESP_LOGD` 打印 `state=%s gx=%.1f gy=%.1f jerk=%.1f`（仅体感态），真机慢转时用 `idf_cli.py -s` 采集，grep 统计 ACTIVE 占比，确认匀速慢转持续 ACTIVE 不丢帧。
3. **O9 曲线辅助**：still/active 色带真机可视化，直观验证滞回切换无抖动。

**真机验收用例**：
- 匀速慢转手腕（≈10dps）3 秒：光标连续移动，日志 ACTIVE 占比 >90%（修复前会间歇 STILL）。
- 静置桌面 10 秒：光标不漂移，日志持续 STILL，EMA 偶发收敛。
- 慢转→停→慢转：切换平滑无一顿一顿。

### 3.2 O9（桌面端，voicestick_core 可测部分）

**红灯（新增，`tests/core_tests.cc`）**：

1. `TestAirMouseGainFactorAcceptsCurveParams`：传入自定义 curve（low_thresh=10, high_factor=5），断言拐点随 curve 变化（原 constexpr 测试改签名）。
2. `TestAirMouseGainFactorDefaultCurveMatchesLegacy`：`AirMouseCurveParams{}` 默认值下，factor 与原 constexpr 值一致（回归保护）。
3. `TestAirMouseStepUsesCurveParams`：step 输出随 `params.curve` 变化（同 omega、不同 curve → 不同 v_target）。
4. `TestAirMouseCurveClamp`：`AirMouseCurveClamp` 钳位越界值（low_thresh>high_thresh 时交换/夹紧，因子越界夹紧）。
5. `TestAirMouseCurveLowBelowHigh`：low_thresh < high_thresh 不变式（配置解析时保证）。

**需调整的旧测试**：现有 6 个增益曲线测试（`air-mouse-gain-curve.md` 4.1 节）引用 `kAirMouseLowThresh` 等 constexpr，改为传 `AirMouseCurveParams{}` 默认值。语义不变，仅签名适配。

**绿灯**：
1. `air_mouse_kin.h`：加 `AirMouseCurveParams`，`AirMouseParams.curve`，改 `AirMouseGainFactor` 签名。
2. `air_mouse_kin.cc`：`AirMouseGainFactor` 用 `curve.*` 替代 `kAirMouse*`；`AirMouseStep` 传 `params.curve`。
3. `app_config.{h,cc}`：4 个曲线配置项 + clamp + 解析/序列化。
4. `voice_stick_coordinator`：`UpdateAirMouseParams` + `live_air_mouse_params_`，`AirMouseTick` 用 live params。
5. `air_mouse_tuning_window.{h,cc}`：非模态窗口 + 控件 + GDI 曲线 + ring buffer。
6. `win32_app`：托盘菜单入口 + 窗口生命周期 + 调参回调接 `UpdateAirMouseParams`。

**重构**：
- `AirMouseGainFactor` 保持纯函数（可单测拐点连续性与 curve 注入）。
- 曲线 clamp 抽独立纯函数 `AirMouseCurveClamp`，配置解析与 UI 均复用。

### 3.3 UI 部分（无单测，手动验证）

- 改 gain 滑块：光标即时变速。
- 改曲线 low_thresh：微调段范围即时变化，曲线图标记拐点移动。
- 实时曲线：转动设备见 omx 折线起伏、vx 跟随、dx 脉冲。
- 保存：重启后参数持久化。
- 重置：恢复默认 15/50/0.15/4.0。

---

## 第四部分：文件改动清单

| 文件 | 改动 | 子项 |
|---|---|---|
| `firmware/components/bmi270/bmi270.c` | 滞回状态机 + `OMEGA_ENTER/EXIT` 宏 + `s_air_mouse_is_active` + EMA 解耦 + `ESP_LOGD` | O4 |
| `firmware/components/bmi270/include/bmi270.h` | （若需暴露 active 状态给 main.c 上报，否则不动） | O4 |
| `desktop/windows/src/air_mouse_kin.h` | `AirMouseCurveParams` + `AirMouseParams.curve` + `AirMouseGainFactor` 签名 | O9 |
| `desktop/windows/src/air_mouse_kin.cc` | `AirMouseGainFactor` 用 curve + `AirMouseCurveClamp` | O9 |
| `desktop/windows/src/app_config.{h,cc}` | 4 个曲线配置项 + clamp + 解析/序列化 | O9 |
| `desktop/windows/src/voice_stick_coordinator.{h,cc}` | `UpdateAirMouseParams` + `live_air_mouse_params_` + `AirMouseTick` 用 live | O9 |
| `desktop/windows/src/air_mouse_tuning_window.{h,cc}` | 新建：非模态调参窗口 + GDI 曲线 + ring buffer | O9 |
| `desktop/windows/src/win32_app.{h,cc}` | 托盘菜单入口 + 窗口生命周期 + 回调接线 | O9 |
| `desktop/windows/tests/core_tests.cc` | 5 个新测试 + 6 个旧曲线测试改签名 | O9 |
| `CLAUDE.md` / `AGENTS.md` | 体感鼠标段补滞回判据与热调参面板说明 | 两 |
| `Doc/Ref/protocol.md` | （O9 范围外固件热调预留位，本次不改） | — |

**不改**：BLE motion 帧格式、settings_dialog 体感灵敏度 UI（保留，热调面板独立）、macOS 端（见记忆 `windows-only-no-macos-streaming`）、增益曲线形状语义（仅 constexpr→运行期，默认值不变）。

---

## 第五部分：风险与回退

### O4 风险

- **静止漂移回归**：滞回状态机若 `OMEGA_EXIT` 过低，ACTIVE 帧内残余噪声经死区后仍可能累积。缓解：死区 3dps 保留作硬截断；`OMEGA_EXIT=2` 低于死区，ACTIVE→STILL 后立即停止上报。回退：`OMEGA_ENTER=OMEGA_EXIT=死区=3` 退化为无滞回（但保留 jerk→幅值判据的修复）。
- **EMA 收敛变慢**：解耦后 EMA 仅在 STILL+jerk 静止时收敛，收敛窗口变窄。缓解：`BIAS_ALPHA=0.05` 不变，长期仍收敛；进入即加载 NVS 持久化零偏（已实现）保证首响应。回退：恢复 still 帧即 EMA（但 reintroduce 耦合）。
- **匀速慢转 EMA 不收敛致零偏漂**：匀速慢转 ACTIVE 时不收敛零偏，若此时温漂，零偏暂不更新。可接受——温漂缓慢，停手即恢复收敛。

### O9 风险

- **曲线参数运行期化破坏现有标定**：默认值须严格等于当前 constexpr（15/50/0.15/4.0）。`TestAirMouseGainFactorDefaultCurveMatchesLegacy` 守护。
- **热调面板线程安全**：面板、AirMouseTick、UpdateAirMouseParams 均在 UI 线程（WM_TIMER/对话框回调同线程），无锁安全。需确认 tuning window 的 WM_PAINT 不与 AirMouseTick 竞争 ring buffer——同线程串行，安全。
- **配置文件膨胀**：4 个曲线参数进 config.toml。可接受（用户可不管，用默认）。
- **回退**：曲线参数设默认值 = 当前 constexpr，行为完全等价；热调面板不打开即无影响。

### 整体回退

- O4：`OMEGA_ENTER/EXIT` 设为死区值，状态机退化为"幅值判据无滞回"，仍优于纯 jerk（保留匀速慢转修复）。
- O9：曲线参数默认值 = constexpr，`AirMouseGainFactor` 签名变更但默认行为不变；热调面板可选。

---

## 第六部分：实施顺序

1. **RFC 确认**（本文档）。
2. **O4 先行**（固件，独立可验）：
   1. 改 `bmi270.c` 滞回状态机 + EMA 解耦 + 日志。
   2. `idf.py build`（或 `idf_cli.py -c`）编译。
   3. 真机验证（3.1 用例）+ 串口日志统计 ACTIVE 占比。
   4. 提交（固件改动）。
3. **O9 后行**（桌面端，依赖 O4 的 still/active 信号可视化）：
   1. TDD 红灯：`core_tests.cc` 5 个新测试 + 6 个旧曲线测试改签名，ctest 确认失败。
   2. TDD 绿灯：`air_mouse_kin.{h,cc}` 曲线运行期化 + `app_config` 配置项 + `UpdateAirMouseParams` + 调参窗口 + win32_app 接线。
   3. ctest 全绿 + `build_win.bat` 构建。
   4. 真机联调：热调面板实时改参看曲线，标定 O4 滞回阈值。
   5. 提交（Windows `git add -f`）。
4. **文档同步**：`CLAUDE.md`/`AGENTS.md` 体感鼠标段补滞回与热调参说明。
5. **真机标定迭代**：用 O9 面板标定 O4 `OMEGA_ENTER/EXIT` + 曲线参数，按第五部分回退点微调。

---

## 第七部分：前置检查清单（PCT 阶段一）

- [x] 数据模型：`AirMouseCurveParams`（O9）、`s_air_mouse_is_active` 状态机（O4）已定义。
- [x] 接口契约：`AirMouseGainFactor(omega, curve)`、`UpdateAirMouseParams(params)`、`air_mouse_update_active(...)` 已明确。
- [x] 边界条件：匀速慢转（jerk 小 omega 大）、静止噪声、边界抖动、停手余震、甩动五场景已识别并验证。
- [ ] 真机标定阈值：`OMEGA_ENTER/EXIT` 初值待真机确认（O9 面板辅助）。
