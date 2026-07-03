# 体感鼠标零偏校准持久化与进入即响应（方案 A）

## 背景与根因

现状进入体感校准流程（`bmi270.c`）：

1. `bmi270_air_mouse_start()` 清零偏 `has_bias=false`、重置校准累加。
2. 校准期（`!has_bias`）用相邻帧 jerk 判静止（三轴变化均 <`AIR_MOUSE_STILL_JERK_DPS`=3 dps），不静止就清空累加重来；攒够 `AIR_MOUSE_CALIB_FRAMES`=10 帧（≈200ms）连续静止才定标 `bias=mean`。
3. **校准期始终 `return false`，不产生位移**。
4. 运行期：减零偏 → jerk 判静止 → 静止时 EMA(α=0.05) 精校 bias 且不产出 → 死区 4 dps → scale 0.6 → clamp ±127。

进入体感"延迟响应"根因：

- 校准期光标完全不动（`return false`），是延迟源头。
- 必须连续 10 帧静止才定标；按侧键进入瞬间手有余震 → jerk 超阈值 → 反复清零 → 校准完成时间不确定（实测 500ms~2s）。
- 每次进入都从零校准，不记忆上次 bias；同温度下 bias 本可复用。
- 无 settling 等待，第一帧就开始判静止，余震必然触发清零。

运行期已有 EMA(α=0.05) 静止精校 + 死区 4 dps 兜底，温漂可自愈——这是方案 A 后台精校的基础。

## 方案 A 设计

核心：NVS 持久化上次稳定 bias，进入体感立即用持久化值响应；settling 期冻结 EMA 防按键余震污染；首次无持久化值时退化为现有校准逻辑。

### NVS 持久化

- 复用 namespace `"voicestick"`（与 `pickup_thr`/`tap_en`/`tap_lvl2` 同库）。
- keys：`am_bias_x` / `am_bias_y` / `am_bias_z`，类型 `i32`，存 `lroundf(bias_dps * 100.0f)`，读回 `/ 100.0f`。精度 0.01 dps（远小于死区 4 dps），范围 ±5 dps 内足够。
- 三 key 全部读到才算命中；任一缺失视为无持久化值（首次使用）。
- 存盘时机：首次定标完成后存一次；退出体感时若 `bias_dirty` 存一次。

### 进入流程（`bmi270_air_mouse_start`）

1. `load_persistent_bias()`：三 key 都读到 → `has_bias=true`，`bias=读回值`；否则 `has_bias=false`。
2. 设 `settle_until_us = now + AIR_MOUSE_SETTLE_MS(150ms)`（按键余震冻结期）。
3. 无持久化值 → 现有校准流程（清累加、`has_prev=false`）。
4. `bias_dirty=false`。

### 轮询（`bmi270_air_mouse_poll`）

**首次校准期（`!has_bias`）：**

- settling 内（`now < settle_until_us`）跳过采样（等余震消退），`return false`。
- settling 后：现有 jerk 静止 + 10 帧定标逻辑不变。
- 定标完成：`has_bias=true`，`save_persistent_bias()`，`bias_dirty=false`。

**运行期（`has_bias`，含持久化进入）：**

- 减零偏 → 死区 → scale → clamp → **产出位移**（进入即响应）。
- jerk 判静止：
  - `still && now > settle_until_us` → EMA 更新 bias，`bias_dirty=true`，`return false`（静止不产出）。
  - settling 内（`now < settle_until_us`）即使判静止也**不 EMA**（防余震污染持久化 bias），但仍按正常流程产出位移（持久化 bias 准确，余震是真实运动应产出）。

### 退出（`bmi270_air_mouse_stop`）

- `if (bias_dirty) save_persistent_bias();`

### 新增状态变量

- `s_air_mouse_settle_until_us`（int64_t）：settling 截止时刻。
- `s_air_mouse_bias_dirty`（bool）：运行期 EMA 是否改过 bias，决定退出是否存盘。

## 参数表

| 参数 | 值 | 说明 |
|---|---|---|
| `AIR_MOUSE_SETTLE_MS` | 150 | 进入后余震冻结 EMA 的时长 |
| `AIR_MOUSE_CALIB_FRAMES` | 10 | 首次校准帧数（不变） |
| `AIR_MOUSE_BIAS_ALPHA` | 0.05 | EMA 系数（不变） |
| `AIR_MOUSE_STILL_JERK_DPS` | 3.0 | 静止 jerk 阈值（不变） |
| `AIR_MOUSE_DEADZONE_DPS` | 4.0 | 死区（不变） |
| NVS keys | `am_bias_x/y/z` | i32，×100 存 |

## 影响范围

- `firmware/components/bmi270/bmi270.c`（核心改动：NVS 读写、settling、定标/退出存盘）。
- `firmware/components/bmi270/include/bmi270.h`（接口注释更新）。
- `firmware/components/bmi270/CMakeLists.txt`（`REQUIRES nvs_flash`，若未含）。
- **不改**：BLE 协议 / motion 帧格式、桌面端、运行时速度控制模型（`air_mouse_kin`，本次会话已改）、敲击检测。

## 验证计划

固件无自动化单测（CLAUDE.md 固件测试策略），按 `idf.py build` 编译 + 真机串口日志验证：

1. `idf.py build` 编译通过。
2. 真机：
   - 清 NVS（`erase-flash`）后首次进入体感：settling(150ms) + 校准(~200ms) 后可用。
   - 退出再进：**立即响应**（持久化 bias），无 200ms+ 延迟。
   - 进入后立即抖动手腕：光标按持久化 bias 响应；settling 内不漂（EMA 冻结）。
   - 静置后 EMA 精校温漂；退出再进仍立即响应。
3. 串口日志：`air mouse bias loaded=...` / `bias saved=...` / `settling` 状态。

## 风险与对策

- **温度大变后持久化 bias 偏**：settling(150ms) 后即开始 EMA 精校，几秒收敛；死区 4 dps 兜底小偏差。
- **NVS 写次数**：仅退出体感时写，频率低（用户进出体感次数有限），NVS 寿命充足。
- **首次无持久化仍延迟**：无先验不可避免，仅首次；之后均立即响应。
