# IMU 敲击检测方案

## 概述

利用 StickS3 板载 BMI270 的加速度计 + 陀螺仪，通过**软件状态机**检测用户**手持设备时双击设备本体**的手势：用户手持 StickS3，用手指快速双击设备外壳，桌面端收到事件后注入一次**下方向键（VK_DOWN）**，用于在输入候选/列表选项之间向下切换。

新功能与现有按住录音、双击主键注入 Enter、IMU 拿起亮屏等行为互不冲突。

## 已确认的设计前提

| 前提 | 证据 |
|---|---|
| BMI270 硬件内置 single/double tap feature engine | Bosch 官方 + 多源技术资料印证，tap 检测为 BMI270 片上智能的标配能力 |
| feature engine 在线态已就绪 | `bmi270.c:287` `load_config_file()` 已加载 8KB config file，`INTERNAL_STATUS=0x01` |
| 配置机制与 any-motion 同构 | `configure_any_motion()`（`bmi270.c:167-203`）提供 page+offset feature 写法范本 |
| 中断状态位可轮询读清 | BMI270 `INT_STATUS` 寄存器位锁存至读清，INT1 不接 ESP32（`bmi270.h:10-11`）不影响轮询路径 |
| 桌面端 VK_DOWN 注入底层能力已具备 | `input_injector_win.cc:45-51` `SendKey()` 私有方法已封装 `SendInput`+`INPUT_KEYBOARD`，支持任意虚拟键码 |
| BLE 事件通道可复用 | `ble_protocol` 通用解析 `event` 字符串，`button_double_click` 已走通 state_tx → coordinator → 注入链路 |

## 关键设计决策

### 1. 检测算法：软件状态机（ACC + 陀螺仪），渐进验证

用户已选定**渐进路径**：先用软件算法验证手持双击的信号特征与误触发情况，效果不够再考虑换 config file 上硬件 engine。

**软件算法真正结合两传感器**（回到用户原始诉求）：
- **加速度计**：检测敲击产生的瞬态冲击脉冲（合加速度幅值突变）。
- **陀螺仪**：在加速度脉冲命中后，读取事件前后窗口的角速度幅值，排除"设备被整体挥动/转动"的伪触发——手持场景下挥动会被纯加速度算法误判为敲击，陀螺仪是关键判别量。

此路径不动 config file（保持 non-legacy），不波及关机态 any-motion 唤醒。代价：需自写状态机、自调参、提升采样率。

### 1a. 软件算法状态机

```
        ┌──────────┐
        │  IDLE    │  持续采样 ACC+GYR
        └────┬─────┘
             │ |Δ合加速度| ≥ TAP_ACC_THR
             │ 且窗口内 GYR 幅值 < GYR_CALM_THR
             │ （排除挥动：敲击是高频冲击，挥动是低频整体运动）
             ▼
        ┌──────────┐
        │ TAP1     │  记录首击时间戳 t1
        └────┬─────┘
             │ 在 [TAP_MIN_GAP, TAP_MAX_GAP] 内出现第二击
             │  （同样判据：ACC 突变 + GYR 平静）
             ▼
        ┌──────────┐
        │ DOUBLE   │  上报 double tap → IDLE
        └──────────┘
             │ 超过 TAP_MAX_GAP 无第二击
             ▼
        ┌──────────┐
        │ SINGLE   │  可选：上报 single tap（预留）→ IDLE
        └──────────┘
```

判别核心：**加速度脉冲 + 陀螺仪平静**。敲击设备本体时，加速度出现尖锐脉冲，但设备整体姿态不变，陀螺仪角速度保持小；挥动设备时，加速度也会突变，但陀螺仪角速度同时大幅上升。用 `GYR 幅值 < GYR_CALM_THR` 作为"非挥动"的确认条件。

### 1b. 采样率与功耗

- tap 脉冲宽度约 10-50ms，Nyquist 要求采样率 ≥ 100Hz。
- 当前在线态 ACC 是 100Hz ODR（`bmi270.c:46` `BMI270_ACC_CONF_NORMAL_100HZ`），但 `imu_poll_timer_cb` 仅 200ms 轮询（5Hz），**严重不足**。
- 需新增专用 tap 采样定时器，周期 **10ms**（100Hz）。仅读 ACC 6 字节 + GYR 6 字节，单次 I2C ~12 字节@400kHz 约 0.3ms，10ms 周期下 I2C 占用约 3%，可接受。
- 录音/识别态可暂停 tap 采样（见交互冲突），降低功耗与总线争用。

