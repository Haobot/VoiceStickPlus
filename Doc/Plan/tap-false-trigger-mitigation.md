# 双击敲击检测误触治理：按键抑制 + ACC 轴分量方向判据

## 背景

用户反馈：IMU 双击敲击检测存在两类误触——
1. **按语音识别键时误触发双击**（VK_DOWN 被误注入）。
2. 一般手持使用中误触。

用户提出"融合麦克风做触发 + 陀螺仪判断敲击/滑动方向"的构思。经评估（见下"否决方案"），麦克风与 GYR 方案存在物理与架构误区。本 RFC 采用纯 IMU 软件层方案，不动音频链路。

## 误触根因（已核查）

### 根因1：按键与 tap 检测的时序门控漏洞

`tap_poll_timer_cb` 的门控条件（`firmware/main/main.c:1531`）：

```c
if (!s_tap_enabled || !voice_ble_is_connected() || s_recording || s_ota_updating) return;
```

tap 轮询仅在 `s_recording` 置 true 后才关闭。但从物理按键按下到 `start_recording()` 成功之间存在窗口：

- **hold_to_talk**：按下后需等 300ms hold threshold（`main.c:51` `DOUBLE_CLICK_MAX_PRESS_MS=300`）确认长按，期间 UI 仍 READY、`s_recording` 仍 false。
- 加上 80ms 提示音（`main.c:512`）+ codec/I2S 初始化时间。

该窗口内 tap 轮询仍在运行，按语音键的手指动作若产生 ACC 脉冲，会被状态机误判为双击第一击或第二击。**这是按语音键误触的直接原因。**

### 根因2：ACC 合幅值丢失轴向信息

当前 `detect_tap_impulse` 把三轴 ACC 压成合幅值 `sqrt(x²+y²+z²)`（`bmi270.c:493-506`），轴向信息丢失：

- 敲击：某一轴出现尖锐单向脉冲。
- 放桌上震动、按键整体平移、设备被碰：三轴低频均匀扰动。

两者合幅值都可能超阈值，算法无法区分。**GYR 在此帮不上忙**——GYR 测角速度（旋转），敲击和滑动时 GYR 都是小值，无法区分（详见否决方案）。

## 否决方案（用户构思，附否决理由）

### 否决 A：麦克风做触发

- **架构死结**：麦克风当前仅在录音时开 I2S（`audio_pipeline.c:447-466` per-session），而 tap 用途恰在非录音态。两者时间互斥（`main.c:1531` 录音时关 tap）。
- 要让麦克风在非录音态做触发，需 I2S+codec+麦克风常驻采集，代价：功耗（电池设备）、隐私（麦克风常开）、提示音冲突（codec 被占）。
- **引入新误触源**：麦克风无法区分"敲壳体结构声"与"说话/打字/环境声"，而 IMU 的核心优势恰是只感知本体运动不受环境声影响。融合麦克风反把 IMU 没有的误触源带进来。收益不抵代价。

### 否决 B：陀螺仪判断敲击 vs 沿壳体滑动方向

- **物理误区**：GYR 测角速度（旋转），不测平移方向。
  - 敲击（垂直壳体）：ACC 法向单向尖锐脉冲，GYR 小。
  - 沿壳体滑动（设备被手持固定）：设备本身不动，IMU 几乎无信号；即便被带动平移，平移不产生角速度，**GYR 仍小**。
- 两者 GYR 都小，GYR 无法区分。当前算法已用 `gyr_mag <= calm_thr` 做"挥动/旋转排除"（`bmi270.c:544`），这是 GYR 能做的上限。
- 区分敲击/滑动应基于 **ACC 轴分量分布与波形形态**，不是 GYR。

## 设计

### 改动1：按键事件抑制 tap 检测（根治根因1，最简最高优先）

在 `tap_poll_timer_cb` 增加按键抑制门控。按键事件（`APP_EVENT_FRONT_DOWN/UP`、远程按键同理）触发后，设置一个"抑制截止时间戳"，该窗口内 tap 轮询直接 return。

```c
// main.c 新增静态变量
static int64_t s_tap_suppress_until_us = 0;

// handle_primary_down / handle_primary_up 入口处（main.c:792、898，note_activity() 旁）：
static void handle_primary_down(app_input_source_t source, uint32_t request_id) {
    s_tap_suppress_until_us = esp_timer_get_time() + (TAP_SUPPRESS_AFTER_BUTTON_MS * 1000LL);
    note_activity();
    ...
}

// tap_poll_timer_cb 门控（main.c:1531）：
if (!s_tap_enabled || !voice_ble_is_connected() || s_recording || s_ota_updating) return;
if (esp_timer_get_time() < s_tap_suppress_until_us) return;  // 按键抑制窗口
if (bmi270_tap_poll()) queue_app_event(APP_EVENT_TAP);
```

参数 `TAP_SUPPRESS_AFTER_BUTTON_MS`：覆盖"按下→录音启动"+"松开后的手指余震"。hold_to_talk 按下到 `s_recording=true` 约 300ms（hold）+ 提示音 80ms + 初始化，取 **600ms** 较稳妥；松开时同理设一次。按键事件本身就是双击可能的诱因，600ms 窗口也能吞掉按键引发的双击误判。

**影响范围**：仅非录音态。录音态本就 `s_recording` 门控关闭 tap，抑制窗口与录音态不冲突。按键抑制期间用户不会去敲设备做 VK_DOWN（正在按语音键），无体验损失。

