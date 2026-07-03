# 体感鼠标（IMU Air Mouse）实施方案

## 1. 需求

在现有语音输入设备基础上，新增一个"体感鼠标"模式，方便在多个界面之间切换焦点、点击：

1. **进入/退出**：侧键（secondary）单击进入体感鼠标模式，再次单击退出。
2. **移动**：进入后，设备陀螺仪的角速度转换为鼠标相对位移（手腕上抬/下压→光标上下，左转/右转→光标左右）。
3. **点击**：体感模式下，主键（primary）单击映射为鼠标左键单击。

本期仅实现 **Windows** 桌面端（与项目"新功能先 Windows 落地"的节奏一致，macOS 后续跟进）。

## 2. 可行性结论

**技术可行。** 关键依据：

| 能力 | 现状 | 结论 |
|---|---|---|
| 陀螺仪数据源 | `bmi270_read_gyr_dps()` 已可读三轴角速度（dps） | 直接可用，pitch→dy、yaw→dx |
| 高频轮询骨架 | 固件已有 10ms `tap_poll` / 200ms `imu_poll` esp_timer | 复刻一个鼠标态 20ms（50Hz）轮询 |
| BLE 带宽 | 录音态用 7.5ms fast interval（~133Hz） | 鼠标态不录音、音频通道空闲，50Hz 上报无压力 |
| 状态通知管道 | `send_state_json` → state_tx 通知 | 新增二进制帧类型复用同一管道 |
| 鼠标注入 | `InputInjectorWin` 已封装 `SendInput` | 新增 `MoveMouse` / `ClickLeft` |
| 模式门控 | tap/录音已有 `set_tap_polling_enabled`、状态机门控 | 复刻互斥门控 |

**主要挑战与对策**：

- **陀螺仪零偏漂移**：静止时陀螺仪读数非零，直接积分会导致光标缓慢漂移。对策：进入模式时采样 ~200ms 静止基线做零偏校准，并对小于死区阈值的角速度归零。
- **高频数据帧过重**：JSON 帧每帧 40+ 字节且需 cJSON 序列化。对策：新增二进制 `motion` 帧（`type=0x11`），载荷仅 4 字节（int16 dx + int16 dy）。
- **模式互斥**：体感模式下必须禁用 tap 注入、避免误启录音。对策：桌面端状态机新增 `air_mouse` 态，进入时门控其它注入路径。

## 3. 架构分层（沿用现有边界）

固件只上报"陀螺仪运动事实"，桌面端持有"体感鼠标模式状态机"并决定光标位移与点击——与"交互状态机在桌面端，不在固件"的既有约定一致。

```text
BMI270 gyro (dps) → 固件 20ms 轮询 → 零偏校准+死区+映射 → int16 dx/dy
    → BLE motion 帧(0x11) → Windows → 累加/加速曲线 → SendInput(MOUSEEVENTF_MOVE)
主键单击(button_click) → 桌面端 air_mouse 态 → SendInput 左键 down/up
侧键单击(button_up secondary) → 桌面端切换 air_mouse 态 → control_rx 下发 air_mouse_enabled
```

## 4. BLE 协议变更

### 4.1 新增 state_tx 二进制帧：motion（设备 → 主机）

在现有 state_tx 通道（notify）上，除 `type=0x10`（JSON 状态）外新增 `type=0x11`（motion）。桌面端 ValueChanged 回调按第 2 字节 type 分流。

```text
struct MotionBleFrame {   // 小端
  uint8_t  version;   // 1
  uint8_t  type;      // 0x11 motion
  int16_t  dx;        // 水平角速度映射（右为正）
  int16_t  dy;        // 垂直角速度映射（下为正）
}
```

固定 6 字节。dx/dy 为固件侧已完成"零偏校准+死区+缩放"的整型位移量（非原始 dps），桌面端只做加速曲线与累加，避免固件/桌面各持一套映射常数导致漂移。

> 采用二进制帧而非复用 JSON `{"event":"motion",...}`：50Hz × 连续数十秒的 JSON 序列化/解析在两端都是浪费，二进制帧载荷从 ~40B 降到 6B 且零解析开销。

### 4.2 新增 control_rx JSON 事件：air_mouse_enabled（主机 → 设备）

```json
{"event":"air_mouse_enabled","enabled":true}
```

固件收到 `enabled:true` 时启动 motion 轮询定时器并做零偏校准，`false` 时停表并复位状态。这样"是否上报 motion"由桌面端状态机权威控制，与 tap_enabled 的模式一致。

### 4.3 文档同步

`Doc/Ref/protocol.md`：在 State Event 段补 motion 帧结构说明，在 Control Event 表补 `air_mouse_enabled` 行。

