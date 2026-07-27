# MiniEncoderC 编码器接入设计

日期：2026-07-27
分支：`feat/mini-encoder-c`
状态：已批准（头脑风暴后确认）

## 1. 背景与目标

用户在 StickS3 上安装了 M5Stack MiniEncoderC Hat（SKU U157，I2C @0x42，HY2.0-4P 接口），要求：

1. 编码器按钮按下 = 与现有主键（primary）完全相同的录音触发行为，含双击注入 Enter、hold_to_talk / click_to_talk 两种模式。
2. 编码器旋转 = 输出上/下箭头按键操作，每格（30 格/圈）一次按键，顺/逆时针映射可配置切换。
3. 录音期间编码器上的 SK6812 LED 亮红灯，结束熄灭。

约束与前提：

- MiniEncoderC 与 StickS3 的 HAT 口结构不兼容（官方说明），用户自行接线，I2C 走 G9/G10。
- **Grove 口 5V 保持不启用**，固件不动 PMIC 的 BOOST_EN；编码器供电由用户接线负责。
- 编码器是 I2C 外设，**不能作为深睡唤醒源**；主键仍是唯一唤醒键。
- 桌面端本次只改 Windows；macOS 不改代码，仅协议文档同步。
- 旋转箭头仅空闲态生效（仿 `tap_to_arrow` 门控）：录音中、识别中、体感鼠标态下不注入。

## 2. 总体架构

```text
MiniEncoderC (I2C @0x42, G9/G10)
   │  10ms 轮询（esp_timer，由 main.c 持有）
   ▼
mini_encoder_c 组件：读按钮(0x20) / 读增量(0x10) / 写 LED(0x30)
   │                           │
   │ 按钮边沿                   │ 旋转增量
   ▼                           ▼
queue_primary_down/up_event   APP_EVENT_ENCODER_ROTATE
(APP_INPUT_SOURCE_ENCODER)         │
   │                           ▼
   │                  voice_ble_send_encoder_rotate()
   │                  {"event":"encoder_rotate",
   │                   "direction":"cw|ccw","steps":n}
   ▼                           │
现有主键状态机（零改动复用）      │ state_tx notify
双击/hold/click_to_talk          ▼
                        Windows HandleEncoderRotate
                        → SendArrowUp/Down × steps
```

设计要点：

- 按钮接入复用 `remote_button` 先例：编码器按钮事件直接调用现有的 `queue_primary_down_event()` / `queue_primary_up_event()`，双击检测、300ms hold 阈值、click_to_talk、体感/取消语义零改动复用。
- 旋转复用 tap→arrow 链路形态：固件只上报原始物理事实（方向 + 步数），语义映射（cw/ccw → Up/Down、invert）在桌面端完成。

## 3. 固件：新组件 `firmware/components/mini_encoder_c/`

### 3.1 对外接口

轮询式，与 `bmi270` 用法一致：timer 由 main.c 持有，组件内无线程、无回调。

```c
esp_err_t mini_encoder_c_init(void);                 // 建第二路 I2C 总线 + 探测 0x42
bool      mini_encoder_c_present(void);
esp_err_t mini_encoder_c_read_button(bool *pressed); // 寄存器 0x20，1 字节，0x01=按下
esp_err_t mini_encoder_c_read_delta(int32_t *delta); // 寄存器 0x10 增量（读后清零语义，真机验证；
                                                     // 若不行则读 0x00 计数做软件差分）
esp_err_t mini_encoder_c_set_led(uint8_t r, uint8_t g, uint8_t b); // SK6812，寄存器 0x30（真机验证）
```

### 3.2 I2C 总线与引脚

- 新开第二路 I2C 总线，不占内部 G47/G48 总线（ES8311/BMI270/M5PM1 共用）。
- `stick_s3_board` 新增 `stick_s3_board_i2c_port()`，暴露内部总线实际占用的 I2C 端口号（`init_i2c` 有 NUM_1→NUM_0 探测回退）；编码器组件使用另一个端口。
- `stick_s3_board.h` 新增引脚宏：`STICK_S3_PIN_GROVE_SDA 9`、`STICK_S3_PIN_GROVE_SCL 10`；探测 0x42 失败时自动交换线序重试一次，仍失败则标记 absent。
- I2C 速率 100 kHz，与内部总线一致。

### 3.3 错误处理

- 连续 10 次 I2C 读写失败 → 标记 absent、停止轮询，避免日志刷屏。
- LED 写失败静默忽略（不影响录音主链路）。
- 编码器 absent 时固件行为与当前版本完全一致（优雅降级）。

## 4. 固件：按钮接入（main.c）

- `app_input_source_t` 新增 `APP_INPUT_SOURCE_ENCODER`（仅用于日志区分，处理逻辑与 `APP_INPUT_SOURCE_PHYSICAL` 相同）。
- 新增 10ms `encoder_poll_timer_cb`（仿 `tap_poll_timer_cb`，main.c:1717 附近）：
  - 按钮状态变化（与上次轮询比较）→ `queue_primary_down_event(APP_INPUT_SOURCE_ENCODER, 0)` / `queue_primary_up_event(...)`。
  - 旋转增量非零 → `queue_app_event()` 新增事件 `APP_EVENT_ENCODER_ROTATE`，payload 携带方向（cw/ccw）与步数。