### 1c. 软件算法参数（初始值，需真机调参）

| 参数 | 初始值 | 含义 |
|---|---|---|
| `TAP_ACC_THR` | ~1.5g（≈6144 LSB @2g） | 合加速度相对静止基线的突变阈值 |
| `GYR_CALM_THR` | ~30 dps | 事件窗口内角速度幅值上限，低于此判为"非挥动" |
| `TAP_MIN_GAP` | 80ms | 两击之间最小间隔 |
| `TAP_MAX_GAP` | 400ms | 两击之间最大间隔，超此判单击 |
| `TAP_DEBOUNCE` | 50ms | 检出一次脉冲后的屏蔽期，防单次冲击多次触发 |

三档灵敏度（low/medium/high）通过调整 `TAP_ACC_THR` 与 `GYR_CALM_THR` 实现。

### 2. 检测位置：固件端

与现有"双击主键"方案一致，手势检测在固件端完成，桌面端仅响应新 BLE 事件。理由同 `primary-button-double-click.md`：固件持有原始 IMU 数据，避免无效上报，协议最小侵入，符合"固件报事实、桌面端做决策"原则。

### 3. 事件通道：新增独立 `tap` 事件

不复用 `button_double_click`（语义不同：一个是按键双击，一个是敲击设备）。新增 `{"event":"tap","kind":"double"}` 走 state_tx notify。`ble_protocol` 无需改动（已通用解析 `event` 字段）。

### 4. 平台范围：仅 Windows

memory 记录流式精修仅 Windows 端实施，macOS 后续推进。macOS 端 `InputInjector.swift` 未暴露 `sendKey(virtualKey:)`，需额外添加，本期不做。

## ⚠️ 实施前置依赖（已通过 Bosch 官方源码核实）

核实方式：`git clone https://github.com/boschsensortec/BMI270_SensorAPI.git`，直接读取 `bmi2_defs.h` / `bmi270_legacy.c` / `bmi270_legacy.h` / `bmi270.c` 源码。以下数值均带源码行号引用，可复核。

### double-tap feature 真实寄存器布局

| 项 | 值 | 源码引用 |
|---|---|---|
| double-tap feature page | **Page 5**（primary）+ **Page 6**（secondary） | `bmi270_legacy.c:516` `{.type=BMI2_DOUBLE_TAP, .page=BMI2_PAGE_5, .start_addr=BMI270_LEGACY_TAP_DETECT_1_STRT_ADDR}`；secondary 见 `:519` `BMI2_TAP_DETECTOR_2 → BMI2_PAGE_6` |
| start_addr（primary） | **0x00** | `bmi270_legacy.h:86` `BMI270_LEGACY_TAP_DETECT_1_STRT_ADDR = 0x00` |
| start_addr（secondary） | **0x00** | `bmi270_legacy.h:87` `BMI270_LEGACY_TAP_DETECT_2_STRT_ADDR = 0x00` |
| 字段布局（primary, page 5） | word 对齐：word0=`data_reg_en`(bit3 mask 0x08)、word2=`tap_sens_thres`、word3=`max_gest_dur`、word7=`quite_time_after_gest` | `bmi270_legacy.c:2671-2696` |
| 字段布局（secondary, page 6） | word0=`wait_for_timeout`、word2=`axis_sel`(bit0-1 mask 0x03) | `bmi270_legacy.c:2765-2777` |
| double-tap 中断状态位 | **`INT_STATUS_0`(0x1C) bit6 = 0x40** | `bmi270_legacy.h:142` `BMI270_LEGACY_INT_DOUBLE_TAP_MASK = 0x40` |
| single-tap 中断位 | bit5 = 0x20 | `bmi270_legacy.h:141` |
| tap 合并中断位 | bit3 = 0x08 | `bmi270_legacy.h:136` `BMI270_LEGACY_INT_TAP_MASK = 0x08` |
| feature enable | `BMI2_TAP_FEAT_EN_MASK = 0x01`（page 5 word0 bit0） | `bmi2_defs.h:644` |
| INT_STATUS 锁存与清位 | 状态位读后自动清（BMI270 特性） | datasheet |