## 5. 固件改动（`firmware/`）

### 5.1 `components/bmi270/`

新增体感鼠标运动读取接口，把零偏校准、死区、映射封装在驱动内（与 tap 检测同级）：

```c
// 进入体感模式时调用：采样静止基线做陀螺仪零偏校准。
void bmi270_air_mouse_start(void);
// 20ms 轮询一次：读 gyr，减零偏、过死区、映射为整型位移。
// 返回 true 且写出 *dx/*dy 表示本次有有效位移；无位移（死区内）返回 false。
bool bmi270_air_mouse_poll(int16_t *dx, int16_t *dy);
```

- 零偏：`start` 时连续采样 ~10 次 gyr 取均值作为 `bias_x/bias_y`。
- 死区：`|dps - bias| < DEADZONE_DPS`（初值 ~2 dps）归零，消除静止漂移。
- 映射：`d = (dps - bias) * SCALE`，`SCALE` 使正常手腕转动产生合适光标速度（初值待真机标定，先给保守值），并对 int16 饱和裁剪。
- 轴向：设备竖握时，yaw（绕垂直轴）→dx，pitch（绕水平轴）→dy；轴选择与符号在真机确认（结合现有 `update_display_orientation` 的朝向约定）。

### 5.2 `components/voice_ble/`

- `voice_ble.h`：新增 `esp_err_t voice_ble_send_motion(int16_t dx, int16_t dy);`
- `voice_ble.c`：仿 `send_state_json` 新增 `send_motion_frame`，header type 用 `0x11`，直接 append 4 字节 dx/dy（小端）。复用同一 `s_state_attr_handle` 与门控条件（连接 + state 已订阅）。

### 5.3 `main/main.c`

- 新增 `s_air_mouse_enabled` 标志与 `s_air_mouse_poll_timer`（周期 `AIR_MOUSE_POLL_INTERVAL_US = 20ms`）。
- 新增 `set_air_mouse_enabled(bool)`：true → `bmi270_air_mouse_start()` + 启动定时器；false → 停表。
- `air_mouse_poll_timer_cb`：调 `bmi270_air_mouse_poll`，有位移则 `voice_ble_send_motion`。
- `handle_control` 解析 `air_mouse_enabled` 事件 → `set_air_mouse_enabled`。
- **互斥**：体感模式下若主键触发录音，应优先保证点击语义；录音态与体感态互斥由桌面端权威控制（桌面端进入体感态时不会因主键单击启动录音，见 6.3），固件侧无需额外硬门控，但需确保 motion 轮询与 tap 轮询不同时运行（进入体感态时桌面端会同时下发 `tap_enabled:false` 或固件在体感态内跳过 tap）。

## 6. Windows 桌面端改动（`desktop/windows/`）

### 6.1 `src/ble_protocol.{h,cc}`（voicestick_core）

- 新增 `struct MotionEvent { std::int16_t dx; std::int16_t dy; };`
- 新增 `static std::optional<MotionEvent> ParseMotionFrame(std::span<const std::uint8_t>);`（校验 version=1、type=0x11、长度=6）。
- 新增 `static ByteVector AirMouseEnabledPayload(bool enabled);`（输出 `{"event":"air_mouse_enabled","enabled":...}`）。

### 6.2 `src/ble_central_win.cc`

state_tx 的 ValueChanged 回调中，先看第 2 字节：`0x11` → `ParseMotionFrame` → 派发 `OnMotionEvent(device_id, dx, dy)`；否则走现有 `ParseStateEvent`。

`BleCentral` 接口新增 `virtual void SendAirMouseEnabled(bool enabled, const std::optional<std::string>& device_id) = 0;`

### 6.3 `src/voice_stick_coordinator.{h,cc}`（核心状态机）

- `InputInjector` 抽象新增：`virtual void MoveMouse(int dx, int dy) = 0;`、`virtual void ClickLeftButton() = 0;`（down+up），可选 `PressLeftButton/ReleaseLeftButton` 供后续拖拽。
- 协调器新增每设备 `air_mouse_active_` 标志（沿用现有按 device_id 组织的多设备结构）。
- **进入/退出**：`HandleSecondaryButtonClick`（侧键单击）中——air_mouse 关则进入（置位、`SendAirMouseEnabled(true)`），开则退出（清位、`SendAirMouseEnabled(false)`）；有活跃录音/识别/待粘贴时侧键单击仍走原取消语义。**侧键原「恢复上次输入确认」已迁移到侧键双击**：固件区分侧键单/双击（单击延迟到 500ms 双击窗口超时后发 `button_click secondary`，窗口内第二击发 `button_double_click secondary`），桌面端 `HandleButtonDoubleClick` 的 secondary 分支调 `RestoreLastInputConfirmation`。两手势彻底分离，无抢占。
- **移动**：新增 `OnMotionEvent(device_id, dx, dy)`——仅当该设备 `air_mouse_active_` 时，经加速曲线（`out = dx * gain`，可加非线性）后 `input_injector_->MoveMouse(out_x, out_y)`。
- **点击**：`HandleButtonClick` 中，若 `air_mouse_active_` 为 true，则 `input_injector_->ClickLeftButton()` 并 return，不走录音/字幕逻辑。
- **门控**：`air_mouse_active_` 为 true 时，`HandlePrimaryButtonDown`（录音启动）与 `HandleTapEvent` 均短路返回，避免冲突。