- 补丁：`double_click_timer_cb` 中两处 `gpio_get_level(STICK_S3_PIN_BUTTON_FRONT)`（约 main.c:1490、1531）与 `enter_power_off` 的按住检查（约 main.c:497-505）抽成帮助函数：
  ```c
  static bool primary_button_held(void);  // GPIO 前键 || 编码器按钮当前状态
  ```
- `APP_EVENT_ENCODER_ROTATE` 的发送门控仿 `APP_EVENT_TAP`（main.c:1335-1344）：仅 BLE 已连接、未录音、状态为 READY/PENDING_CONFIRMATION、体感鼠标未使能时才发送。
- 旋转事件合帧：每次轮询窗口内同向增量累计为一帧（`steps` 累计，发送即清零）；方向在窗口内反转的极端情况按最新方向处理。

## 5. 固件：LED

- `start_recording()` 成功路径 → `mini_encoder_c_set_led(255, 0, 0)`。
- 录音会话结束（drain 完成后的停止路径）→ `mini_encoder_c_set_led(0, 0, 0)`。
- 仅 `mini_encoder_c_present()` 为 true 时执行。

## 6. BLE 协议

state_tx 新增 JSON 事件：

```json
{"event":"encoder_rotate","direction":"cw","steps":2}
```

- `direction`：`"cw"` | `"ccw"`，原始物理方向，固件不做语义映射。
- `steps`：本次窗口内同向累计格数，≥1。
- `voice_ble.c` 仿 `voice_ble_send_tap`（约 1100 行）新增：
  ```c
  esp_err_t voice_ble_send_encoder_rotate(const char *direction, uint8_t steps);
  ```
- `voice_ble_send_device_info` 的 `buttons` 数组**不变**（编码器行为上就是 primary，不暴露新按键角色）。
- `Doc/Ref/protocol.md` 同步补充该事件定义（状态事件清单 + 语义说明）。

## 7. Windows 桌面端

### 7.1 事件消费

- `voice_stick_coordinator.cc` 的 `HandleStateEvent` 新增 `encoder_rotate` 分支 → `HandleEncoderRotate(direction, steps)`。
- 门控：`config_.encoder_to_arrow` 为 true 才注入。
- 方向映射：默认 `cw → VK_DOWN` / `ccw → VK_UP`；`encoder_rotation_invert=true` 时翻转。
- 每个 step 注入一次按键（`SendInput`，与现有 `SendArrowDown` 同路径）。

### 7.2 InputInjectorWin

- 补 `SendArrowUp()`（VK_UP），与现有 `SendArrowDown()`（input_injector_win.cc:65）对称。

### 7.3 配置

新增两个全局配置项（`app_config.h/.cc` 解析，默认值为未配置时的行为）：

| 配置项 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `encoder_to_arrow` | bool | `true` | 编码器旋转注入方向键开关 |
| `encoder_rotation_invert` | bool | `false` | 旋转方向翻转（true 时 cw→Up） |

同步更新示例配置：Windows `config.template.toml` 与 macOS `desktop/macos/Config/config.example.toml`（仅作字段示例，macOS 代码不消费）。

### 7.4 测试

`desktop/windows/tests/core_tests.cc` 新增用例并挂入 `main()`：

1. `encoder_rotate` 事件 JSON 解析（direction/steps 字段、缺字段容错）。
2. 方向映射与 `encoder_rotation_invert` 翻转。
3. `encoder_to_arrow` 门控（false 时不注入）。

验证命令：`ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests`。

## 8. 文档同步

- `Doc/Ref/protocol.md`：`encoder_rotate` 事件定义。
- `AGENTS.md` 与 `CLAUDE.md` 同步更新：
  - 板级硬件映射表加 MiniEncoderC 行（I2C @0x42，G9/G10，自行接线）。
  - 配置节加 `encoder_to_arrow` / `encoder_rotation_invert`。
  - 注意事项加两条：编码器键不能作为深睡唤醒源；Grove 5V 保持不启用、编码器供电由外部接线负责。
  - `firmware/components/` 组件清单从 5 个更新为 6 个。

## 9. 验证计划

- 固件：`idf.py build` 通过（ESP-IDF v5.5.1，esp32s3）。
- 真机回归（无编码器）：探测失败优雅降级，主键/侧键/敲击/体感行为与当前版本一致。
- 真机功能（有编码器）：按钮按住/双击/click_to_talk 与主键一致；旋转空闲态注入上下箭头、录音中不注入；录音亮红灯、结束熄灭；invert 配置生效。
- Windows：`voicestick_windows_tests` 全绿；真机半自动验证旋转注入方向与 invert。

## 10. 明确不做（YAGNI）

- 不启用 Grove 5V（不动 PMIC BOOST_EN）。
- 不做 macOS 端代码实现（仅示例配置与协议文档同步）。
- 不做 LED 多状态变色（仅录音红/灭）。
- 编码器不作为深睡唤醒源。
- `voice_ble_send_device_info` 不新增按键角色。