寄存器地址（与仓库现有 `bmi270.c` 一致）：`FEAT_PAGE=0x2F`、`FEATURES=0x30`、`INT_STATUS_0=0x1C`、`INT1_MAP_FEAT=0x56`（`bmi2_defs.h:207,218,219,238`）。

### 🚨 关键阻塞：config file variant 不匹配

**仓库当前 8KB config file 是 non-legacy 版本，tap feature 在该 config file 下不可用。**

核实证据（逐字节比对）：
- 仓库 `firmware/components/bmi270/bmi270_config_file.h` 的 8192 字节与官方 `bmi270.c` 中 `bmi270_config_file[]` **完全相同**（8192/8192 字节匹配）。
- 与官方 `bmi270_legacy.c` 中 `bmi270_legacy_config_file[]` **不同**（前 100 字节即不匹配）。
- non-legacy `bmi270.c` 的 feature 中断表 `bmi270_map_int`（`bmi270.c:519-528`）**只注册 sig/step/wrist/any_motion/no_motion，不含 tap**。
- tap 的全部实现（page 5/6 配置 + 中断 bit 0x40）只在 `bmi270_legacy.c`，对应 legacy config file。
- 旁证：仓库 `configure_any_motion()` 写 page 1 offset 0x0C，正是 non-legacy 的 `BMI270_ANY_MOT_STRT_ADDR=0x0C`（`bmi270.h:76`）；legacy 的 any-motion 在 `page 1 offset 0x06`（`bmi270_legacy.h:77`）。

**结论**：要启用 double-tap，必须将 config file 从 `bmi270_config_file`（non-legacy）**替换为 `bmi270_legacy_config_file`**（legacy）。这不是"配几个寄存器"的小改动，而是替换 feature engine 固件，会波及现有功能。

### 换 config file 的连带影响（必须先评估）

替换为 legacy config file 后，现有 any-motion 拿起唤醒路径会受影响：

1. **在线态 any-motion**：仓库当前未启用 any-motion feature（在线态只读 ACC 软件差分），不受影响。
2. **关机态 `bmi270_enable_pickup_wake()`**（`bmi270.c:412-451`）：调用 `configure_any_motion()` 写 page 1 offset 0x0C。换 legacy config file 后，any-motion 的 start_addr 从 0x0C 变 0x06，**`configure_any_motion()` 必须同步改为 offset 0x06**，否则关机态拿起唤醒失效。
3. **feature 中断号变化**：legacy 的 any-motion 中断掩码 `BMI270_LEGACY_INT_ANY_MOT_MASK` 与 non-legacy 的 `BMI270_INT_ANY_MOT_MASK` 数值可能不同，`INT1_MAP_FEAT` 写入值需核对。
4. **config file 加载流程不变**：`load_config_file()` 的 INIT_ADDR/INIT_DATA burst 写法与 variant 无关，8192 字节直接替换即可。

### 待用户决策

换 config file 是不可逆地改动现有 feature engine 基础，影响关机态唤醒这条已上线功能。请确认是否接受以下二选一：

- **方案 A**：换用 legacy config file，同步改造 `configure_any_motion()` 适配新 offset/中断号，承担关机态唤醒需要重新真机验证的风险。
- **方案 B**：放弃硬件 tap engine，改走**软件算法**（ACC+陀螺仪，原用户诉求的第二选项），不动 config file，但需自写状态机并提升采样率。

> 历史记录：本节早先版本曾填入"Page 1 / Offset 0x00 / tap_select bit0-2 / threshold bit2-5 / INT_STATUS_0 bit4 / 从 Espressif 官方驱动获取"等数值，经核对 Bosch 官方源码后确认**全部错误**（page 应为 5/6，字段为 word 对齐多字段，中断位为 bit6/0x40），且"Espressif 官方驱动"查无实据。已删除。

## 固件侧设计（软件算法路径）

### bmi270 组件改动

`firmware/components/bmi270/`：

