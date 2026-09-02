# VoiceStick — Agent 工作指南

本文件是面向 AI 编码助手的**核心枢纽（Hub）**：只保留每次任务都可能用到的核心知识（项目定位、构建速查、架构红线、高频坑），扩展细节按主题分布在 `Doc/` 下各文档中——用到哪个主题，按文末「文档索引」直接去查对应文档。

`CLAUDE.md`（Claude Code）与 `CODEBUDDY.md`（CodeBuddy）是本文的同源副本，三者内容一致，修改整体性内容时同步更新三份，避免漂移。

## 项目概览

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。设备采集按键、音频与 IMU 经 BLE 上报；**桌面端是状态唯一可信源**，负责交互状态机、ASR、文本显示与注入；网站负责落地页、浏览器端 USB 固件烧录（esptool-js）和 Sparkle/WinSparkle 更新源。

输入设备两类并存：自研 StickS3（设备 ID `VS-XXXX`，配固件）与小米蓝牙遥控器 2 Pro（`RC-XXXX`，仅 Windows 端，**固件零改动**，ATVV 接入全部在桌面端）。

核心音频数据流：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
                                                              \-> Opus decode -> PCM -> 虚拟麦克风（微信输入法模式）
```

ASR 路径不把 Opus 解码回 PCM；微信输入法模式是例外（解码 PCM 渲染到系统虚拟麦克风，不经 ASR 文本注入）。小米遥控器音频在进协调器前归一化为与 StickS3 相同的标准 Opus 帧，下游零改动。

当前版本见根目录 `VERSION`（纯文本，无换行）；发布前必须确保 `firmware/version.txt` 与之一致。

## 技术栈

| 模块 | 语言/框架 | 构建工具 | 目标平台 |
|---|---|---|---|
| 固件 | C (ESP-IDF v5.5.1) | `idf.py` | ESP32-S3 (M5Stack StickS3) |
| macOS 桌面端 | Swift 5.9, AppKit, CoreBluetooth | SwiftPM | macOS 12+ |
| Windows 桌面端 | C++20, Win32, C++/WinRT, Direct2D | CMake + Ninja + MSVC 2022 x64 | Windows 10 1903+ |
| 网站 | Vue 3, Vite, vue-i18n, esptool-js | Vite | 静态站点 (GitHub Pages) |

固件关键外部依赖：`espressif/button` ^4.1.6、`espressif/esp_codec_dev` ^1.3.4 + `78/esp-opus` ^1.0.5、`lvgl/lvgl` 9.2.0，由 ESP-IDF component manager 经各组件 `idf_component.yml` 管理。

## 构建与测试速查

| 模块 | 最小验证 | 测试 |
|---|---|---|
| 固件 | `cd firmware && idf.py build`（Windows 可用 `python scripts/idf_cli.py -c`） | 无单测，编译通过 + 真机验证 |
| macOS | `cd desktop/macos && swift build` | 无测试目标，编译通过 + 手动测试 |
| Windows | 根目录 `build_win.bat` | `ctest --test-dir desktop\windows\build-x64 --output-on-failure` |
| 网站 | `cd website && npm run build` | 无自动化测试 |

各平台完整命令、发布构建、MSI 打包、E2E 真机工具链见 `Doc/Agent/build-and-test.md`。

## 架构边界红线

- **交互状态机在桌面端，不在固件中**。改交互流程优先改桌面协调器（macOS `VoiceStickCoordinator` / Windows `voice_stick_coordinator.cc`）；固件只管硬件 I/O、音频编码、BLE、电源管理和渲染主机下发的 `ui_state`。例外：双击时序检测在固件端（小米遥控器语音键则在桌面端适配层），桌面端统一响应 `button_double_click`。
- **修改协议字段、状态枚举、配置项或发布产物格式时**，同步检查 `Doc/Ref/protocol.md`、固件 C、macOS Swift、Windows C++、网站和发布脚本。
- **小米遥控器固件完全不改**：ATVV 会话/ADPCM 解码/Opus 归一化全部在桌面端完成。
- BLE GATT service UUID `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`（`audio_tx`/`state_tx`/`control_rx`/`ota_rx`/`ota_tx` 五特征）；v1.8.0 起固件升级只走 BLE OTA。特征表与帧格式见 `Doc/Ref/protocol.md`；小米遥控器走 Google ATVV profile（`AB5E0001-…`），同文 ATVV 设备档案章节。
- 固件组件与板级引脚映射见 `Doc/Agent/firmware-architecture.md`；桌面端模块划分见 `Doc/Agent/desktop-architecture.md`；交互状态表与按键语义见 `Doc/Agent/interaction-model.md`。

## 配置

桌面端运行时配置：macOS `~/Library/Application Support/VoiceStick/config.toml`，Windows `%APPDATA%\VoiceStick\config.toml`。完整字段说明见 `Doc/Ref/desktop-config.md`，示例见 `desktop/macos/Config/config.example.toml`。

## 代码风格

- **Swift（macOS）**：标准 Swift/AppKit 命名。
- **C++（Windows）**：Google C++ 风格：`snake_case` 文件名和变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **C（固件）**：ESP-IDF 风格，组件通过 `idf_component_register` 注册，组件间通过 `REQUIRES` 声明依赖。
- **Vue/JS（网站）**：Vue 3 Composition API 风格。

仓库未提交统一 lint/formatter 配置；不要臆造 lint 命令，用各组件已有的构建/测试命令验证。

## 安全红线

- API 密钥等凭据只存在于本机 `config.toml`，不进仓库；示例配置用占位符。
- 内置凭据（`VOICESTICK_BUILTIN_*`）与 MSI 打包链路生成的含 key 产物均 gitignored，不得提交。
- 集成测试与 E2E 坚持「不伪造结果」：无凭据/无设备时 SKIP 或报错，不 mock 真实链路。
- 固件 OTA 与桌面端自动更新走官方渠道（GitHub Release + 阿里云 OSS + appcast），不绕过签名校验。

细节见 `Doc/Agent/release-and-security.md`；发布权威流程见 `Doc/Ref/release.md`。

## 给 Agent 的提示（高频坑）

- Windows 桌面端修改构建通过后，自动重启 VoiceStick.exe 让改动生效（先结束运行中进程，启动后查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`），无需用户另行指示。
- `.gitignore` 整体忽略 `desktop/windows/`：提交 Windows 源码必须 `git add -f`，并用 `git ls-files <path>` 逐个验证新文件已被跟踪（教训：f75af4f5 曾因漏提交导致全新克隆无法构建）。
- Windows 构建目录统一用 `desktop/windows/build-x64`；旧 `desktop/windows/build` 可能混入错误 VS/SDK 缓存。
- 搜索仓库时排除 `website/node_modules/` 和 `firmware/build/`。
- 同步规则：改网站 UI 文案同步 `website/src/i18n/zh-CN.json` + `en-US.json`；改 `README.md` 同步 `README.zh-CN.md`；改 `VERSION` 同步 `firmware/version.txt`；改协议同步 `Doc/Ref/protocol.md` 与全部实现端。
- `Doc/` 子目录语义：`Ref/`（协议/配置/流程等事实参考）、`Plan/`（设计方案）、`Guide/`（第三方服务接入指南）、`Expe/`（经验教训）、`Agent/`（面向 AI 助手的工作知识，即本 Hub 的扩展文档）。根目录 `docs/superpowers/`（小写）是历史产物，不再维护，新设计方案放 `Doc/Plan/`。
- `build_native.bat` / `do_build.bat` / `desktop\windows\build.bat` 含本机绝对路径或固定版本号，复用前先检查内容；根目录 `test.bat` 是占位脚本；根目录散落的 `*.log`、`%BUILD_LOG%` 等是构建残留日志，不是源码。
- Windows 构建若报 `C1083: winrt/base.h` 找不到：`build_win.bat` 已用 SDK 自带 `cppwinrt.exe` 生成投影头到 `desktop/windows/generated_winrt/`（gitignored）并 prepend 到 `INCLUDE`；手动 vcvars64 构建时须同样加入。
- MiniEncoderC 编码器是 I2C 外设，不能作为深睡唤醒源；主键（GPIO11）是唯一唤醒键。Grove 口 5V 不启用，编码器由顶部 Hat 排针供电。
- VoiceStickFlash 改动除构建/CTest 外，用 `scripts\prepare_flash_payload.ps1` 冒烟 payload；真机验收清单见 `Doc/Plan/windows-com-flash-tool.md` §7.2。
- `scripts/` 下辅助脚本：`probe_asr_websocket_ping.py`（ASR 连通性）、`probe_hotword_extraction.py`（热词提炼链路探测）、`update-appcast.py`（生成 appcast）、`idf_cli.py`（包装 idf.py）、`png_to_lvgl_argb_bin.py` 等 LVGL 资源工具、`scripts/e2e_test/`（E2E 真机验证）。

