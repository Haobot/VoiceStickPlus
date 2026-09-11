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
- `llm_base_url` / `llm_api_key` / `llm_model`：OpenAI 兼容 LLM，用于翻译与精修；`refine_enabled` 默认 `false`（v2.3.2 起需用户手动开启）。
- `llm_disable_thinking`（默认 `true`）：向所有 LLM 请求（精修/翻译/热词提取）注入 `enable_thinking:false` 与 `chat_template_kwargs.enable_thinking:false`，关闭推理型模型（DeepSeek-R1、Qwen3 混合思考等）的深度思考以加快输出；对接严格校验未知字段、报 400 的 OpenAI 官方 API 时可设 `false` 关闭该组参数。
- `hotword_process_enabled` / `hotword_process_prompt`：热词处理（Windows），划词加词时用 LLM 提炼热词，复用 `llm_*` 连接配置；默认关闭。
- `hotword_mining_enabled`：热词候选挖掘（Windows，默认关闭）。两条挖掘通道共用计数存储：①精修 diff 挖掘（精修纠回不在表标识符时计数，无开关）；②LLM 主动提炼（本开关打开时，每会话完成后异步让 LLM 从最终文本提炼候选）。同一词达 3 次（`kHotwordCandidateThreshold`）弹托盘通知并在设置-热词区给出「加入/忽略」候选；计数存 `%APPDATA%\VoiceStick\hotword_candidates.json`。明确不做全自动入表，原因见 `Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md`。
- `asr_hotwords` 发送策略（Windows）：火山 corpus 直传前按「频率 × 新近度 × 手动加权」评分排序并装入 80 tokens 预算（`kHotwordCorpusTokenBudget`），超预算时优先保留高频/新近/手动词，其余本次会话不参与识别（每次运行提示一次，明细见日志）；使用统计（命中次数 + 最近使用时间，从最终文本大小写不敏感匹配，不记录文本本身）存 `%APPDATA%\VoiceStick\hotword_usage.json`。评分模型与 `scripts/e2e_test/asr_bench/hotword_select.py` 一致，设计见 `Doc/Plan/hotword-eval-and-prioritization.md` §3；实现 `desktop/windows/src/hotword_selector.cc`。精修/翻译 prompt 的热词段取评分 top-50（`kHotwordPromptMaxWords`），防大库稀释小模型注意力。
- `interaction_mode`：`hold_to_talk`（默认）或 `click_to_talk`，控制 focused_app/字幕模式的触发方式（托盘菜单可切）。wechat 模式的触发方式由 `[wechat_input_method].trigger_mode` 独立控制，不联动全局 `interaction_mode`。
- `paired_device_ids`：已配对设备 4 位十六进制 ID 列表，如 `C3D8,09AF`。
- `[output].target`：`focused_app`（默认）、`subtitle` 或 `wechat_input_method`；`[output].transform`：`original` 或 `translate`；可用 `[device.<id>.output]` 按设备覆盖。
- `[wechat_input_method]`：微信输入法模式专属配置，含 `trigger_mode`（wechat 专属触发方式，`hold_to_talk` 默认或 `click_to_talk`，与全局 `interaction_mode` 解耦）、`session_model`（输入法会话模型，`hold_to_talk` 默认或 `click_to_talk`，与 `trigger_mode` 正交：trigger 描述用户怎么按设备键，session model 描述怎么驱动输入法——hold=注入按住热键 SendDown/SendUp 对，click=注入单次 SendClick；未显式配置时跟随 `trigger_mode`，保存时按推导值固化写盘。组合 `trigger_mode="click_to_talk"` + `session_model="hold_to_talk"` 为「点按折叠」方案：设备键点按即折叠为 button_click、立即注入按住热键（**限时 2.5s 自动松开**）；小米遥控器下会话音频由 WeType **直接采集默认录音设备（真实麦克风）**——本端不启动采集/虚拟麦渲染/设备切换（2026-09-11 修订，此前「本机麦直供虚拟麦」管道在 keyup 后拆除会卡死 WeType）。停止击 = SendUp + **注入一次鼠标左键（光标位置）**触发 WeType 面板 detach 关闭——WeType 语音热键只能启动/重启会话，keyup/ESC 无收尾作用，文字上屏由 WeType 自行完成（停说后 ~150ms 自动 commit，与停止击解耦；2026-09-12 定案，见 RFC 修订 2 节）；StickS3 同组合仍走 BLE 音频经虚拟麦克风管道。背景与设计见 `Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md`）、`hotkey_hold` / `hotkey_click`（长按式/点按式各自记忆的触发热键，默认 `ctrl+win` / `ralt`）、`virtual_mic_playback_name` / `virtual_mic_capture_name`（虚拟麦克风播放/采集端设备名，通常对应 VB-CABLE 两端，仅 BLE 音频路径使用）、`auto_switch_default_recording_device`（录音期自动把系统默认录音设备切到虚拟麦克风采集端，松开切回；仅 BLE 音频经虚拟麦的路径生效，小米 click/hold 直连组合不生效）。
- `[local_asr]`（仅 Windows，本机麦克风模式，Doc/Plan/local-mic-mode.md；设置入口合并方案见 Doc/Plan/asr-settings-local-provider-merge.md；流式 partial 设计见 Doc/Plan/local-asr-streaming-partial.md）：`enabled`（默认 false，开启后安装按住说话 LL 热键）、`models_dir`（SenseVoice 模型目录，须含 model.int8.onnx + tokens.txt；空 = exe 同级 models/，相对路径以 exe 目录为基准）、`push_to_talk_key`（按住说话单键，默认 `right ctrl`；支持 right/left ctrl|shift|alt、f1-f24、capslock 等见 push_to_talk_key.h）。按住说话→本机麦克风采集→SenseVoice 本地识别→注入，无 BLE 设备可用；录音期间按 600ms 节流周期对全量音频滚动重解码持续产出 partial（与云端同一条 `on_partial`→悬浮窗/字幕/设备屏显示路径），松键无新增音频时直接复用上次结果出 final；**`enabled=true` 时设备会话（遥控器语音键/全局热键触发的设备录音）同样路由本地 SenseVoice，断网可用**——云端热词/实时分段自然降级，`asr_provider` 保留云端值供切回时无缝恢复。设置入口：设置「语音识别 → 服务提供方」下拉框末位「本地语音识别（离线，无需云端密钥）」选中即 `enabled=true`，选中后才显示「模型目录」行；按住说话热键在托盘菜单「热键」子菜单「按住说话热键...」中设置。保存即热更生效：模型目录变化重建运行件，热键变化即时重装钩子，无需重启；手改 config.toml 仍需重启应用。
- `[local_asr]` 本地文本精修（Doc/Plan/local-text-refinement.md，仅 Windows，随 llama.cpp 静态链入，断网可用）：`refine_enabled`（默认 true；本地识别会话的 final 文本过「L1 规则 → L2 本地 LLM（Qwen3-1.7B-Q4_K_M）→ L3 守卫」三层精修，逐层回退最差不劣于规则级；缺模型自动退化直通不阻塞输入）、`refine_model`（GGUF 模型路径，空 = 按档位探测默认模型（见 `refine_cross_turn`），相对路径锚 models_dir；env `VOICESTICK_REFINE_MODEL` 最高优先供测试）、`refine_num_threads`（CPU 推理线程数，默认 6，范围 1..32）、`refine_prompt`（本地精修 system prompt 含 few-shot 示例，多行文本；空 = 内置默认，可在设置 → 本地语音识别区块「本地精修提示词」多行框查看与编辑，保存时与默认等值自动归空=清空即恢复默认；改动经幂等键触发引擎重建重算 KV 前缀，注意 few-shot 格式须保持「输入：/输出：」示例对——0.6B/1.7B 对指令式 prompt 无法跟随，few-shot 是硬要求）、`refine_cross_turn`（跨轮上下文纠错，默认 false，设置界面复选框「跨轮上下文纠错（Qwen3-4B）」；开启后本地精修携带最近 5 轮历史（2 分钟 TTL）走纠正指令管线——模型只输出「错词→纠正词/待删片段」指令行，执行在拼音守卫代码层（等长+上文出现+逐字近音+幅度回退），空历史首轮也走该管线保证形态与 KV 前缀一致；模型档位优先 `Qwen3-4B-Q4_K_M`（M0 spike GO 口径，常驻内存 +1.4GB，Release 实测 2.5~5s/句随轮数增长），缺失回退 1.7B（纠正能力降级为安全弱档，守卫保底不变）；设计见 Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md）。与云端 `refine_enabled` 互斥：本地识别会话（含 enabled=true 时的设备会话）优先走本地精修。保存热更：模型路径/线程数/提示词（及跨轮开关引起的档位切换）变化重建引擎（1.7B 约 1.1GB 加载约 1s；4B 约 2.5GB）。
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

