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