## 文档索引

用到哪个主题，直接查对应文档：

| 主题 | 文档 |
|---|---|
| 构建命令与测试策略（全平台细节、E2E） | `Doc/Agent/build-and-test.md` |
| 关键配置文件清单 | `Doc/Agent/config-files.md` |
| 固件架构（组件、引脚、电源） | `Doc/Agent/firmware-architecture.md` |
| 桌面端架构（macOS/Windows 模块） | `Doc/Agent/desktop-architecture.md` |
| 核心交互模型（状态表、按键语义） | `Doc/Agent/interaction-model.md` |
| 发布流程与安全细节 | `Doc/Agent/release-and-security.md`；权威流程 `Doc/Ref/release.md` |
| BLE 协议与帧格式（含小米 ATVV 档案） | `Doc/Ref/protocol.md` |
| 桌面端配置字段 | `Doc/Ref/desktop-config.md` |
| E2E 真机验证工具链 | `Doc/Ref/e2e-test-toolchain.md` |
| 经验教训记忆（排查问题前先查） | `Doc/Expe/claude-memory-distilled.md`（寄存器值/阈值/行号为记录时点结论，引用前以当前源码为准） |
| 项目 Skills（`.agents/skills/`，场景命中自动加载） | `sticks3-flash-ota`（固件烧录/OTA）、`build-windows`、`build-firmware`、`usb-jtag-flash-log`（串口日志采集）、`work-summary-retro`（工作总结/经验沉淀/教训反思的文档管理）；新增/修改 Skill 后需重启会话刷新 |