1. **启用陀螺仪**：当前 `bmi270_init()`（`bmi270.c:298`）只写 `BMI270_PWR_CTRL_ACC_EN`(0x04)。改为 `ACC_EN | GYR_EN`（0x06），并配 `GYR_CONF`/`GYR_RANGE`。
   - `GYR_RANGE`：±1000dps（0x01）或 ±500dps（0x02），手持敲击角速度不大，±500dps 精度更高。
   - `GYR_CONF`：100Hz ODR 与 ACC 对齐（性能模式）。
   - 新增 `BMI270_REG_GYR_X_LSB`(0x12)、`BMI270_REG_GYR_CONF`(0x42)、`BMI270_REG_GYR_RANGE`(0x43) 寄存器常量。

2. 新增 `bmi270_read_gyr_dps(float *x, float *y, float *z)`：仿 `bmi270_read_acc_g()`（`bmi270.c:367-384`），读 GYR 6 字节并按量程换算。MPU6886 分支同样补陀螺仪读取（其 `PWR_MGMT_2` 当前禁了 gyro，需放开）。

3. 新增 tap 检测状态机（不依赖 feature engine，纯软件）：
   ```c
   typedef enum { TAP_IDLE, TAP_FIRST, TAP_WAIT_SECOND } tap_state_t;

   bool bmi270_tap_poll(void);  // 10ms 调用一次，返回本次是否检出 double-tap
   ```
   - 内部维护 `tap_state_t`、`t1_us`、静止基线（合加速度慢跟随）。
   - 每次调用：读 ACC+GYR → 算合加速度与角速度幅值 → 状态机推进（见 1a 状态图）。
   - 命中 double-tap 返回 true 并回到 IDLE。

4. 新增配置接口：
   ```c
   void bmi270_set_tap_enabled(bool enable);
   void bmi270_set_tap_sensitivity(int level);  // 0=low 1=medium 2=high
   ```
   - `enable=false` 时 `bmi270_tap_poll()` 直接返回 false，不读 GYR（省功耗）。
   - `level` 调整 `TAP_ACC_THR`/`GYR_CALM_THR` 档位常量。

5. **不动 config file、不动 `configure_any_motion()`、不动关机态 `bmi270_enable_pickup_wake()`**。软件算法与 feature engine 完全解耦，关机态唤醒不受影响。

### main.c 改动

1. 新增 10ms 周期专用软件定时器 `tap_poll_timer`：
   - 周期常数 `#define TAP_POLL_PERIOD_MS 10`
   - 回调中若 `s_tap_enabled` 且当前非录音/识别态，调 `bmi270_tap_poll()`，命中则 `esp_event_post(APP_EVENT_TAP, ...)`。
   - **状态门控**：录音/识别态暂停轮询（见交互冲突），降低 I2C 总线与 Opus 编码争用。

2. 事件处理（`main.c` 事件循环）：
   - `APP_EVENT_TAP`：仅在 BLE 已连接时上报（未连接不上报），调 `voice_ble_send_tap("double")`。

3. control_rx 新增字段（`ble_control_cb`，`main.c:666-739`）：
   - `{"event":"tap_enabled","enabled":<bool>}` → `bmi270_set_tap_enabled()` + 写 NVS 键 `tap_en`。
   - `{"event":"tap_sensitivity","level":"low"|"medium"|"high"}` → 映射到 level int → `bmi270_set_tap_sensitivity()` + 写 NVS 键 `tap_lvl`。
   - 开机恢复：`load_tap_settings_from_nvs()`（仿 `load_pickup_threshold_from_nvs()`，`main.c:1649-1671`）。

4. **调试辅助**：复用 `show_imu_debug` 开关（`main.c:80,702-705`），开启时在屏幕额外显示 GYR 幅值与 tap 状态机当前态/命中计数，便于真机调参。

### voice_ble 组件改动

`firmware/components/voice_ble/`：

- `voice_ble.h` 新增声明：
  ```c
  void voice_ble_send_tap(const char *kind);  // kind: "double" / "single"(预留)
  ```
- `voice_ble.c` 实现，组 JSON `{"event":"tap","kind":"double"}` 调 `send_state_json`（仿 `voice_ble_send_button_double_click`，`voice_ble.c:1064-1071`）。
- **MTU 注意**：`control_rx` 写入受 BLE MTU 限制，但 `tap` 事件是 state_tx notify（设备→主机），JSON 短，无溢出风险。

