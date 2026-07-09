# 屏幕亮度与亮屏时间续航优化方案

- 版本：v1.0
- 日期：2026-07-09
- 状态：已确认设计，待实现
- 相关文档：`Doc/Plan/固件待机省电策略.md`、`Doc/Ref/StickS3 低功耗配置.md`、`Doc/Plan/ble-hotkey-wake-sleep-plan.md`、`Doc/Ref/protocol.md`

## 1. 背景与目标

用户反馈屏幕亮度太大，希望降低亮度、缩短亮屏时间以提升续航，并结合陀螺仪（BMI270）和按钮操作制定方案。

当前固件已实现的省电基线（`firmware/main/main.c`、`firmware/components/ui_status/ui_status.c`）：

- 四级电源状态机：S0 亮屏 -> S1 暗屏 -> S2 熄屏 -> S3 深度睡眠
- 超时：30s 降亮度 / 60s 熄屏 / 10min 深睡（连接态）/ BLE 断连 10min 关机
- 亮度为编译期宏：S0 = 32/255（约 12.5%）、S1 = 8/255（约 3.1%）、S2/S3 = 0
- 深睡仅前键 GPIO11（ext1, ANY_LOW）唤醒；IMU any-motion 唤醒代码已完整（`bmi270_enable_pickup_wake`、`stick_s3_board_power_off`）但被 `main.c:437` 的 `#if 0` 禁用，未真机验证
- S1/S2 熄屏态的"软件拿起亮屏"已实现可用（见第 3 节）
- 亮度与超时均为编译期常量，桌面端无法下发，无运行时可调

目标：在低风险前提下降低亮度、缩短亮屏时间，并通过陀螺仪实现"拿起来亮屏"，按钮负责进入交互。

## 2. 设计决策（已与用户确认）

| 决策点 | 选择 | 说明 |
|---|---|---|
| 亮屏触发源 | 拿起来亮屏（IMU） | 陀螺仪负责"亮屏看状态"，按钮负责"进入交互" |
| 息屏节奏 | 适中：10s/20s/5min | S0->S1 10s、S1->S2 20s、S2->S3 5min |
| 深睡唤醒 | 分阶段 | 第一阶段先做低风险熄屏态软件拿起亮屏；第二阶段再验证深睡 IMU 唤醒 |
| 亮度档位 | 适度降：Active 20 / Dim 4 | 约 7.8% / 1.6%，明显变暗但保证可读 |

不做的事（YAGNI）：

- 不做桌面端亮度可调（用户未选，避免改协议+三端）
- 不做环境光自动亮度（无传感器）
- 不启用 light sleep（已被禁用，USB 稳定性，见 `固件待机省电策略.md`）
- 不改 IMU 拿起检测算法（已有基础，`bmi270_pickup_detected`）

## 3. 第一阶段：参数调整（核心交付）

### 3.1 参数变更

修改 `firmware/main/main.c:33-40` 的 5 个编译期宏：

| 宏 | 当前值 | 新值 | 含义 |
|---|---|---|---|
| `DISPLAY_ACTIVE_BRIGHTNESS` | 32 | 20 | S0 亮屏亮度，约 7.8% |
| `DISPLAY_DIM_BRIGHTNESS` | 8 | 4 | S1 暗屏亮度，约 1.6% |
| `DISPLAY_DIM_TIMEOUT_MS` | 30s | 10s | S0 -> S1 降亮超时 |
| `DISPLAY_OFF_TIMEOUT_MS` | 60s | 20s | S1 -> S2 熄屏超时 |
| `POWEROFF_TIMEOUT_MS` | 10min | 5min | S2 -> S3 深睡超时（连接态） |

保持不变：

- `DISC_POWEROFF_TIMEOUT_MS`（BLE 断连 10min 关机）：与连接态深睡语义不同，不调整
- `PICKUP_POLL_INTERVAL_MS`（500ms）：拿起响应延迟约 500ms，对"看一眼状态"足够

### 3.2 亮屏触发与状态机

```
S0(亮,20) --10s--> S1(暗,4) --20s--> S2(灭,0) --5min--> S3(深睡,前键唤醒)
   ^__________________________________|  拿起事件
   (S1/S2 软件拿起检测 -> note_activity -> 回 S0)
```

