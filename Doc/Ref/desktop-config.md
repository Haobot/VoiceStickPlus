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
- `tap_to_arrow`：IMU 敲击映射方向键开关。
- `encoder_to_arrow` / `encoder_rotation_invert` / `encoder_rotate_cw_key` / `encoder_rotate_ccw_key`：MiniEncoderC 编码器旋转注入开关（默认 `true`）、方向翻转（默认 `false`，true 时顺时针→Up）与 cw/ccw 自定义按键（热键语法）。
- `encoder_rotate_fast_threshold` / `encoder_rotate_cw_fast_key` / `encoder_rotate_ccw_fast_key`：旋转快慢分档——固件 10ms 窗口计数的单窗口格速（steps × 100 格/秒）量化到 100 格/秒，直接比较会让 100~200 间阈值失效且偶发 2 步窗误判快，故桌面端对单窗口格速做 EWMA 平滑（α=0.5 按事件更新，与墙钟无关，新手势静默 >250ms 后从零冷启动，见 `desktop/windows/src/encoder_speed.h`），平滑估计 ≥ 阈值（默认 200）判为快速手势，改注快速档按键（默认 cw=`pagedown` / ccw=`pageup`，慢速逐行、快速翻页）；一次快速手势只注入一次并进入停转锁定，锁定期间屏蔽所有旋转输出（含减速段慢速事件与换向事件），直到静默 >250ms 判定停稳才恢复识别；快速档按键非法时回退普通按键。设置对话框中阈值为滑杆控件（范围 100–300，超出范围的配置值显示时钳制）。
- `encoder_rotate_decide_window_ms`：慢速注入延迟判定窗（默认 80ms，0 = 关闭延迟判定即立即注入）。慢速事件先挂起累计，窗内判快则整段丢弃（消除快甩加速段的误注入），到期由 30ms 定时器驱动的 `EncoderRotateTick()` 冲刷补注；慢转因此有 ≤80ms 注入延迟，连续慢转按窗成批注入、总量不变。仅 config.toml 高级项，不进设置对话框。
- `encoder_led_color`：编码器录音灯颜色（red/green/blue/yellow/purple/cyan/white/off），BLE 下发固件 NVS 持久化。
- `encoder_press_action` / `encoder_press_key` / `encoder_double_click_action` / `encoder_double_click_key`：编码器单击/双击动作（recording|key）与自定义按键；`press_action=key` 派生固件录音门控关闭，双击 recording 走 remote_button 切换起停。

以上编码器设置项仅 Windows 端消费。

- `air_mouse_*`：体感鼠标参数（`air_mouse_sensitivity_x/y`、`air_mouse_tau`、`air_mouse_invert_y`、`air_mouse_curve_*`、`air_mouse_control_mode`、`air_mouse_rate_*` 等），完整字段见 `desktop/macos/Config/config.example.toml` 与 `desktop/windows/src/app_config.cc`。
