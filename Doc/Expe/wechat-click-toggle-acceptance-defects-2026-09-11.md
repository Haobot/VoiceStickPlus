# 方案 A 真机首验两缺陷：虚拟麦回环零音频 + 测试进程抹写用户 config

- 日期：2026-09-11（方案 A 见 `Doc/Rfc/xiaomi-wechat-click-toggle-2026-09-12.md`，前置排查见 `Doc/Expe/wetype-voice-injection-blocked-2026-09-11.md`）
- 相关文件：`desktop/windows/src/wasapi_mic_capture.cc`、`desktop/windows/src/voice_stick_coordinator.cc`、`desktop/windows/src/app_config.cc`、`desktop/windows/src/mic_capture.h`
- 相关 commit：d4fbfd13（两修复）；2fe74d21（引入缺陷的方案 A 主链路）

## 症状

方案 A（trigger=click_to_talk + session_model=hold_to_talk）链路激活成功——button_click 折叠、auto_switch 切默认录音设备、renderer 打开 CABLE Input、SendDown 注入、WeType 浮窗弹出——但**音量条零变化、无音频传输**。另伴随：已配对小米遥控器在 VoiceStick 端永远显示"正在连接中"，须在系统蓝牙设置删除重配对方可激活。

## 日志判据

- 回环缺陷：`wechat latency: auto_switch end`（默认录音设备已切虚拟麦）之后才出现 `WasapiMicCapture: capturing from default microphone`——采集器按"默认设备"解析时解析到的已是虚拟麦采集端。修复后该行变为 `capturing from pinned endpoint`。
- 地址清零：启动日志有 `scan started` 但**无** `Queueing paired device connect RC-xxxx`（启动排队要求 `entry.bluetooth_address != 0`），配对条目形如 `6459,000000000000,0,,xiaomi_remote_2_pro,`（地址 0/kind 0/名字空/hardware 保留）。

## 根因

1. **虚拟麦回环**：时序为 click → auto_switch（默认录音设备 → CABLE Output）→ renderer.Start（CABLE Input）→ SendDown → 本机麦采集 Start。采集器 `GetDefaultAudioEndpoint(eCapture, eConsole)` 在 Start 时刻解析默认设备——此刻默认已是 CABLE 采集端，于是采集到的是自己渲染进 CABLE 的回环：ring buffer → 渲染 → CABLE → 采集 → ring buffer 自旋，真实麦克风从未被采集。
2. **地址清零**：`AppConfig::SavePairedDeviceInfo` 对内存配对列表**未找到**的 device_id 会新建零地址条目并全量 `Save()`。协调器单元测试（2fe74d21 新增的小米方案 A 用例）喂了带 hardware 的 device_info 事件，测试配置（Defaults，无配对条目）走未找到分支 → 把测试进程的 Defaults 配置**整份覆盖**真实 `%APPDATA%\VoiceStick\config.toml`：凭据（火山/腾讯/LLM key）、asr_hotwords、模型路径、[xiaomi.keys] 全被抹成默认值，配对地址清零致启动排队连接失效（用户侧症状="正在连接中"，删除重配对可恢复地址但救不回凭据）。core_tests.cc 既有的能力门控用例注释早已写明"不注入 device_info 事件，因此完全不触达 SavePairedDeviceInfo 落盘真实 config.toml"——新用例违反了这条已知约束。

## 修复（d4fbfd13）

1. 回环：`IMicCapture` 新增 `SetPreferredEndpointId`（默认空实现），协调器在 `StartLocalMicForWechatSession` 把 auto_switch 保存的**切换前端点**（`saved_default_capture_id_`，本来就为恢复默认设备而保存）钉进采集器；`WasapiMicCapture` 非空时按 `IMMDeviceEnumerator::GetDevice(id)` 打开，**失败不回退默认设备**（回退即回环），如实报错走会话回滚。auto_switch 关闭/切换失败时无保存值，保持默认设备行为（本机麦克风模式零影响）。
2. 清零：`SavePairedDeviceInfo` 未找到时直接 return——零地址条目对用户无价值（排队连接要求地址非零），hardware 自愈仅对既有条目有意义；连带消除了"无配对状态的配置副本全量覆盖用户 config"的写盘面。

用户侧恢复：凭据/热词/模型路径等从当日 07:46 备份（`config.toml.bak-20260911`）外科手术式回填；配对地址由用户删除重配对自愈。

## 验证

- 单测：小米方案 A 用例注入假切换器（默认设备=real-mic-ep，虚拟麦=cable-ep），断言第一击后 `preferred_endpoint_id == "real-mic-ep"` 且默认设备已切 cable-ep、第二击后切回 real-mic-ep；新增未知设备用例断言 `paired_devices/paired_device_ids` 保持为空。红→绿各按正确断言失败后通过。
- 端到端防回归：全单测运行前后对用户 config.toml 做 md5 比对——修复前必被写（事故实证），修复后**逐字节不变**。
- CTest 2/2 通过（集成测试带真实凭据全跑）。
- 真机：修复版已部署、RC-6459 重连、ATVV 会话创建正常；**音频回环修复的真机验收（音量条跳动+说话上屏）待用户执行**。

