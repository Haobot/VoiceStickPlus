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
- `hotword_categories`（Windows，热词分区管理）：热词库按分类分区存储的事实源，JSON 数组字符串，如 `[{"name":"","words":"词1,词2"},{"name":"语文","words":"杜甫"}]`；`name` 为空字符串表示"默认"分类（UI 按界面语言本地化显示），`words` 复用 `asr_hotwords` 的逗号/换行切分语法。每分类热词上限 25 个（`HotwordCategory::kMaxWords`）。首次使用（无分区数据）预置 默认/语文/数学/程序/AI 五个分类，旧配置的 `asr_hotwords` 自动迁入默认分类。
- `active_hotword_category`（Windows）：当前活跃分类名（空字符串 = 默认分类）。**ASR 识别仅使用活跃分类的热词**（切分类即切换识别场景）；`asr_hotwords` 始终等于活跃分类的词表（投影），下游 ASR/精修/统计零改动，老版本 exe 读新配置也只认 `asr_hotwords`，可无损回滚。设置 → 热词区顶部的分类下拉可切换/新增（≤30 字符，不可重名）/删除（至少保留一个）分类；划词添加、LLM 提炼、纠错保存的热词一律进入当前活跃分类。
- `smart_hotword_enabled`（Windows，默认 `true`，设置 → 热词区「智能热词管理」开关）：智能热词管理，在手动分区之上叠加三层自动化——① **上下文识别自动切换**：对 ASR 最终文本做关键词加权评分（内置 程序/AI/数学/语文 四分类词表，`hotword_context.cc`），得分 ≥2.5、命中 ≥3 词、连续 2 次决策一致且距上次切换 ≥300s 才切换活跃分类（当前分类热词命中时拒绝切换，切换只改热词分类、不触碰文本纠错/注入路径）；② **容量动态管理**：活跃分类默认软限 5 词（超出进入"监控中"状态，仅日志与可视化标注），硬上限仍 25 词，加词满员时自动逐出活跃度最低词腾位；③ **活跃度评估与回收**：活跃度复用 `hotword_usage.json` 统计（`HotwordScore`：次数 × 30 天时间衰减 + manual 加成），被逐出的词连同统计进回收池 `%APPDATA%\VoiceStick\hotword_evicted.json`，池内词再次出现在最终文本即视为活跃度回升、自动回归当前活跃分类（池内记录的原分类仅供展示，不作回归目标——词可能被加进与语义无关的分类；活跃分类已满时先逐出更冷的词）。设置 → 热词区「活跃度…」按钮打开可视化对话框（各分类词表/使用次数/活跃度/闲置天数 + 回收池 + 监控状态；回收池行首 ✗ 图标点击即删除该词，「清空回收池」按钮一键删除全部池内词、清空前有确认，删除即时落地到 hotword_evicted.json）。关闭后恢复纯手动分区管理。
- `interaction_mode`：`hold_to_talk`（默认）或 `click_to_talk`，控制 focused_app/字幕模式的触发方式（托盘菜单可切）。wechat 模式的触发方式由 `[wechat_input_method].trigger_mode` 独立控制，不联动全局 `interaction_mode`。
- `paired_device_ids`：已配对设备 4 位十六进制 ID 列表，如 `C3D8,09AF`。
- `[output].target`：`focused_app`（默认）、`subtitle`、`wechat_input_method` 或 `virtual_mic`；`[output].transform`：`original` 或 `translate`；可用 `[device.<id>.output]` 按设备覆盖（`transform`/`translation_target` 维度，以及 `target` 维度=麦克风模式设备级开关，见下）。
- `[device.<id>.output].target`（麦克风模式，仅 Windows）：设备级输出目标覆盖，写 `wechat_input_method` 或 `virtual_mic` 即把该设备切为麦克风模式——录音不再走 ASR，而是解码 PCM 渲染到 `[wechat_input_method].virtual_mic_playback_name` 指定的虚拟麦克风播放端（系统应用从对应虚拟麦克风采集端即可录到该设备音频）。两者区别：`wechat_input_method` 额外触发第三方输入法语音热键（并可在录音期自动切默认录音设备）；`virtual_mic` 为直通模式，不触发热键、不切默认设备，由应用自行从虚拟麦克风采集端（如 CABLE Output）取音识别。缺失该键时设备跟随全局 `[output].target`（旧配置行为不变）。托盘设备子菜单「麦克风模式」复选框即此开关：勾选写入 `virtual_mic` 覆盖、取消回落全局默认（全局默认同为麦克风模式时显式回 `focused_app`，保证可关闭）；仅写入与全局默认不同的覆盖（相等则不落盘）。
- `[wechat_input_method]`：微信输入法模式专属配置，含 `trigger_mode`（wechat/virtual_mic 共用的触发方式，`hold_to_talk` 默认或 `click_to_talk`，与全局 `interaction_mode` 解耦）、`hotkey_hold` / `hotkey_click`（长按式/点按式各自记忆的触发热键，默认 `ctrl+win` / `ralt`，仅 `wechat_input_method` 模式发送）、`virtual_mic_playback_name` / `virtual_mic_capture_name`（虚拟麦克风播放/采集端设备名，通常对应 VB-CABLE 两端）、`auto_switch_default_recording_device`（录音期自动把系统默认录音设备切到虚拟麦克风采集端，松开切回；仅 `wechat_input_method` 模式生效，`virtual_mic` 直通模式不切）。
- 虚拟麦克风改名（设置 → 输出目标=第三方输入法/虚拟麦克风时出现「虚拟麦克风：显示为设备名」按钮）：把系统录音端虚拟麦克风（CABLE Output）的设备友好名改为第一台 StickS3 配对设备名（如 `VS-091C`，无配对设备时为 `VoiceStick`），方便在应用（Clash/浏览器等）的麦克风列表中直接找到。需管理员权限：主进程 `runas` 提权启动自身 `--rename-virtual-mic <endpoint-id> <name>` 子进程执行（main.cc 分支），写入走 IPolicyConfig::SetPropertyValue，失败回退写 HKLM MMDevices 注册表 + 重启音频服务；改名成功后 `virtual_mic_capture_name` 自动同步为新名（auto_switch 按名找设备）。VB-CABLE 重装会恢复默认名，重新执行改名即可。实现 `desktop/windows/src/virtual_mic_renamer.{h,cc}`。
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