### 交互冲突处理

| 设备状态 | tap 事件行为 |
|---|---|
| 未配对/未连接 | 不上报（BLE 未连接） |
| 连接空闲 | 上报 tap，桌面端注入 VK_DOWN |
| 录音中 | **建议忽略**（避免敲击震动干扰录音期间切换选项，与"录音中侧键不取消"的保守策略一致） |
| 识别中 | 忽略（与"识别中忽略新录音"一致） |
| 确认倒计时/手动确认中 | 上报（用户可能想切换候选） |

固件在 `APP_EVENT_TAP` 处理中按当前 `s_ui_state` 门控：仅 `idle`/`confirm_countdown`/`manual_confirm` 态上报。

## Windows 桌面侧设计

### 注入接口

`desktop/windows/src/input_injector_win.h` + `voice_stick_coordinator.h:143-148`：

- `InputInjector` 基类新增：
  ```cpp
  virtual void SendArrowDown() = 0;
  ```
- `InputInjectorWin` 实现（复用已有私有 `SendKey`，`input_injector_win.cc:45-51`）：
  ```cpp
  void InputInjectorWin::SendArrowDown() {
      SendKey(VK_DOWN, true);
      SendKey(VK_DOWN, false);
  }
  ```

### 事件分发

`voice_stick_coordinator.cc`：

- `HandleStateEvent`（行 326-345）新增分支：
  ```cpp
  else if (event.event == "tap") {
      HandleTapEvent(event, device_id);
  }
  ```
- 新增 `HandleTapEvent`：
  - 检查 `config_.tap_to_arrow` 开关，关闭则忽略。
  - 调 `input_injector_->SendArrowDown()`。
  - 回写 `ready` UI 状态（`EnterReady("tap_arrow_down")`）。
  - 与 `HandleButtonDoubleClick`（行 405-427）不同：不取消录音、不取消字幕（tap 在录音中已被固件门控掉，桌面端收到时一定是空闲/确认态）。

### 配置

`desktop/windows/src/app_config.h` + `.cc`：

- 新增字段：
  ```cpp
  bool tap_to_arrow = false;          // 总开关，默认关
  // 二期可扩展：std::string tap_keycode = "down";
  ```
- 四处改动（`app_config.cc`）：INI 解析、TOML 表解析、序列化、默认值。
- `settings_dialog.cc` + `.h`：加复选框控件"敲击设备切换选项（双击→↓）"，仿 `imu_wake_sensitivity_combo_`（行 463-469）模式。

### UIPI 权限

memory 记录微信 4.0 等应用为 High IL，UIPI 静默丢弃 SendInput。本项目已通过 `requireAdministrator` 清单 + 任务计划程序自启解决（见 `windows-uipi-elevated-injection.md`）。`VK_DOWN` 注入走同一 `SendKey` 路径，无需额外权限处理。

## 测试策略

### Windows 单元测试（TDD，先写）

`desktop/windows/tests/core_tests.cc`，用现有 FakeInputInjector 模式新增：

1. **`TestTapEventInjectsArrowDown`** 🔴
   - 构造 `StateEvent{event="tap", kind="double"}`，`tap_to_arrow=true`。
   - 喂给 coordinator `HandleStateEvent`。
   - 断言 `FakeInputInjector::SendArrowDown` 被调用一次。

2. **`TestTapDisabledWhenConfigOff`** 🔴
   - `tap_to_arrow=false`，同样事件。
   - 断言 `SendArrowDown` 未被调用。

3. **`TestTapIgnoredDuringRecording`** 🔴
   - 模拟录音活跃态（`active_session_id_` 非空）。
   - 收到 tap 事件。
   - 断言不注入、不取消录音（与固件门控一致，桌面端兜底）。

4. **`TestTapDuringConfirmCountdownInjects`** 🔴
   - 确认倒计时态收到 tap。
   - 断言 `SendArrowDown` 被调用，且不取消待粘贴文本。

FakeInputInjector 需新增 `SendArrowDown` 的计数记录（与现有 `paste_count`/`enter_count` 同模式）。

### 固件测试