- S1/S2 拿起亮屏：已实现，无需补代码
  - 拿起轮询在 S0->S1 时启动（`display_dim_timer_cb`，main.c:1358）
  - S1->S2 的 `display_off_timer_cb`（main.c:1383-1396）只把背光设为 0，不停轮询
  - `APP_EVENT_PICKUP`（main.c:1272-1278）在 `s_display_dimmed` 为真时唤醒，S1/S2 态该标志都为真
  - 拿起后 `note_activity()` 停轮询、亮度回 S0、重置三级计时器
- 录音/OTA 中常亮：现有守卫 `!s_recording && !s_ota_updating`，不启动 dim 计时器，保持 S0
- 拿起误触治理：pickup 阈值默认 800 LSB，桌面端可经 `imu_wake_sensitivity`（threshold 50-2000 LSB）下发调节，避免桌面振动误亮屏

### 3.3 按钮角色

- 主键（GPIO11）：亮屏后按住录音（push-to-talk）；深睡态作为第一阶段唯一唤醒源（ext1, ANY_LOW）
- 侧键（GPIO12）：恢复上次输入确认 / 进退体感鼠标（现有逻辑）
- 分工：陀螺仪负责"亮屏看状态"（无需按键），按钮负责"进入交互"（录音等）。拿起亮屏后不操作则 10s 自动降亮，按键则进入录音常亮。两者不冲突。

## 4. 第二阶段：深睡 IMU 唤醒（独立后续验证项）

- 取消 `firmware/main/main.c:437` 的 `#if 0`，启用路径 A：M5PM1 真关机（`stick_s3_board_power_off`）+ BMI270 any-motion（50mg/20ms，INT1 active low -> PYG4 WAKE）
- 代码已完整就绪，仅未真机验证通：
  - `bmi270_enable_pickup_wake()`（`firmware/components/bmi270/bmi270.c:996-1035`）：加载 8KB config file + 开 ACC + 配 any-motion + INT1 输出
  - `stick_s3_board_power_off()`（`firmware/components/stick_s3_board/stick_s3_board.c:396-424`）：M5PM1 关机序列
- 硬件约束：BMI270 INT1 接 M5PM1 的 GPIO4(PYG4)，不接 ESP32-S3 任何 GPIO，深睡态只能经 M5PM1 唤醒整机（见 `Doc/Ref/StickS3 低功耗配置.md`）
- 真机验证：关机后拿起能否唤醒整机
- 回退策略：不通则保留第一阶段（深睡仍按键唤醒），不阻塞第一阶段交付

## 5. 风险与验证

| 风险 | 验证方式 |
|---|---|
| 亮度 20/4 真机可读性 | 真机看效果，必要时微调（改宏即可） |
| PWM 低 duty 频闪 | RC_FAST 时钟已减闪烁（ui_status.c:291），20/255 应可接受；不行则回调 |
| 拿起误触/漏检 | 真机放置稳定性测试，调 `imu_wake_sensitivity` 阈值 |
| 深睡 IMU 唤醒链路 | 第二阶段真机验证，不通有回退 |

固件无自动化单测，验证方式 = `idf.py build` 编译通过 + 真机运行时测试：

- 放下后观察 10s 降亮 / 20s 熄屏 / 5min 深睡节奏
- S2 熄屏态拿起能亮屏
- 录音中保持常亮
- 深睡态前键唤醒正常（第一阶段）

## 6. 涉及文件

第一阶段：

- `firmware/main/main.c`：5 个宏（行 33-40）
- `Doc/Ref/protocol.md:287-288`：修正过时的"5 minutes"深睡描述为实际值（10min -> 5min，与本次改动后一致）
- 本设计文档

第二阶段（独立后续）：

- `firmware/main/main.c:437`：取消 `#if 0`（代码已就绪）

## 7. 与现有方案的关系

- 继承 `Doc/Plan/固件待机省电策略.md` 的四级状态机与超时框架，仅调整参数值
- 不影响 `Doc/Plan/imu-tap-detection.md`（敲击检测）、`Doc/Plan/imu-air-mouse.md`（体感鼠标）、`Doc/Plan/tap-false-trigger-mitigation.md`（误触治理）：这些 IMU 功能在线态运行，与省电参数调整正交
- 第二阶段唤醒启用后，与 `ble-hotkey-wake-sleep-plan.md` 的"连接态可关机"改造一致：连接态 S2->S3 可关机，深睡态前键/IMU 唤醒