## 长期技术记忆

- **"默认设备"类资源要在改变默认之前解析/钉扎**：任何"auto_switch 默认设备 + 采/放同一虚拟麦对"的架构里，采集端必须显式钉住切换前的真实端点，不能事后按默认解析——时序决定一切。
- **测试二进制与生产共用真实落盘路径 = 定时炸弹**：`AppConfig::Save()` 系列无路径注入缝，任何触发 Save 的用例都会写真实用户 config。本次修复只拆了引信（未知设备不落盘），**根治仍欠路径注入缝**（构造时注入 ConfigPath 或 portable 重定向），新增涉及 Save/SavePairedDevice* 的用例前先想这条。
- **协调器用例喂 device_info（带 hardware/version）= 触发落盘**：需要小米身份时按能力门控用例的模式在测试 config 里 seed `paired_devices` 条目（IsXiaomiRemoteDevice 有配对种子兜底路径），不喂事件。
- 排查"配置莫名被改"先比对文件 mtime 与测试运行时刻；诊断日志 grep 时间段必须锚定 `] HH:MM:` 形态，裸 `HH:MM` 子串会匹配到其他小时的 MM:MM（本次 02:13 被误读成 13:13）。
- Python 经 bash heredoc 传反斜杠字面量会被传输层吃一层：构造 Windows 路径用 `chr(92)` 运行时拼接并回读验证，别信源码里的转义层数。

## 遗留/观察项

- `AppConfig` 落盘路径注入缝（测试隔离的根治方案）未做；`integration_tests.cc` 的 `AppConfig::Load()` 经 `MaybeRecoverTencentSecretId` 迁移保存仍会规范式回写用户 config（内容保真但 mtime 变化，见 `Doc/Agent/build-and-test.md` 警告）。
- 钉扎端点打开失败（如用户会话中途拔掉真实麦克风）走会话回滚提示"麦克风启动失败"，不做设备热切换——观察真机是否需要更细的恢复策略。

## 追加（同日第三次真机反馈）：钉扎之后仍零音频——哑麦克风在源头

d4fbfd13 部署后用户仍报零音频。日志证明链路已全通（`capturing from pinned endpoint`、会话正常起停、WeType 面板弹出），最终用 sounddevice 对每个录音设备录 2 秒测 RMS 定案：**默认录音设备（Realtek 麦克风阵列）未静音、音量 100%，但采集为纯数字零（RMS=0.5/peak=1）**——阵列侧哑了（典型诱因：睡眠/唤醒后 Realtek 阵列驱动挂死；不排除硬件电平开关）。钉扎修复只保证"采对设备"，不保证"设备有声"。

系统侧同时发现两个其他麦克风端点被静音（"麦克风"@86%、"Microphone"@100%），已解除；**CABLE Output 从未静音**（排除了 WeType 侧被静音的假设）。全机唯一活麦 = "Microphone (2- MateView)"（华为显示器麦第二总线实例，RMS≈1081）。处置：经 IPolicyConfig `{F8679F50-…}`/`{870af99c-…}` `SetDefaultEndpoint` 把默认录音设备（eConsole 角色）切到该活麦；VoiceStick 无需重启——auto_switch 在会话开始时才读默认并钉扎，下一次点击自动生效。原默认为 `{0.0.1.00000000}.{fdfb1b53-…}`（麦克风阵列），声音设置可随时改回。

诊断配方（复用）：

- **逐设备录 2 秒看 RMS/peak 是判"哑麦"的唯一硬证据**：peak≤2（满量程 32768）即数字零，正常的设备即使安静房间也有底噪（RMS 几十起步）；未静音+100% 音量+数字零=驱动挂死或硬件电平开关，不是音量问题。
- 端点静音枚举：pycaw 的 `AudioUtilities.GetAllDevices()` 不建模采集端（`Endpoint=None`），须走 `IMMDeviceEnumerator::EnumAudioEndpoints(eCapture, ACTIVE)` + `IMMDevice::Activate(IAudioEndpointVolume)`；名称对齐别猜 EnumAudioEndpoints 顺序（与声音设置顺序不一致），用注册表 `MMDevices\Audio\Capture\<guid>\Properties` 值名 `{a45c254e-…},2` 读友好名。
- 隐私授权三处（HKCU microphone / NonPackaged / HKLM）均为 Allow 才排除隐私因素；Deny 时 WASAPI 通常直接激活失败而非静音。
- 切默认录音设备：comtypes 定义 IPolicyConfig 全 12 方法 vtable（槽位必须齐），`SetDefaultEndpoint(id, 0=eConsole)`；pycaw 不含此接口。仓库生产实现见 `default_audio_device_controller.h`（同源移植）。
- 产品观察：auto_switch+本机麦直供架构隐含依赖"切换前默认录音设备是活麦"，后续可考虑会话启动时对钉扎端点做静音检测（前 300ms RMS≈0 时提示用户检查麦克风）。

