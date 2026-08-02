# 桌面端配置参考（config.toml）

本文承载桌面端运行时配置的完整字段说明，2026-08-02 由 `AGENTS.md`/`CLAUDE.md` 的「配置」章节迁入；根指南只保留摘要与指向本文的指针。

桌面端运行时配置文件位置：

- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows：`%APPDATA%\VoiceStick\config.toml`

Windows MSI 还会把 `config.template.toml` 装到 `%ProgramFiles%\VoiceStick\` 下，首启复制到 `%APPDATA%`（升级不覆盖）。示例见 `desktop/macos/Config/config.example.toml`。

## 字段明细

- `asr_provider`：ASR 提供商，可选 `volcengine`、`voicestick_cloud` 或 `tencent`（腾讯为 v1.8.2 新增）。
- `volcengine_api_key` / `voicestick_api_key` / `voicestick_cloud_url`：火山直连密钥，或 VoiceStick Cloud 中转密钥与 WebSocket URL。
- `volcengine_boosting_table_id` / `volcengine_correct_table_id`：火山自学习平台热词表/替换词表 ID（控制台创建），作为 `corpus.boosting_table_id` / `corpus.correct_table_id` 发送；背景见 `Doc/Ref/volcengine-asr.md`（corpus 热词直传只在流式第一遍生效，二遍最终文本不吃直传，精修 prompt 会附加热词表由 LLM 兜底纠正）。
- `tencent_secret_id` / `tencent_secret_key` / `tencent_appid`：腾讯云 ASR 凭据（加载时自动 Trim 去前后空格）。
- `llm_base_url` / `llm_api_key` / `llm_model`：OpenAI 兼容 LLM，用于翻译与精修；`refine_enabled` 默认 `true`。
- `hotword_process_enabled` / `hotword_process_prompt`：热词处理（Windows），划词加词时用 LLM 提炼热词，复用 `llm_*` 连接配置；默认关闭。
- `hotword_mining_enabled`：热词候选挖掘（Windows，默认关闭）。两条挖掘通道共用计数存储：①精修 diff 挖掘（精修纠回不在表标识符时计数，无开关）；②LLM 主动提炼（本开关打开时，每会话完成后异步让 LLM 从最终文本提炼候选）。同一词达 3 次（`kHotwordCandidateThreshold`）弹托盘通知并在设置-热词区给出「加入/忽略」候选；计数存 `%APPDATA%\VoiceStick\hotword_candidates.json`。明确不做全自动入表，原因见 `Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md`。
- `interaction_mode`：`hold_to_talk`（默认）或 `click_to_talk`，控制 focused_app/字幕模式的触发方式（托盘菜单可切）。wechat 模式的触发方式由 `[wechat_input_method].trigger_mode` 独立控制，不联动全局 `interaction_mode`。
- `paired_device_ids`：已配对设备 4 位十六进制 ID 列表，如 `C3D8,09AF`。
- `[output].target`：`focused_app`（默认）、`subtitle` 或 `wechat_input_method`；`[output].transform`：`original` 或 `translate`；可用 `[device.<id>.output]` 按设备覆盖。
- `[wechat_input_method]`：微信输入法模式专属配置，含 `trigger_mode`（wechat 专属触发方式，`hold_to_talk` 默认或 `click_to_talk`，与全局 `interaction_mode` 解耦）、`hotkey_hold` / `hotkey_click`（长按式/点按式各自记忆的触发热键，默认 `ctrl+win` / `ralt`）、`virtual_mic_playback_name` / `virtual_mic_capture_name`（虚拟麦克风播放/采集端设备名，通常对应 VB-CABLE 两端）、`auto_switch_default_recording_device`（录音期自动把系统默认录音设备切到虚拟麦克风采集端，松开切回）。
- `tap_to_arrow`：IMU 敲击映射方向键开关（顶层键为全局默认；v1.8.x 旧配置仍可加载，见下方「设备交互配置」按设备覆盖）。

### 编码器配置（仅 Windows 消费）

MiniEncoderC 编码器配置为**全局默认 + 按设备覆盖**，结构镜像 `[device.<id>.output]`：

- **全局默认**：顶层 `encoder_*` 扁平键（v1.8.x 旧配置仍可加载）。字段与默认值：
  - `encoder_to_arrow`（true）：旋转是否注入按键；关闭后旋转行（方向翻转/cw/ccw 按键/快慢阈值/快速档按键）在设备级对话框中隐藏。
  - `encoder_rotation_invert`（false）：方向翻转，true 时顺时针→Up。
  - `encoder_rotate_cw_key` / `encoder_rotate_ccw_key`：顺时针/逆时针自定义按键（热键语法，如 `down`/`up`）。
  - `encoder_rotate_fast_threshold`（200）：旋转快慢分档阈值（格/秒）。固件 10ms 窗口计数的单窗口格速（steps × 100 格/秒）量化到 100 格/秒，直接比较会让 100~200 间阈值失效且偶发 2 步窗误判快，故桌面端对单窗口格速做 EWMA 平滑（α=0.5 按事件更新，与墙钟无关，新手势静默 >250ms 后从零冷启动，见 `desktop/windows/src/encoder_speed.h`），平滑估计 ≥ 阈值判为快速手势，改注快速档按键（默认 cw=`pagedown` / ccw=`pageup`，慢速逐行、快速翻页）；一次快速手势只注入一次并进入停转锁定，锁定期间屏蔽所有旋转输出（含减速段慢速事件与换向事件），直到静默 >250ms 判定停稳才恢复识别；快速档按键非法时回退普通按键。设备级对话框中阈值为滑杆（范围 100–300，超出范围的配置值显示时钳制）。
  - `encoder_rotate_decide_window_ms`（80）：慢速注入延迟判定窗（0 = 立即注入）。慢速事件先挂起累计，窗内判快则整段丢弃（消除快甩加速段误注入），到期由 30ms 定时器驱动的 `EncoderRotateTick()` 冲刷补注；慢转因此有 ≤80ms 注入延迟、连续慢转按窗成批注入、总量不变。仅 config.toml 高级项，不进对话框。
  - `encoder_rotate_cw_fast_key` / `encoder_rotate_ccw_fast_key`：快速档 cw/ccw 按键（默认 `pagedown`/`pageup`）。
  - `encoder_led_color`：编码器录音灯颜色（red/green/blue/yellow/purple/cyan/white/off），BLE 下发固件 NVS 持久化。
  - `encoder_press_action` / `encoder_press_key`：单击动作（recording|key，默认 `recording`）与自定义按键；`press_action=key` 派生固件录音门控关闭。
  - `encoder_double_click_action` / `encoder_double_click_key`：双击动作（key|recording，默认 `key`，按键 `enter`）与自定义按键；双击 `recording` 走 remote_button 切换起停。
- **按设备覆盖**：`[device.<id>.encoder]` 表，键名去 `encoder_` 前缀（如 `to_arrow`、`rotate_cw_key`、`led_color`、`press_action`、`double_click_key`），未写的字段回落全局默认。仅写入与全局默认不同的覆盖（相等则不落盘）。示例：

  ```toml
  [device.9BC1.encoder]
  led_color = "blue"
  press_action = "key"
  press_key = "ctrl+f"
  ```

- **UI 入口**：编码器设置已从「设置」对话框移除，改为从托盘设备子菜单的「编码器设置…」（Encoder settings...）打开**设备级对话框**，仅当设备 `encoder_present`（固件上报 MiniEncoderC 探测成功）时显示该菜单项。对话框「恢复默认」按钮等同于清除该设备覆盖、回落全局默认。

以上编码器设置项仅 Windows 端消费。

- `air_mouse_*`：体感鼠标参数（`air_mouse_sensitivity_x/y`、`air_mouse_tau`、`air_mouse_invert_y`、`air_mouse_curve_*`、`air_mouse_control_mode`、`air_mouse_rate_*` 等）。其中 `air_mouse_sensitivity_x/y`（左右/上下灵敏度档 1~10）为**设备级覆盖**字段，详见下方「设备交互配置」；其余进阶参数（`tau`/`invert_y`/`curve_*`/`control_mode`/`rate_*`/`neutral_deadzone`）仍为全局唯一值，完整字段见 `desktop/macos/Config/config.example.toml` 与 `desktop/windows/src/app_config.cc`。

### 设备交互配置（仅 Windows 消费）

设备交互设置（IMU 唤醒灵敏度、敲击映射方向键、敲击灵敏度、体感鼠标左右/上下灵敏度）为**全局默认 + 按设备覆盖**，结构镜像 `[device.<id>.output]` 与 `[device.<id>.encoder]`：

- **全局默认**：顶层扁平键 `imu_wake_sensitivity`（`low`/`medium`/`high`，默认 `low`）、`tap_to_arrow`（bool，默认 `false`）、`tap_sensitivity`（1~10，默认 5）、`air_mouse_sensitivity_x/y`（1~10，默认 5）。v1.8.x 旧配置（这些键写在顶层）仍可加载，回落为全局默认。
- **按设备覆盖**：`[device.<id>.interaction]` 表，键名与全局默认一致（`imu_wake_sensitivity`/`tap_to_arrow`/`tap_sensitivity`/`air_mouse_sensitivity_x`/`air_mouse_sensitivity_y`）。未写的字段回落全局默认；仅写入与全局默认不同的覆盖（相等则不落盘，等价于清除覆盖）。示例：

  ```toml
  [device.9BC1.interaction]
  imu_wake_sensitivity = "high"
  tap_to_arrow = true
  tap_sensitivity = 3
  air_mouse_sensitivity_x = 8
  air_mouse_sensitivity_y = 6
  ```

- **UI 入口**：设备交互设置已从「设置」对话框移除，改为从托盘设备子菜单的「设备交互设置…」（Device interaction settings...）打开**设备级对话框**（所有设备都显示该菜单项）。对话框「恢复默认」按钮等同于清除该设备覆盖、回落全局默认。连接时与配置更新时，协调器对所有已连接设备逐台单播其有效交互设置（无覆盖设备收到全局默认）。
- **体感鼠标热调参**：托盘「体感鼠标调参」打开非模态窗口，标题带设备 ID，按**当前激活设备**调参。其中的左右/上下灵敏度滑杆写入该设备的 `[device.<id>.interaction]` 覆盖；其余进阶参数（`tau`/`invert_y`/`curve_*`/`control_mode`/`rate_*`/`neutral_deadzone`）仍写全局 `config.toml`。