### 小米遥控器配置（仅 Windows 消费）

小米蓝牙遥控器 2 Pro（ATVV 协议）相关配置。设备 ID 形如 `RC-XXXX`（4 位大写 hex，与 StickS3 的 `VS-XXXX` 双前缀并存；内部存储与 `[device.<id>.*]` 表键均用去前缀的 4 位 hex，如 `[device.3A7F.xiaomi]`），ID 由 BLE 地址低 16 位分配，名称白名单/`RC-` 前缀识别见 `BleProtocol::DeviceClassFromName`。

- `xiaomi_suppress_f5`（bool，默认 `true`）：全局 F5 抑制开关。遥控器语音键按下时 Windows 会收到附带的 F5 按键（HID 副作用）；开启后桌面端用低级键盘钩子（`WH_KEYBOARD_LL`）在「80ms 窗内有 ATVV MIC_OPEN」时吞掉该 F5。
- **按设备覆盖**：`[device.<id>.xiaomi]` 表，结构镜像 `[device.<id>.output]`。未写的字段回落 `default_xiaomi_settings` 结构默认值；仅写入与默认不同的覆盖（相等则不落盘）。字段：
  - `gain_db`（double，默认 `12.0`）：ADPCM 解码后增益（dB），±24 dB 限幅在消费侧（`PcmPostprocessor`）完成。
  - `double_click_ms`（int >0，默认 `350`）：语音键双击判定窗（毫秒），镜像固件 `DOUBLE_CLICK_WINDOW_MS` 语义；非正值忽略并保留默认值。「遥控器设置…」UI 对话框将其限制在 200~600ms（越界 clamp）；手写配置 >0 均接受。

  示例：

  ```toml
  [device.3A7F.xiaomi]
  gain_db = 18.0
  double_click_ms = 400
  ```