**注意点**：
- `s_tap_suppress_until_us` 仅在 `tap_poll_timer_cb`（timer 任务）和 `handle_primary_*`（app_event 任务）读写，两者都是任务上下文非 ISR，单 64 位读写在 ESP32-S3 上原子性可接受，无需锁。若严谨可加 `portMUX_TYPE`，但 10ms 轮询的容忍度高，过设计不必。
- 远程按键（`remote_button_down/up`，`main.c:716-723`）同样走 `APP_EVENT_FRONT_DOWN/UP`，自动覆盖。

### 改动2：ACC 轴分量方向判据（治理根因2，提升敲击/扰动判别力）

改造 `detect_tap_impulse` 与 `read_acc_mag_and_gyr_mag`，保留三轴分量而非压成合幅值。判据：

1. **主轴判定**：找出当前帧 ACC 三轴分量绝对值最大的轴 `dominant_axis`，及其 `delta_dom = |a_dom - baseline_dom|`。
2. **集中度判据**：要求主轴 delta 占合 delta 的比例 ≥ 阈值（如 0.6）：
   `delta_dom / (delta_x + delta_y + delta_z) >= axis_concentration_ratio`
   - 敲击：能量集中在单一轴，比例高 → 通过。
   - 三轴均匀扰动（桌面震动、整体平移）：比例低 → 拒绝。
3. **方向一致性**（可选增强）：敲击的尖峰在主轴上单向（先正后负回弹），可加"主轴符号一致性"判据。但当前 100Hz 采样对 <20ms 脉冲欠采样，符号判据可靠性有限，列为可选。

基线逻辑同步改为**按轴维护**：`baseline_x/y/z` 各自 EMA 跟随，冲击期冻结、平静期慢跟随（沿用现有逻辑，仅扩展到三轴）。

```c
// bmi270.c 状态变量扩展
static float s_tap_baseline[3] = {0, 0, 0};  // x/y/z 各自基线
static bool s_tap_has_baseline = false;

// 命中条件改为：
//   (1) 主轴 delta >= acc_thr_g
//   (2) 主轴集中度 >= axis_concentration_ratio
//   (3) gyr_mag <= gyr_calm_thr_dps  （保留原挥动排除）
```

`tap_params_t` 新增 `axis_concentration_ratio` 字段（各档统一 0.6，或随档位微调）。档位表 1~10 的 `acc_thr_g`/`gyr_calm_thr_dps` 不变，仅增字段。

### 改动3（可选）：脉冲宽度形态判据

敲击脉冲宽 <20ms，滑动/碰撞是宽波形。当前 100Hz（10ms 周期）欠采样，形态判据可靠性有限。**本 RFC 暂不实施**，列为后续可选。若要可靠做形态判据，需先提高 ACC ODR（如 400Hz）或启用 BMI270 hardware tap feature——后者需核实当前 config file 是否支持 single/double tap feature（已确认 any-motion feature 可用并路由 INT1，`bmi270.c:255-256`，但 tap feature 未核实，单独立项）。

## 边界与不变量

- **改动1 不影响录音态 tap 门控**：录音时 `s_recording` 仍优先 return，抑制窗口仅作用于非录音态。
- **改动1 不影响 tap 灵敏度档位**：抑制是时序门控，不改阈值。
- **改动2 不改协议**：`tap_sensitivity` 1~10 字段语义不变，仅算法内部判据增强。`axis_concentration_ratio` 是固件内部参数，不暴露桌面端。
- **改动2 基线迁移**：现有单值基线 `s_tap_acc_baseline` 改为三轴数组，NVS 不存基线（基线是运行时状态），无迁移问题。
- **GYR 挥动排除保留**：改动2 的判据 (3) 保留 `gyr_mag <= gyr_calm_thr_dps`，不破坏现有"挥动排除"能力。
- **双击间隔/去抖参数不变**：`TAP_MIN_GAP_MS=80`/`TAP_MAX_GAP_MS=400`/`TAP_DEBOUNCE_MS=50` 保留。

## 验证

固件无自动化单测，靠 `idf.py build` 编译 + 真机：

1. **编译**：`idf.py build` 通过。
2. **真机-改动1**：
   - hold_to_talk 模式按语音键，重复 20 次，统计 VK_DOWN 误注入次数 → 应为 0（对照改动前）。
   - click_to_talk 模式同上。
   - 按键后立即（<600ms）敲设备，应不触发 VK_DOWN（抑制窗口内）；按键后 >600ms 敲设备，应正常触发。
3. **真机-改动2**：
   - 正常双击壳体 → VK_DOWN 正常触发（不回归）。
   - 设备放桌面、敲桌子 → 应不触发（三轴均匀扰动被集中度判据拒绝）。
   - 手持轻微晃动 → 应不触发（GYR 排除 + 集中度判据）。
   - 各档位 1/5/10 抽测灵敏度不回归。
4. **不回归**：tap_enabled 开关、NVS 持久化、`show_imu_debug` 屏显不受影响。

## 实施顺序

1. **先做改动1**（按键抑制）：改动极小、零风险、根治最痛的"按语音键误触"。单独验证效果。
2. 若改动1 后一般手持误触仍明显，再做**改动2**（轴分量判据）。
3. 改动3 待 ODR/hardware tap 核实后单独立项。

## 相关

- 现有算法与档位表：`firmware/components/bmi270/bmi270.c`、记忆 `imu-tap-detection-impl`、`tap-sensitivity-10-levels`。
- 否决的麦克风方案依据：`firmware/components/audio_pipeline/audio_pipeline.c`（per-session 资源、PCM 局部变量）。