无自动化测试。靠真机验证：
- 用 `sticks3-flash-ota` skill 烧录后，手持设备双击外壳，观察是否触发。
- 借助 `show_imu_debug` 开关（`main.c:80,702-705`）在屏幕显示 tap 命中计数与 INT_STATUS 原始值，辅助调阈值。
- 验证阈值三档（low/medium/high）的误触发率与漏检率。
- 验证录音中 tap 被门控掉、不产生 BLE 上报。

## 风险与应对

| 风险 | 应对 |
|---|---|
| BMI270 double-tap 硬件 feature 寄存器数值 | 已通过 Bosch 官方源码核实（page 5/6、word 对齐字段、INT_STATUS_0 bit6/0x40），见"实施前置依赖"。软件算法路径不使用这些寄存器，仅作二期备查 |
| **config file variant 不匹配** | 仓库现用 non-legacy config file，硬件 tap feature 不可用。**软件算法路径不动 config file**，规避此阻塞。若二期改硬件 engine 再处理 |
| 手持挥动误触发 tap | 陀螺仪角速度幅值作"非挥动"确认（`GYR_CALM_THR`），这是软件算法相对纯 ACC 的核心优势 |
| 软件算法参数难调 | 初始值给保守档（高阈值），借助 `show_imu_debug` 屏显 GYR 幅值与状态机态真机调参；三档灵敏度逐步放开 |
| 10ms 轮询增加 I2C 负载 | 单次 12 字节@400kHz≈0.3ms，10ms 周期占用约 3%；录音/识别态暂停轮询进一步降负 |
| 启用陀螺仪增加功耗 | `tap_enabled=false` 时不读 GYR；关机态仍走原 any-motion 路径，不受影响 |
| tap 与录音按键行为冲突 | 固件按 `ui_state` 门控，录音/识别中暂停 tap 轮询且不上报 |
| MPU6886 兼容 | MPU6886 当前禁用 gyro（`PWR_MGMT_2`=0x07），需放开并补 `bmi270_read_gyr_dps` MPU6886 分支；MPU6886 量程/换算与 BMI270 不同 |

## 二期可选：换 legacy config file 上硬件 tap engine

若一期软件算法实测漏检/误检仍不理想，再考虑换 config file：
- 将 8KB config file 替换为 `bmi270_legacy_config_file`（已核实仓库当前与 `bmi270.c` 逐字节相同，legacy 版在官方 `bmi270_legacy.c`）。
- 同步改造 `configure_any_motion()`：any-motion start_addr 从 0x0C 改为 0x06（`bmi270_legacy.h:77`），中断掩码改用 `BMI270_LEGACY_INT_ANY_MOT_MASK`。
- 新增 `configure_double_tap()`：写 page 5（primary，word0/2/3/7）+ page 6（secondary，word0/2），见"实施前置依赖"字段布局。
- 轮询 `INT_STATUS_0` bit6(0x40) 检出 double-tap。
- **必须真机重新验证关机态拿起唤醒**（any-motion offset 变更）。
- 硬件 engine 优势：Bosch 预调优、低功耗、不占主控；劣势：只用 ACC 无陀螺仪确认、改动面大。

## 提交拆分

1. `feat(firmware): 启用 BMI270 陀螺仪并实现软件 double-tap 检测`
   - bmi270 组件：开 GYR、`bmi270_read_gyr_dps`、tap 状态机 `bmi270_tap_poll`、开关/灵敏度接口
   - main.c：10ms tap 轮询定时器 + 事件处理 + control_rx 字段 + NVS 持久化 + 调试屏显
   - voice_ble：`voice_ble_send_tap`
   - Doc/Ref/protocol.md：补 tap 事件说明
2. `feat(windows): tap 事件注入下方向键并加配置开关`
   - InputInjector 接口 + 实现
   - coordinator HandleTapEvent + 状态门控
   - 配置字段 + 设置对话框
   - core_tests.cc 四个测试用例
3. `docs(protocol): 补充 tap 事件帧格式说明`（可与 1 合并）

## 交互模型表更新

`CLAUDE.md` / `AGENTS.md` 核心交互模型表新增一行说明（待功能落地后同步）：

| 状态 | 敲击设备（双击外壳） |
|---|---|
| 连接空闲 | 注入下方向键（切换候选/选项） |
| 录音中/识别中 | 忽略 |
| 确认倒计时/手动确认中 | 注入下方向键 |