### 6.4 `src/input_injector_win.{h,cc}`

```cpp
void MoveMouse(int dx, int dy) override;      // SendInput INPUT_MOUSE + MOUSEEVENTF_MOVE（相对）
void ClickLeftButton() override;              // LEFTDOWN + LEFTUP
```

### 6.5 配置项（`src/app_config.{h,cc}` + `config.example.toml`）

- `air_mouse_gain`：光标灵敏度倍率（浮点，默认待标定，如 `1.0`），下发不影响固件，仅桌面端加速曲线用。
- （可选）`air_mouse_invert_y`：Y 轴反向布尔，适配用户习惯。

固件侧映射常数（SCALE/DEADZONE）先用编译期常量，标定稳定后再考虑是否需要下发调节。

## 7. 测试策略

遵循 TDD（`voicestick_core` 可测部分先写失败测试）：

### 7.1 Windows 单元测试（`tests/core_tests.cc`，目标 `voicestick_windows_tests`）

- `TestParseMotionFrame`：合法 6 字节帧解析出正确 dx/dy（含负值/小端）；version 错、type 错、长度不足均返回 nullopt。
- `TestAirMouseEnabledPayload`：输出 JSON 与约定一致（true/false）。
- `TestCoordinatorAirMouseToggle`：用 Fake BleCentral，模拟侧键单击 → 断言下发 `SendAirMouseEnabled(true)`；再次单击 → `false`。
- `TestCoordinatorAirMouseClick`：air_mouse 开时主键 `button_click` → Fake InputInjector 记录 `ClickLeftButton` 调用、且**不**启动录音/ASR。
- `TestCoordinatorMotionMove`：air_mouse 开时 `OnMotionEvent(dx,dy)` → Fake InputInjector 记录 `MoveMouse`；air_mouse 关时忽略。
- `TestCoordinatorAirMouseGatesRecording`：air_mouse 开时主键 down 不触发录音；tap 事件被忽略。

### 7.2 固件验证

无自动化单测。`idf.py build` 编译通过 + 真机验证：进入模式后转动手腕光标平滑移动、静止不漂移、主键单击左键生效、侧键退出。零偏/死区/SCALE 真机标定。

### 7.3 集成验证

真机 + VoiceStick.exe：侧键进入 → 转动移动光标 → 主键单击 → 侧键退出 → 语音/双击/tap 功能不受影响（回归）。

## 8. 分阶段实施顺序

1. **协议与文档**：定 motion 帧（0x11）与 air_mouse_enabled 事件，更新 `Doc/Ref/protocol.md`。
2. **Windows core（TDD）**：`ParseMotionFrame`、`AirMouseEnabledPayload`、协调器切换/点击/移动/门控 + 单测全绿。
3. **Windows 平台层**：`ble_central_win` 帧分流与下发、`input_injector_win` 鼠标注入、`ble_protocol` 收尾。
4. **固件**：bmi270 air_mouse 接口、voice_ble motion 帧、main.c 轮询与控制解析、`idf.py build`。
5. **真机联调标定**：SCALE/DEADZONE/gain/轴向符号，回归其它功能。
6. **收尾**：`VERSION`/`firmware/version.txt` 视是否发布决定，构建 + CTest + 打包提交（按项目 Windows 收尾约定）。

## 9. 风险与回退

- **轴向/符号不确定**：真机第一版可能上下/左右反，标定阶段解决；`air_mouse_invert_y` 兜底。
- **漂移**：若死区+零偏仍漂，考虑桌面端二次死区或固件周期性重校准（静止 N 帧重采基线）。
- **侧键语义冲突**：已解决——侧键单击=进/退体感，侧键双击=恢复上次输入，固件层区分单/双击（见 6.3）。代价：侧键单击响应延迟 500ms（与主键单击手感一致）。
- **回退**：所有改动以新增为主（新帧类型、新事件、新态标志），关闭 air_mouse 后行为与现状完全一致，风险可控。