- **按键映射**（Windows，2026-09 起）：`[xiaomi.keys]` 全局默认表 + `[device.<id>.xiaomi.keys]` 按设备覆盖表，键为 12 个可映射按钮 ID（`power`/`up`/`left`/`ok`/`right`/`down`/`back`/`volume_up`/`home`/`volume_down`/`menu`/`tv`；语音键 `mic` 不参与映射），值为 key_spec 快捷键语法（同热键语法，如 `backspace`、`ctrl+shift+v`，见 `key_spec.h`）。空串显式取消该键映射。设备覆盖与全局默认相同的条目不落盘。消费端为 Windows 端 LL 钩子拦截 + Raw Input VID/PID 佐证 + SendInput 注入（设计见 `Doc/Plan/xiaomi-keymap-consumer.md`），未映射的键保持系统原生行为。托盘「按键映射…」对话框编辑（仅对已配对小米遥控器显示）。同型号多台遥控器无法按设备区分时，映射取活跃设备的有效值。

  示例：

  ```toml
  [xiaomi.keys]
  back = "backspace"        # 返回键 → 退格删除

  [device.3A7F.xiaomi.keys]
  home = "win+d"            # 该设备主页键 → 显示桌面
  menu = ""                 # 该设备菜单键显式取消映射
  ```
