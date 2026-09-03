# 桌面端架构（macOS / Windows 模块）

本文承载桌面端架构细节，2026-08 由根目录 `AGENTS.md`/`CLAUDE.md` 的「桌面端职责」章节迁入；根指南只保留职责边界一句话与指向本文的指针。

桌面端是状态唯一可信源，负责 BLE 配对和多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译与精修、悬浮窗/字幕、文本注入、体感鼠标、配置管理和自动更新。

## macOS

代码分三个 SwiftPM target：

- `Sources/VoiceStickCore/`：平台无关纯逻辑，全部可单测——BLE 协议帧、ATVV 会话状态机（`XiaomiAtvvSession`，自 Windows `xiaomi_atvv_session` 逐行移植）、IMA-ADPCM 解码、PCM 后处理、Opus 重编码封装、Ogg Opus mux、ASR 帧格式、配置解析。新增核心行为优先放这里。
- `Sources/COpus/`：vendored libopus C target（Windows 侧 Opus 编码在 app 层，macOS 侧收到 COpus 供 Core 调用）。
- `Sources/VoiceStickApp/`：AppKit/CoreBluetooth 平台外壳——`VoiceStickCoordinator`（状态机，含编码器按键/敲击/旋转事件分发、设备交互与编码器设置逐台下发）、`EncoderRotateSpeedEstimator`（旋转快慢 EWMA 分档，对齐 Windows `encoder_speed.h`）、`BleCentral` / `BleProtocol`（CoreBluetooth 与协议帧，含小米 0x180F/0x2A19 电量读取合成 `battery_status`、power_log/power_mgmt 分发）、`OggOpusMuxer` / `ASRWebSocketClient` / `TencentASRClient`（+`TencentASRVocabClient` 热词表）（音频封装与火山/腾讯双 ASR）、`LLMRefinementClient` / `LLMTranslationClient`（精修/翻译）、`InputInjector` / `KeySpec`（粘贴/回车/热键语法按键注入）、`GlobalHotkeyManager`（全局热键→`remote_button`）、`XiaomiF5Suppressor`（CGEvent tap 抑制遥控器附带 F5）、`OverlayController` / `SubtitleController` / `StatusController`（悬浮窗/字幕/托盘菜单，含按设备子菜单与电量显示）、`FirmwareManifest` / `FirmwareUpdateWindowController`（固件更新，含本地文件 OTA）、`SettingsWindowController` 与配对/设备交互/编码器/遥控器/电池电压监测等窗口控制器。

测试在 `Tests/VoiceStickTests/`：CLT-only 环境无 XCTest 可用，用 executable runner（自写断言主程序，375 断言）经 `swift run VoiceStickTests` 执行。

## Windows

Windows 端在 `desktop/windows/CMakeLists.txt` 中拆成五个源码目标（另有 `winsparkle_lib` 导入库）：

- `voicestick_core`：可测试核心库，包含配置解析（含 `Active*()` 内置凭据回退访问器）、BLE 协议（含 `DeviceClass` 设备类与 `VS-`/`RC-` 双前缀设备 ID）、Ogg Opus mux、ASR 帧格式、LLM 翻译/精修（含热词 few-shot 与改坏回退守卫）、热词候选挖掘（`hotword_candidate_miner`）、调试音频缓存、固件清单解析、日志、本地化和协调器状态机；VoiceStickFlash 的烧录逻辑（`com_port_selector` / `esptool_flash_command` / `esptool_progress` / `voice_stick_flash_tool`）也在此。小米遥控器接入同为 core 纯逻辑模块（全部可单测）：`xiaomi_atvv_protocol`（UUID/opcode 常量、GET_CAPS/ACK/MIC_CLOSE 构造、CAPS 解析含旧版布局兼容）、`xiaomi_atvv_session`（ATVV 会话状态机：RC003 无 SYNC 硬重置、150ms 尾包宽限、300ms 重开拒绝、双击时序检测）、`ima_adpcm_decoder`、`pcm_postprocessor`（三点平滑 + 增益限幅、按协商帧长切帧）、`audio_opus_encoder`（16kHz/mono/40ms，参数对齐固件 audio_pipeline）；`PairedDeviceEntry.hardware` 派生能力集（has_screen/has_ota/has_encoder/has_imu/has_battery）驱动 `Send*` 跳过与托盘菜单显隐。
- `VoiceStickApp`：Win32 平台外壳，包含托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、配对/设置/固件更新等对话框。
- `VoiceStickFlash`：独立 COM 口固件烧录小工具（BLE OTA 之外的用户级兜底链路），Win32 GUI 外壳只做 UI + esptool 子进程；设计见 `Doc/Plan/windows-com-flash-tool.md`。
- `voicestick_windows_tests`：基于 `assert` 的单元测试，源码在 `desktop/windows/tests/core_tests.cc`，用自定义 Fake/Mock 不联网验证核心库；由 CTest 注册为同名测试，不支持按测试函数名过滤。
- `voicestick_integration_tests`：L1 ASR 链路集成测试，源码在 `desktop/windows/tests/integration_tests.cc`，用真实 `AsrClientWin` 连火山 ASR（需 `%APPDATA%\VoiceStick\config.toml` 配 `volcengine_api_key` 且联网），无 key 时返回 77 被 CTest 标记为 SKIP，不伪造结果。

新增核心行为优先放入 `voicestick_core`，并在 `desktop/windows/tests/core_tests.cc` 覆盖；跨链路验证可补到 `integration_tests.cc`。
