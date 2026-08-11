# CHANGELOG.md

## Unreleased

- 热词高频优先裁剪（Windows）：热词库超出单次会话直传预算（火山 80 tokens）时，按「频率 × 新近度 × 手动加权」评分优先保留（`hotword_selector`，与 `scripts/e2e_test/asr_bench/hotword_select.py` 同一评分模型），替代原按插入顺序贪心截断——新加的词排在列表尾部、旧逻辑最先被裁。使用统计（命中次数 + 最近使用时间，从最终文本大小写不敏感匹配，不记录文本）存 `%APPDATA%\VoiceStick\hotword_usage.json`；超预算时每次运行提示一次（浮窗/托盘，明细见日志）。精修/翻译 LLM prompt 的热词段改为评分 top-50（`kHotwordPromptMaxWords`），防大库稀释小模型注意力。新增 `TestHotwordSelector` 单测（镜像 hotword_select.py 自测断言）。

## v2.3.6

- 烧录进度解析兼容 esptool 5.x（Windows）：VoiceStickFlash 内嵌 esptool 5.2.0，管道非 TTY 时实际输出 `Writing at 0x00010000 [=====>                    ]  45.7% 1077248/2359296 bytes... ` 形式的进度行，`EsptoolProgressParser` 补充识别该格式（`(X %)` 形式继续兼容），新增对应单测；注释同步说明两种 esptool 版本的进度格式。
- 便携版包含固件烧录工具（Windows）：`scripts/package-portable.ps1` 新增收集 `VoiceStickFlash.exe` 与 `FlashTool\` 自包含 esptool 运行时（优先复用 `build-msi-x64\flash_payload`，否则现场调用 `prepare_flash_payload.ps1` 生成；布局与 `LocatePythonExe()` 候选 1 一致），使便携版同样具备 COM 口固件烧录能力，与 MSI 布局一致。

## v2.3.7

- 安装程序支持英文（Windows）：`desktop/windows/installer/` 新增 `en-US.wxl` 与 `license-en.rtf`（`zh-CN.wxl` 补 `FlashToolName`、`LicensePath`，license 拆分为 `license-zh-CN.rtf` / `license-en.rtf` 两版）；`VoiceStick.wxs` 的 license 与开始菜单「固件烧录工具」快捷方式名改为按 culture 本地化（`!(loc.LicensePath)` / `!(loc.FlashToolName)`）。WiX 4.0 一次构建只产一个 culture（多语言 MSI 支持尚未落地，`-culture` 为单值过滤），故 `build-msi.bat` / `build-msi-unsigned.bat` 改为循环产出 `VoiceStick_<版本>_zh-CN.msi`（语言码 2052）与 `VoiceStick_<版本>_en-US.msi`（语言码 1033）两个安装包，各自签名。`deploy-website.yml` 的 appcast 只收录 en-US 版（WinSparkle 0.9.2 不支持按语言选 enclosure，`sparkle:language` 未实现）；zh-CN 版仅供中文用户手动下载。已实测：两个 MSI 语言码、license、快捷方式名均按 culture 正确嵌入。
- 新增 COM 口固件烧录工具 VoiceStickFlash（Windows）：独立 Win32 GUI 小工具，随 MSI 安装（`INSTALLFOLDER\VoiceStickFlash.exe` + `FlashTool\` 自包含 python-embed + esptool 运行时，免系统 Python），作为 BLE OTA 之外的用户级兜底链路（救砖 / 分区表变更 / bootloader 更新 / 恢复出厂）。支持三种模式：整包烧录（merged bin @ 0x0）、仅应用分区（@ 0x10000）、先完全擦除再整包；COM 口自动枚举 + 评分选中（ESP32-S3 原生 USB VID 303A 优先，规则与 `scripts/idf_cli.yaml` 一致）；子进程方式跑 `python -m esptool`（`--after no_reset`，烧完提示手动短按电源键重启），stdout 解析阶段/进度/错误；开始前检测 VoiceStickApp 运行并警告。入口：托盘菜单「固件烧录工具…」+ 固件更新对话框「高级… COM 口烧录」按钮。可测试逻辑下沉 `voicestick_core`（`com_port_selector` / `esptool_flash_command` / `esptool_progress` / `voice_stick_flash_tool`，4 组新单测）；`scripts/prepare_flash_payload.ps1` 幂等准备 payload（`VOICESTICK_PYTHON_EMBED_URL` 可覆盖下载源），`build-msi.bat` / `build-msi-unsigned.bat` 自动调用并打进 MSI。设计见 `Doc/Plan/windows-com-flash-tool.md`。
- 修复编码器慢速旋转配置失效（Windows）：设备级编码器设置（`[device.<id>.encoder]` 的 `rotate_cw_key`/`rotate_ccw_key`）对慢速旋转无效，输出锁死全局默认 Up/Down，快速档正常。根因：`FlushEncoderRotatePending` 参数为 `const std::string&`，`EncoderRotateTick()` 传入的正是成员 `encoder_pending_device_id_` 本身，函数体 `clear()` 清空该成员使参数引用的对象变为空串，`EncoderSettingsForDevice("")` 查不到设备覆盖、回落全局默认（v2.3.0 引入 `encoder_pending_device_id_` 时遗留的引用别名问题）。修复：参数改按值传递消除引用别名；新增回归测试 `TestEncoderRotateCustomKeysPendingPathDeviceOverride`（设备覆盖 + pending → Tick 冲刷路径，此前测试全用全局默认键掩盖了回落路径）。

## v2.3.5

- MSI 打包密钥来源对齐本机配置（Windows）：`build-msi.bat` 的 `VOICESTICK_MSI_CONFIG_SOURCE` 默认值从 `dist/VoiceStick_Portable_v2.0.0/config.toml` 改为 `%APPDATA%\VoiceStick\config.toml`，与 `extract_builtin_key.ps1`（exe 内置凭据）同源，本机更新密钥后打包即生效；`build-msi-unsigned.bat` 补上相同的 `generate_msi_config.ps1` 生成步骤（此前未签名构建只支持 `VOICESTICK_CONFIG_TEMPLATE` 手动覆盖，默认打占位符模板）。

## v2.3.4

- MSI 安装时写入含测试密钥的 config.toml（Windows）：新增 `scripts/generate_msi_config.ps1` 从 `dist/VoiceStick_Portable_v2.0.0/config.toml` 提取火山/腾讯/LLM 七项密钥，注入 `config.template.toml` 生成含 key 的构建产物（gitignored，密钥不进仓库）；`build-msi.bat` Step 3 前默认调用（`VOICESTICK_MSI_CONFIG_SOURCE` 可覆盖来源，保留 `VOICESTICK_CONFIG_TEMPLATE` 手动回退）；`VoiceStick.wxs` 新增 `SeedMsiConfigExec` 安装时自定义动作，把 `INSTALLFOLDER\config.template.toml` 整份复制覆盖到 `%APPDATA%\VoiceStick\config.toml`（deferred + Impersonate + `NOT Installed` 条件，覆盖已有配置）。运行时 `Active*()` 本就优先读 config.toml，config 有 key 即读它，开箱即用不再提示缺 key。真机验证：安装后 `%APPDATA%` config 被覆盖为含 7 项测试 key，启动日志 `make_asr: provider=tencent appid=…` 确认读到 key，Onboarding 自动完成。

- 修复 BLE 配对失败（Windows）：固件广播名与 MAC 低位推断的设备 ID 不一致时（如广播名 D63C / MAC 低位 D63E），同一物理地址会同时产生命名候选与临时候选，后者可能覆盖前者，配对对话框点到临时候选命中 "waiting for name" 不发起连接，设备停在 pairing 屏。修复 `MergePairingCandidate`：同地址合并时命名候选优先，后到的临时包不覆盖；`PairSelectedDevice` 改用与列表显示一致的 `VisiblePairingCandidates` 过滤后候选取数，消除索引错位；新增直接连接与临时候选点击诊断日志；补回归测试。

## 2026-08-05 v2.3.3

- 修复首启 volcengine ASR 缺 resource_id 致需切换供应商才可用（Windows）：`config.template.toml` 的 `resource_id=""` 覆盖成员默认值 `volc.seedasr.sauc.duration`，内置 key 跳过 kAsr 的 onboarding 不填 `resource_id`，致 `AsrClientWin` 发空 `X-Api-Resource-Id`、volcengine ASR 失败；进设置切换一次供应商保存时 `resource_combo_` 默认选第一项填入有效值才修复。新增 `AppConfig::ActiveResourceId()`：`resource_id` 空时回退 `SupportedResourceIds().front()`（`volc.seedasr.sauc.duration`）；`asr_client_win.cc`（`X-Api-Resource-Id` 请求头）与 `asr_protocol.cc`（`MakeStartSessionFrame` JSON）改用 `ActiveResourceId()`；`config.template.toml` 填默认值 `volc.seedasr.sauc.duration`；加 `TestActiveResourceId` 单测。

## 2026-08-05 v2.3.2

- 内置腾讯云 ASR 与 DeepSeek LLM 凭据（Windows）：在 v2.3.1 内置火山 key 基础上扩展，`builtin_secrets.h.in` 新增腾讯云 `SecretId`/`SecretKey`/`AppId` 与 DeepSeek `llm_api_key`/`llm_base_url`/`llm_model` 六个编译期常量；`AppConfig` 新增 `ActiveTencentSecretId/SecretKey/Appid` 与 `ActiveLlmApiKey/BaseUrl/Model` 六个回退访问器，复用通用纯函数 `ResolveActiveString`（配置值优先，空则回退内置，不落盘）；`ResolveActiveApiKey` 签名扩展 `builtin_tencent_id` 参数，tencent 模式也回退内置 `secret_id`，onboarding 在 tencent 模式同样跳过 kAsr。`asr_client_tencent.cc`（Start 空检查 + BuildSignedUrl 签名）与 `llm_chat_client.cc`（api_key/model/base_url 读取点）改用 `Active*()` 访问器，确保内置回退在 ASR/LLM 调用链路生效。
- build-msi 注入全套内置凭据（Windows）：`extract_builtin_key.ps1` 改为输出 7 行 `VOICESTICK_BUILTIN_<NAME>=<value>`（火山 + 腾讯云 3 + DeepSeek 3）；`build-msi.bat` 用 `for /f` 循环 set 每行为环境变量，cmake configure 带 7 个 `-D`，使 MSI 内置全套凭据，新用户首启 tencent/DeepSeek 模式开箱即用。
- 精修默认关闭（Windows）：`AppConfig::refine_enabled` 默认 `true`->`false`，`resources/config.template.toml` 同步；精修改为用户手动开启，避免新用户首启即触发额外 LLM 调用。

## 2026-08-04 v2.3.0

- 编码器旋转快慢分档（Windows）：窗口格速 = steps × 100（格/秒）≥ `encoder_rotate_fast_threshold`（默认 200）判为快速手势，改注快速档按键（默认 cw=`pagedown` / ccw=`pageup`，慢速逐行、快速翻页）；一次快速手势只注入一次并进入停转锁定，屏蔽减速段慢速输出与换向事件，静默 >250ms 判定停稳后恢复识别；快速档按键非法回退普通按键；设置对话框「编码器」一节新增快速档按键控件与阈值滑杆（范围 100–300）。
- 编码器慢速注入延迟判定（Windows）：慢速事件先挂起 `encoder_rotate_decide_window_ms`（默认 80ms，0 关闭），窗内判快整段丢弃（消除快甩加速段的误逐行注入），到期成批补注（总量不变，慢转延迟 ≤80ms 无感）。
- 编码器设置迁移为设备级按设备覆盖（Windows）：从全局「设置」对话框移出，改为托盘设备子菜单「编码器设置…」打开设备级对话框（仅 `encoder_present` 设备显示），可针对每台设备单独配置 MiniEncoderC。
  - 配置结构：`app_config` 新增 `EncoderSettings` 结构体 + 全局 `default_encoder_settings` + `device_encoder_settings` 映射；`EncoderSettingsForDevice(device_id)` 按设备回落默认。旧顶层 `encoder_*` 扁平键仍兼容加载；按设备覆盖写入 `[device.<id>.encoder]` 表（键名去 `encoder_` 前缀，结构镜像 `[device.<id>.output]`），与全局默认相同的覆盖不落盘。
  - 设置对话框移除「编码器」整节（13 控件）；新增 `EncoderSettingsDialog`（13 项 + 恢复默认，恢复默认即清除该设备覆盖回落全局）；托盘设备子菜单新增入口（菜单 6000-6199）。
  - 协调器：连接/配置更新/各编码器事件均按 `device_id` 取 `EncoderSettingsForDevice` 单播下发 `led_color` 与录音门控；修复 R1（pending flush 丢失设备上下文，新增 `encoder_pending_device_id_`）与 R2（跨设备共享速度估计，新增 `last_encoder_rotate_device_id_` 在设备切换时复位）。
  - 单测：`TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate` 改为断言按设备单播；新增 `TestCoordinatorSyncsEncoderSettingsPerDeviceOverride`；`core_tests` 全量迁移到 `default_encoder_settings.*`。
  - 文档：`Doc/Ref/desktop-config.md` 编码器章重写为「全局默认 + 按设备覆盖」并附 TOML 示例；`CLAUDE.md`/`AGENTS.md`/`CODEBUDDY.md` 配置节同步。

- 设备交互设置迁移为设备级按设备覆盖（Windows）：IMU 唤醒灵敏度 / 敲击映射方向键 / 敲击灵敏度 / 体感鼠标左右·上下灵敏度，从全局「设置」对话框移出，改为托盘设备子菜单「设备交互设置…」打开设备级对话框（所有设备显示）；体感鼠标灵敏度按设备覆盖，其余进阶 `air_mouse_*` 参数（tau/invert_y/curve/control_mode/rate/neutral_deadzone）保持全局唯一。
  - 配置结构：`app_config` 新增 `InteractionSettings` 结构体 + 全局 `default_interaction_settings` + `device_interaction_settings` 映射；`InteractionSettingsForDevice(device_id)` 按设备回落默认。旧顶层 `imu_wake_sensitivity`/`tap_to_arrow`/`tap_sensitivity`/`air_mouse_sensitivity_x/y` 扁平键仍兼容加载；按设备覆盖写入 `[device.<id>.interaction]` 表（键名一致），与全局默认相同的覆盖不落盘。
  - 设置对话框移除「设备交互」整节（5 控件）；新增 `InteractionSettingsDialog`（5 项 + 恢复默认，恢复默认即清除该设备覆盖回落全局）；托盘设备子菜单新增入口（菜单 6200-6399）。
  - 协调器：连接/配置更新/敲击事件均按 `device_id` 取 `InteractionSettingsForDevice` 单播下发 `tap_enabled`/`tap_sensitivity`/`imu_wake_sensitivity`；`AirMouseParamsForDevice(device_id)` 按设备取灵敏度（gain = 灵敏度 × 48，与 `air_mouse_tuning_window` 增益倍率对齐修正 ×16 旧偏差），多设备体感态下运行期参数改为 `std::map<device_id, AirMouseParams>` 分别保存；体感鼠标热调参窗口标题带设备 ID、按激活设备调参，灵敏度写入该设备 `InteractionSettings`、其余进阶参数写全局。
  - 单测：`TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate` / `TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate` 改为断言按设备单播；新增 `TestCoordinatorSyncsInteractionSettingsPerDeviceOverride`；涉及旧字段的测试全量迁移到 `default_interaction_settings.*`。
  - 文档：`Doc/Ref/desktop-config.md` 新增「设备交互配置」章（全局默认 + `[device.<id>.interaction]` + TOML 示例 + 托盘入口 + 体感热调参说明）；`CLAUDE.md`/`AGENTS.md`/`CODEBUDDY.md` 配置节同步。

- MSI 内置 key 内测分发（Windows）：`OnboardingDialog` 配对设备后按 `NeedsAsrStep`（下沉 `voicestick_core`）判断，内置 key（`ActiveApiKey` 非空）跳过 `kAsr` 步直达 `kReady`，公开版仍走三步。新增 `SavePreservingDiskCredentials`：运行时 Save 先重读磁盘凭据再写回，避免内存过期 key 覆盖用户手改或内置 key，修复"替换 config 后 ASR 不工作"。`build-msi.bat` 经 `VOICESTICK_CONFIG_TEMPLATE` 注入真实密钥到 `config.template.toml`（首启复制到 `%APPDATA%`，不覆盖已有）。`build_win.bat` 修复 C++/WinRT 步骤 if 块内 `%ProgramFiles(x86)%` 括号解析错误（延迟扩展 + `PF_X86` 中转）。`build-msi.bat` 签名时间戳服务器 DigiCert 不可达换 Sectigo。

## 2026-07-29 v2.2.0

编码器设置与热词处理。

- 新增热词处理（Windows）：划词加词可选 LLM 提炼，长选文自动提取热词去重入表，浮窗展示结果 3 秒；设置对话框新增「热词处理」配置栏（启用开关 + 提示词，复用文本精修 LLM 配置）。
- 新增编码器设置项（Windows + 固件）：MiniEncoderC 旋转/单击/双击动作可配置。
  - 旋转 cw/ccw 自定义按键注入（key_spec 热键语法：方向键/enter/tab/pageup/pagedown/f1-f24/单字符/ctrl/alt/shift/win 组合），非法配置回退方向键。
  - 单击动作可选 recording（同主键录音语义，默认）或 key（注入自定义按键）；配 key 时桌面端派生下发固件录音门控关闭（编码器按压只发 click/double_click 按键事件）。
  - 双击动作可选 key（默认 enter，沿用取消当前会话结构）或 recording（经 remote_button 通道切换录音起停）。
  - 编码器录音灯颜色 8 预设（red/.../off）经 BLE 下发固件并 NVS 持久化，重启保持。
  - 固件按键事件新增 `source:"encoder"` 标签，桌面端按 source 路由编码器事件，物理键路径行为不变。
  - 设置对话框新增「编码器」一节（9 个控件，按键字段保存前 ParseKeySpec 校验）。

## 2026-07-25 v2.1.2

性能修复与录音稳定性。

- **perf(desktop/windows)**: 快速重启回连两轮提速（~10.8s → 中位 5.4s，7 样本全部一次成功）。
  - 一轮：僵尸链路安定窗（4.5s），根因定性为 Windows 僵尸链路（OS 持有已消失对端的旧链路，首次连接空挂 ~3.5-4s + 5s 退避）。
  - 二轮：安定窗缩至 1.5s（判出僵尸时已主动 Close，栈立即发 LL_TERMINATE，无需等被动监督超时）+ state 订阅 2.5s 应用层超时（撞未死僵尸提前取消，不再空挂 3.5-4s）+ zombie_suspect 免退避重试（15s 窗内最多 3 次，覆盖连按重启产生的多重僵尸）。
- **perf(desktop/windows)**: 深睡唤醒回连提速（连接编排 1.4s + boot 到广播 0.9s）。
- **fix(desktop/windows)**: 录音稳定性——finalizing/停滞 watchdog 兜底与 drain 尾帧保留，修复松开按钮后偶发卡 listening 与尾字丢失。
- **feat(firmware)**: 软件 AGC 电平归一与按键音抑制——HPF 后、Opus 前自研 AGC（target -6dBFS、max +20dB、噪声门、0.8FS 瞬时限幅），关闭硬件 ALC；开头 60ms 静音+淡入、drain 尾帧淡出消除按键音。轻声均值电平 -31~-39dB → -17~-25dB。
- **feat(scripts)**: 腾讯 ASR 回放工具与调试音频频谱分析页（`scripts/e2e_test/spectrogram_server.py`）。
- 文档：回连压缩设计与实测记录、BLE 僵尸链路经验、项目记忆蒸馏增补。

## 2026-07-21 v2.1.1

- **fix(windows)**: 设置保存闪退根治--`SaveSettings` 的 `config.Save()` 增加异常安全（try-catch），并修复路径含非 ASCII 字符时按 ACP(GBK) 解析抛 `system_error` 致 `std::terminate` 闪退（改用 UTF-8/UTF-16 显式转换）。
- **fix(windows)**: BLE 僵尸会话心跳探活与拆除重连，修复 deep sleep 唤醒后卡 pairing 死锁、无法回连录音。
- **feat(firmware)**: 点动模式主键双击直接注入回车，不触发录音。
- **feat(desktop/windows)**: 第三方输入法按触发模式（长按/点按）分别记忆触发热键。
- **fix(desktop/windows)**: 第三方输入法触发方式与全局 `interaction_mode` 解耦，修复切回 focused_app 长按失效。
- **fix(desktop/windows)**: 点动模式快速点动错位修复（`session_id` 校验）。
- 文档：新增 MSI 代码签名证书配置指南、蒸馏项目记忆为长期经验参考、CLAUDE.md/AGENTS.md 增量同步。

## 2026-07-06 v1.9.0

- **feat(wechat_input_method)**: 新增微信输入法语音输出模式——将识别语音经 Opus 解码为 PCM 后渲染到系统虚拟麦克风（如 VB-CABLE Output），供微信输入法等应用作为音频输入源。
  - Windows 端新增 Opus 解码器（vendored xiph/opus v1.5.2）、PCM 环形缓冲、WASAPI 虚拟麦克风渲染器（shared mode + AUTOCONVERTPCM）。
  - 配置模型、协调器状态机、设置对话框 UI 扩展支持 `wechat_input_method` 输出模式。
  - 修复虚拟麦 16kHz 格式被 WASAPI 拒绝导致渲染启动失败（需 AUTOCONVERTPCM）。
  - 接入调试音频缓存并修复 BLE 闪断后录音卡死（漏接 `CancelActiveCycleIfDeviceDisconnected` 断连清理分支）。
- **fix(audio)**: 改善近场 ASR 识别效果。
  - 根因：ES8311 PGA 固定 36 dB（最大档，63 倍）致近场 ADC 硬削波，谐波失真破坏语音频谱。固定增益不改变 SNR，问题是增益超出 ADC 线性区导致削波。
  - 方案一：PGA 36→24 dB + Opus 码率 20→32 kbps。
  - 方案二：启用 ES8311 硬件 ADC ALC（target -18dBFS + winsize=2 + automute off）自适应不同说话距离。
- **fix(windows)**: 去除管理员启动要求回退为 asInvoker，不再弹 UAC；开机自启从任务计划程序改回 `HKCU\...\Run`。
  - 权衡：`focused_app` 粘贴注入模式无法再向微信 4.0 等高完整性窗口发送 SendInput（UIPI）；微信输入法模式走虚拟麦克风渲染，不受影响。

## 2026-07-05 v1.8.2

- 版本号从 `v1.8.0` 更新到 `v1.8.2`。
- fix(windows): 修复腾讯 ASR 配置字段映射错误导致的 4002 "密钥不存在"。
  - 根因：`settings_dialog.cc` / `onboarding_dialog.cc` 中 ASR 提供商下拉框切换时只区分了 VoiceStick Cloud 与其他，切换到 Tencent 时错误地从 `volcengine_api_key` 加载密钥，用户保存后可能把正确的 Tencent SecretId 写入错误字段。
  - 修复：用显式 switch 把三个提供商各自映射到正确的配置字段。
  - `desktop/windows/src/app_config.cc`：加载配置时自动迁移——若当前提供商为 Tencent，且 `volcengine_api_key` 是 `AKID...` 形式的 Tencent SecretId，而 `tencent_secret_id` 为空或不以 `AKID` 开头，则自动回迁并落盘。
  - `desktop/windows/src/app_config.cc`：TOML 解析时对 `tencent_secret_id`、`tencent_secret_key`、`tencent_appid`、`volcengine_api_key`、`voicestick_api_key`、`llm_api_key` 等凭据字段自动 Trim，避免前后空格导致鉴权失败。
  - `desktop/windows/tests/core_tests.cc`：新增 `TestTencentSecretIdRecoveryFromVolcengineField()` 与 `TestTencentCredentialsTrimmedOnLoad()`。

## 2026-07-01 v1.8.0

- **breaking**: 移除 Wi-Fi STA 配网与局域网 HTTP(S) OTA pull 全链路，固件升级改走 BLE OTA（与 USB COM 口烧录，后者未来实现）。
  - 固件：删除 `components/voice_net/` 整个组件、`VOICE_NET_DISABLE` 宏与 `main.c` 全部 `voice_net_*` 调用点；`ota_commit` 改为直接 `esp_ota_mark_app_valid_cancel_rollback`；boot 兜底签到改为无条件；清理 `sdkconfig.defaults` 中 Wi-Fi/mDNS/TLS 相关项；`ui_status` 移除 Wi-Fi 信息行。
  - Windows：删除 `wifi_settings_dialog`/`wifi_credentials_win`/`ota_command`/`voice_stick_ctl` 四个源文件与 `VoiceStickCtl` 目标；清理 `ble_protocol`/`app_config`/`coordinator`/`ble_central_win`/`win32_app`/`settings_dialog`/`localization` 中 Wi-Fi/LAN-OTA 符号与 10 个 `core_tests`；保留 BLE OTA、固件清单、WinSparkle。
  - 文档：`Doc/Ref/protocol.md` 删 Wi-Fi 章节、`show_wifi_info`/`wifi_status` 帧；删除 5 个 `Doc/Plan` 方案文件与 `scripts/probe_wifi_provisioning.py`；`CLAUDE.md`/`AGENTS.md` 逐行清理。
  - macOS 端零改动（从未实现 Wi-Fi/LAN-OTA）。

## 2026-06-30 v1.7.2

- 版本号从 `v1.7.1` 更新到 `v1.7.2`。
- fix(windows): 修复开启精修后悬浮窗持续闪动、程序卡死。
  - 根因：`OverlayWindow::OnTimer` 在 `kListening` 模式下每 16ms 无条件 `InvalidateStaticLayer`，导致 `BuildStaticLayer`/`PaintText` 每 16ms 全量重建 D2D 文本布局（`CreateTextLayout` 不缓存），UI 线程渲染过载卡死；流式精修 token 每 ~60ms 重置 140ms 文字滚动过渡动画，`scroll_offset` 中途反复跳动闪动。
  - `desktop/windows/src/overlay_window.cc/.h`：`OnTimer` 在 kListening 静态文本时不重建 static layer，仅重绘动态指示器（音浪条），复用缓存文本布局；新增 `AppendPartial` 流式追加入口，跳过文字滚动过渡动画；`Show` 增加 `skip_text_transition` 参数。
  - `desktop/windows/src/voice_stick_coordinator.cc/.h`：`UiDelegate` 增加 `AppendPartial` 纯虚；`TransformText` 精修分支恢复 `refiner_.RefineStream`，`on_token` 节流式调 `AppendPartial` 逐字流式显示。
  - `desktop/windows/src/win32_app.cc/.h`：`AppendPartial` 转发至 `overlay_->AppendPartial`。
  - `desktop/windows/tests/core_tests.cc`：`FakeUi` 补 `AppendPartial` 实现。
  - 新增 `Doc/Plan/overlay-render-streaming-refine.md`。
  - 实测精修耗时 2.5~12.8s 随文本长度增长，LLM 延迟不可压缩；"压缩总时间"方向（definite 分段并行 / 倒计时并行）经查证否决，优化重心为"让精修等待可感知"。

## 2026-06-30 v1.7.1

- 版本号从 `v1.6.8` 更新到 `v1.7.1`。
- fix(固件): hold_to_talk 连接就绪过渡期录音启动增加重试。
  - 设备重连后 Windows 需重新做 GATT 服务发现 + 特征值订阅才能让 `ble_ready` 置位（约 1.5–2s）。在此过渡期内按住按钮触发 hold threshold，`start_recording` 会因 `ble_ready=0` 被拒，且只调一次不再重试，用户必须松开重按。
  - `firmware/main/main.c`：hold threshold 到点因 `ble_ready=0` 被拒时，只要按钮仍按下就按 100ms 间隔重试，覆盖订阅过渡期；超时（2s）或松开则干净放弃。只对 `ble_ready=0` 这一可恢复原因重试，ota/ui_state 等不可恢复原因走原放弃逻辑。复用 `s_double_click_timer`，新增 `s_recording_retry_pending` 状态，`handle_primary_up` 与断连清理同步处理。
  - 新增 `Doc/Plan/hold-to-talk-recording-start-retry.md`。
- fix(固件): 扩大 NimBLE mbuf 池并节流告警，消除音频通知瞬时耗尽。
  - 长录音中 central 处理慢 / conn interval 偏大时，NimBLE host 队列堆积未发送 notification，MSYS_1 池（100 块 ×256B）瞬时耗尽，出现 `tx seq=N mbuf alloc failed` 刷屏并丢帧。
  - `firmware/sdkconfig` / `sdkconfig.defaults`：`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` 100→200（多占约 25KB 内部 RAM，Wi-Fi 已禁用空间充裕）。真机长录音验证再无 alloc failed。
  - `firmware/components/voice_ble/voice_ble.c`：`voice_ble_send_audio` 的 alloc failed 告警节流（`s_mbuf_fail_streak`，每 10 次打印一条，恢复时打印恢复计数），断连清零。
- docs: 修正 `CLAUDE.md`/`AGENTS.md` 中 `Doc`/`docs` 措辞与 `run_build.bat` 悬空引用。

## 2026-06-26 v1.6.8

- 版本号从 `v1.6.7` 更新到 `v1.6.8`。
- feat(desktop, firmware): 在主设置中显示设备已连接 Wi-Fi 信息。
  - `desktop/windows/src/app_config.h/.cc`：新增 `DeviceWifiInfo` 结构、`show_device_wifi_info` 开关与 `device_wifi_infos` 映射；在 `[device.<id>.wifi_info]` 段持久化存储每个配对设备的 SSID 与 IP；解绑设备时同步清理；新增 `Load(path)`/`Save(path)` 重载便于测试。
  - `desktop/windows/src/ble_protocol.h/.cc`：新增 `ShowWifiInfoPayload()`，生成 `{"event":"show_wifi_info","enabled":...}` 控制帧。
  - `desktop/windows/src/ble_central_win.h/.cc`：新增 `SendShowWifiInfo()`，向指定或全部已连接设备下发 `show_wifi_info`。
  - `desktop/windows/src/voice_stick_coordinator.h/.cc`：连接成功后与配置变更时均发送 `show_wifi_info`；连接建立后向所有设备请求 `wifi_status_request`，确保状态及时同步。
  - `desktop/windows/src/settings_dialog.h/.cc`：新增"显示已连接 Wi-Fi 信息"复选框与只读的 SSID/IP 编辑框；打开设置时请求 Wi-Fi 状态，收到状态后刷新显示；窗口客户区高度由 720 增至 840。
  - `desktop/windows/src/win32_app.cc`：收到固件 `wifi_status` 后将 SSID/IP 写入 `config_.device_wifi_infos` 并持久化，同时触发设置界面刷新。
  - `desktop/windows/src/localization.h/.cc`：补充中英文"显示已连接 Wi-Fi 信息"、"Wi-Fi SSID"、"IP 地址"、"WIFI Idle"等文案。
  - `firmware/components/voice_net/include/voice_net.h` / `voice_net.c`：新增 `voice_net_set_status_changed_callback()` 与 `voice_net_get_status()`，在 STA 状态变化时回调 SSID/IP/状态字符串。
  - `firmware/main/main.c`：解析 `show_wifi_info` 控制事件；新增 `update_wifi_info_ui()` 与 `on_voice_net_status_changed()`；注册 Wi-Fi 状态变化回调，实现开关与网络变化的实时 UI 刷新。
  - `firmware/components/ui_status/include/ui_status.h` / `ui_status.c`：新增 `ui_status_set_wifi_text()` 与 `s_wifi_label`，在 IMU 调试行下方居中显示 SSID/IP 或 "WIFI Idle"，由桌面端开关控制显隐。
  - `desktop/windows/tests/core_tests.cc`：新增 `TestAppConfigWifiInfoRoundTrip()`，验证 `show_device_wifi_info` 与 `[device.<id>.wifi_info]` 的读写持久化。
  - 同步更新 `Doc/Ref/protocol.md`，补充 `show_wifi_info` 事件说明；新增 `Doc/Plan/...` 实施计划与 `Doc/...` 设计文档；删除过时的 `run_build.bat`。
- feat(firmware/ui): 放大设备号、电量与 Wi-Fi 信息字号，并将电量百分比移至电池图标下方。
  - `firmware/components/ui_status/ui_status.c`：
    - 顶部设备号 `s_top_label` 字体由 `lv_font_montserrat_10` 提升至 `lv_font_montserrat_16`，宽度由 66 增至 72，垂直位置略微下移至 y=2，提升可读性。
    - 电池图标 `s_battery_shell` 水平位置由 `-31` 右移至 `-8`，与右侧边界对齐。
    - 电量百分比 `s_battery_label` 字体由 10 提升至 16，宽度由 28 增至 46，并使用 `lv_obj_align_to` 对齐到电池图标底部右侧，避免与设备号/IMU 行重叠。
    - IMU 调试行 `s_imu_label` 垂直位置由 y=14 下移至 y=38，为大号设备号与电池图标留出空间。
    - Wi-Fi 信息行 `s_wifi_label` 字体由 10 提升至 14，垂直位置由 y=80 下移至 y=104，保持与 IMU 行的新布局间距。
