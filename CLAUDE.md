# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code 在本仓库工作时提供指导，工作语言为简体中文。

仓库根目录的 `AGENTS.md` 是本文件的同源副本，面向通用 AI 编码助手，内容（项目概览、构建命令、代码架构）与本文基本一致。修改本文涉及的整体性内容时，同步更新 `AGENTS.md`，避免两份文档漂移；以本文为权威源。

## 项目概览

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。设备负责采集按键与音频并通过 BLE 上报；桌面端负责交互状态机、ASR、文本显示与注入；网站负责落地页、浏览器端 USB 固件烧录和 Sparkle/WinSparkle 更新源。

当前版本：`1.8.0`（见仓库根目录 `VERSION`）。发布前需确保 `firmware/version.txt` 与 `VERSION` 一致。

主要目录：

- `firmware/`：ESP-IDF C 固件，目标板为 M5Stack StickS3 / ESP32-S3。
- `desktop/macos/`：SwiftPM/AppKit 菜单栏应用，目标 macOS 12+。
- `desktop/windows/`：C++20 / Win32 / C++/WinRT 托盘应用，目标 Windows 10 1903+；2019 年 Windows 10 构建会走地址直连的 BLE 兼容路径。
- `desktop/linux/`：Linux 桌面端占位目录。
- `website/`：Vue 3 + Vite 站点，包含 Web Serial 固件烧录工具、`vue-i18n` 中英文落地页和 appcast 发布页面。
- `Doc/`：参考文档（`Doc/Ref/`，含 BLE 协议、火山引擎/腾讯云 ASR 帧格式、发布流程、低功耗配置）、实施方案 RFC（`Doc/Plan/`）、火山引擎 ASR 服务端接入指南（`Doc/Guide/`）以及实验与反思记录（`Doc/Expe/`）。注意仓库里大写 `Doc/` 与小写 `docs/` 同时存在：前者是本项目文档，后者是 superpowers 工具的工作目录（`docs/superpowers/plans/`、`docs/superpowers/specs/`），二者无关，修改文档时认准大写 `Doc/`。

仓库关键文件树：

```text
firmware/
  main/main.c                      主循环：按键、BLE、录音、UI、电源、OTA
  components/
    stick_s3_board/                板级初始化：引脚、LCD、PMIC、I2S/codec
    audio_pipeline/                ES8311 → 16kHz PCM → Opus
    voice_ble/                     GATT 服务、通知、控制写入、BLE OTA
    bmi270/                        BMI270 IMU 驱动
    ui_status/                     ST7789/LVGL 渲染、亮度、休眠、OTA 进度
  partitions_ota.csv               两个 3MB OTA slot + ~1984KB storage
  version.txt                      固件版本（发布前须与 VERSION 一致）
desktop/macos/
  Package.swift                    Swift 5.9，Sparkle + TOMLKit + CZlib
  Sources/VoiceStickApp/           Swift 源码
  Config/config.example.toml       配置示例
desktop/windows/
  CMakeLists.txt                   拆为 voicestick_core + VoiceStickApp + voicestick_windows_tests
  src/voice_stick_coordinator.cc   交互状态机（核心）
  src/ble_central_win.cc           BLE 平台层
  src/ble_protocol.cc              BLE 协议与 OTA 命令编解码
  src/asr_client_win.cc            ASR WebSocket 客户端（火山 / VoiceStick Cloud）
  src/asr_client_tencent.cc        腾讯云 ASR 客户端
  src/input_injector_win.cc        SendInput / 剪贴板注入
  src/llm_chat_client.cc           LLM Chat 基类（OpenAI 兼容网络层）
  src/llm_refinement_client.cc     LLM 精修
  src/llm_translation_client.cc    LLM 翻译
  src/ogg_opus_muxer.cc            Opus → Ogg Opus 封装
  tests/core_tests.cc              核心库单元测试
website/
  src/App.vue                      主页面与 Web Serial 烧录
  src/i18n/zh-CN.json              中文文案
  src/i18n/en-US.json              英文文案
  public/appcast.xml               Sparkle/WinSparkle 更新源
scripts/                           构建与资源脚本
Doc/Ref/protocol.md                BLE 协议帧格式
Doc/Plan/                          实施方案 RFC
VERSION                            单一版本来源（纯文本）
ArduFlux.json                      ArduFlux IDE 配置（非版本控制重点）
```

## 常用命令

### 固件（ESP-IDF v5.5.1，目标 `esp32s3`）

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

烧录和串口监视：

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

设备仍使用旧单应用分区表时，首次升级到当前 OTA 分区表需要擦除后重刷：

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

固件依赖由 ESP-IDF component manager 管理，核心依赖包括 `espressif/button`、`espressif/esp_codec_dev`、`78/esp-opus`、`lvgl/lvgl`。当前分区表为两个 3 MB OTA app slot 加约 1984 KB `storage` 分区。

### macOS 桌面端（SwiftPM）

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

macOS 端目前没有专用测试目标。运行时会请求蓝牙权限；文本注入依赖模拟 `Command-V` 和可选 Return，如系统拦截按键事件，需要给终端或应用授予辅助功能权限。

SwiftPM 依赖见 `desktop/macos/Package.swift`：`sparkle-project/Sparkle` 2.6+（自动更新）、`LebJe/TOMLKit` 0.6+（配置解析）、内置 `CZlib`（Ogg CRC）。新增或升级依赖时在此文件修改。

发布构建和 DMG（在仓库根目录执行）：

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

推荐从仓库根目录使用：

```bat
build_win.bat
```

`build_win.bat` 会自动查找 VS 2022、结束残留的 `VoiceStick.exe` / `ninja.exe` / `cmake.exe` / `cl.exe` / `link.exe`，删除并重建 `desktop\windows\build-x64`，只构建不运行 CTest。运行前确认没有其它构建任务依赖这些进程。

手动构建（先进入 VS 2022 x64 开发者环境，或先调用 `vcvars64.bat`）：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
```

运行全部 Windows 测试：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure
```

运行单个/一组 CTest 目标（按 CTest 名称正则过滤）：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R <测试名称正则>
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

`voicestick_windows_tests` 是一个基于 `assert` 的测试可执行文件，目前不支持按测试函数名过滤；新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。

运行应用：

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI，用于 WinSparkle 更新）：

```bat
scripts\build-msi.bat
```

`build_native.bat` 会一并构建、测试并生成 MSI，但内部硬编码了旧版本号与本地路径，**复用前必须先检查内容**；`do_build.bat`、`desktop\windows\build.bat` 同样包含本机绝对路径或固定版本号，不可盲用。根目录 `test.bat` 目前只是占位脚本，不运行 CTest。

### 网站（Vue 3 + Vite）

```sh
cd website
npm install
npm run dev
npm run build
npm run preview
```

CI 使用 Node 22 和 `npm ci`。`website/package.json` 目前只定义了 `dev`、`build`、`preview`，没有 lint/test 脚本。修改网站后用 `npm run build` 作为最小验证；修改下载链接或固件回退地址时同步检查 `website/package.json` 的版本号。

### Lint / 格式化状态

仓库当前未提交统一 lint/formatter 配置；不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令。修改对应组件后运行该组件已有的构建/测试命令作为验证。

## 高层架构

### BLE 协议边界

BLE GATT 服务 UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

- `audio_tx`（通知，`0x5101`）：Opus 音频帧，设备 → 主机。
- `state_tx`（通知，`0x5102`）：按键事件、电量、固件版本等，设备 → 主机。
- `control_rx`（无响应写，`0x5103`）：`ui_state`、`interaction_mode`、`prompt_tone`、`show_imu_debug`、`imu_wake_sensitivity`、`tap_enabled`、`tap_sensitivity`、`air_mouse_enabled`、`ota_commit` 等，主机 → 设备。注意 `control_rx` 写入受 BLE MTU 限制，JSON 需控制长度避免溢出。
- `ota_rx`（写 / 无响应写，`0x5104`）：BLE OTA 控制与数据帧，主机 → 设备。
- `ota_tx`（通知，`0x5105`）：BLE OTA 状态帧，设备 → 主机。

完整帧格式见 `Doc/Ref/protocol.md`。修改 BLE 消息时，需要同步考虑固件、macOS、Windows 和文档。

### 固件职责

固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。

- `firmware/main/main.c` 编排按键、BLE、录音会话、UI 状态、电源管理和 OTA 事件。
- `components/audio_pipeline/` 从 ES8311 I2S 麦克风读取 16 kHz 单声道 PCM，编码为 Opus 后通过回调交给 BLE 层。
- `components/voice_ble/` 实现 GATT 服务、音频/状态通知、主机控制写入和 BLE OTA 数据流。
- `components/ui_status/` 基于 ST7789/LVGL 渲染状态界面、亮度、休眠前显示和 OTA 进度。
- `components/bmi270/` BMI270 IMU 驱动。
- `components/stick_s3_board/` 集中维护 StickS3 引脚、LCD、PMIC、I2S/codec 等板级初始化；引脚定义在 `firmware/components/stick_s3_board/include/stick_s3_board.h`。

### 桌面端职责

桌面端是状态唯一可信源，负责：BLE 配对和多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译、悬浮窗/字幕、文本注入、配置管理和自动更新。

核心音频路径：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
```

主机端不把 Opus 解码回 PCM；ASR 与调试音频缓存都使用同一份 Ogg Opus 流。

### macOS 实现

macOS 代码集中在 `desktop/macos/Sources/VoiceStickApp/`：

- `VoiceStickCoordinator`：交互状态机。
- `BleCentral` / `BleProtocol`：CoreBluetooth 和协议解析。
- `OggOpusMuxer` / `ASRWebSocketClient`：音频封装和 ASR 链路。
- `InputInjector`：剪贴板粘贴和 Return 注入。
- `OverlayController` / `SubtitleController` / `StatusController`：悬浮窗、字幕和菜单栏状态。
- `FirmwareManifest` / `FirmwareUpdateWindowController`：固件更新检查和窗口。

### Windows 实现

Windows 端在 `desktop/windows/CMakeLists.txt` 中拆成三个目标：

- `voicestick_core`：可测试核心库，包含配置解析、BLE 协议、Ogg Opus mux、ASR 帧格式、LLM 翻译、调试音频缓存、固件清单解析、日志、本地化和协调器状态机。
- `VoiceStickApp`：Win32 平台外壳，包含托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、配对/设置/固件更新等对话框。

新增核心行为优先放入 `voicestick_core`，并在 `desktop/windows/tests/core_tests.cc` 覆盖；测试目标名为 `voicestick_windows_tests`。Windows 代码遵循 Google C++ 命名风格：`snake_case` 文件名/变量，`CapWords` 类型，`MixedCase()` 方法，4 空格缩进。

### 网站实现

`website/src/App.vue` 承载主要页面和 Web Serial 烧录流程；`website/src/i18n/zh-CN.json` 与 `website/src/i18n/en-US.json` 保存中英文文案，浏览器语言以 `zh` 开头时默认中文。浏览器烧录器读取 `VITE_FIRMWARE_MANIFEST_URL` 指向的固件 manifest，并使用其中的 `merged_url`；加载失败时按 `website/package.json` 版本拼出回退 merged 固件 URL。新增或修改 UI 文案时同步维护两个语言文件。搜索仓库时排除 `website/node_modules/` 以免噪声过多。

## 核心交互模型

固件上报原始按键事实，桌面端解释为交互行为并回写 `ui_state`：

| 状态 | 主键（正面） | 侧键 |
|---|---|---|
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音 | 恢复上一次输入确认 |
| 录音中 | 释放结束录音 | 不取消当前录音 |
| 识别中 | 忽略新录音 | 取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 取消待粘贴文本 |

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式。文本输出支持 `focused_app`（默认粘贴到当前焦点，默认自动按 Return）和 `subtitle`（仅显示字幕）。识别结果可通过 OpenAI-compatible LLM 做翻译，也可按设备单独覆盖输出设置。

## 配置文件

示例配置：`desktop/macos/Config/config.example.toml`

运行时配置路径：

- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows（标准安装）：`%APPDATA%\VoiceStick\config.toml`
- Windows（便携模式）：程序同目录下的 `config.toml`
- Windows 调试音频缓存（标准安装）：`%LOCALAPPDATA%\VoiceStick\DebugAudio`
- Windows 调试音频缓存（便携模式）：程序同目录下的 `DebugAudio\`

**便携模式**：程序启动时检测同目录是否存在 `config.toml`，若存在则自动激活便携模式。便携模式下所有数据（配置、日志、调试音频）存储在程序目录而非系统 `%APPDATA%`，且禁用开机自启和自动更新（WinSparkle）。删除同目录 `config.toml` 后恢复标准安装版行为。便携版通过 `scripts/package-portable.bat` 构建打包。

常用配置项：

- `asr_provider`：`volcengine`、`voicestick_cloud` 或 `tencent`（腾讯云 ASR，需配合 `tencent_secret_id` / `tencent_secret_key` / `tencent_appid` / `tencent_engine_model_type` / `tencent_hotword_id`，详见 `Doc/Ref/tencent-asr.md`）。
- `volcengine_api_key` / `voicestick_api_key` / `voicestick_cloud_url`：ASR 服务的 API key 与 WebSocket URL。
- `llm_base_url` / `llm_api_key` / `llm_model`：OpenAI-compatible LLM 接口，用于翻译和精修。
- `resource_id`：火山引擎 resource ID，支持 `volc.seedasr.sauc.duration`、`volc.seedasr.sauc.concurrent`、`volc.bigasr.sauc.duration`、`volc.bigasr.sauc.concurrent`。
- `asr_hotwords`：逗号分隔的 ASR 热词，同时作为术语提示传给 LLM。
- `auto_enter`：粘贴后是否自动按 Return。
- `debug_audio_cache`：是否保存调试 Ogg Opus 音频（Windows 默认路径 `%LOCALAPPDATA%\VoiceStick\DebugAudio`）。
- `prompt_tone_enabled`：是否在录音启停时播放提示音，默认 `true`。
- `interaction_mode`：`hold_to_talk` 或 `click_to_talk`。
- `[output].target`：`focused_app` 或 `subtitle`。
- `[output].transform`：`original` 或 `translate`。
- `[output].translation_target`：目标语言代码，如 `en` 或 `zh-Hans`。
- `[device.<id>.output]`：按设备覆盖输出/翻译设置。
- `refine_enabled`：是否对 ASR 原文做 LLM 精修（去停顿空格、修标点、去口头语），默认 `true`；翻译路径的精修已融入翻译 prompt，不受此开关额外调用影响。`refine_prompt` 可覆盖内置精修 prompt（为空用默认）。
- `paired_device_ids`：逗号分隔的 4 位十六进制设备 ID，如 `C3D8,09AF`。用于限制仅连接指定设备。
- `show_imu_debug`：是否在设备屏幕上显示 IMU 加速度调试数值，默认 `false`。
- `device_theme_colors` / `device_overlay_positions`：按设备覆盖悬浮窗颜色和位置。
- `imu_wake_sensitivity`：IMU 拿起/晃动亮屏灵敏度，取值 `low` / `medium` / `high`。
- `tap_to_arrow`：Windows 端是否把固件检测到的双击敲击事件注入为方向键 Down，默认 `false`。
- `tap_sensitivity`：双击敲击灵敏度，整数 `1..10`，数值越大越灵敏，默认 `5`。
- `air_mouse_sensitivity_x`：体感鼠标左右（yaw）灵敏度档位（整数 `1..10`，默认 `5`），映射 `gain_x = sensitivity_x × 16`。设置界面滑杆调节。
- `air_mouse_sensitivity_y`：体感鼠标上下（pitch）灵敏度档位（整数 `1..10`，默认 `5`），映射 `gain_y = sensitivity_y × 16`。
- `air_mouse_tau`：体感鼠标速度环时间常数（秒，默认 `0.05`），手停滑行 ≈ 3×tau，越大缓停越长。
- `air_mouse_invert_y`：体感鼠标是否反转 Y 轴，默认 `false`。

## 安全与敏感信息

- **绝对不要提交 API key**。`config.toml` 中的 `volcengine_api_key`、`voicestick_api_key`、`llm_api_key` 均被 `.gitignore` 排除。
- 固件 OTA 更新时，桌面端会校验 `ota_size` 和 `ota_sha256` 后再写入设备。
- 发布产物（macOS DMG/ZIP、Windows MSI、固件 bin）均需签名或校验。

## 测试策略

- **Windows**：`desktop/windows/tests/core_tests.cc` 使用自定义 Fake/Mock 对 `voicestick_core` 中的状态机、配置解析、协议编解码、Ogg Opus mux 等进行单元测试。测试目标名为 `voicestick_windows_tests`（CMake CTest 目标）。新增核心行为时在 `core_tests.cc` 的 `main()` 中添加 `Test...()` 函数注册。
- **macOS**：目前没有专用测试目标。验证方式为 `swift build` 编译通过和运行时手动测试。
- **固件**：没有自动化单元测试。验证方式为 `idf.py build` 编译通过和真机运行时测试。
- **网站**：没有自动化测试。验证方式为 `npm run build` 构建通过。

## 代码风格

- **Swift（macOS）**：遵循标准 Swift/AppKit 命名。使用 `swift build` 验证编译。
- **C++（Windows）**：遵循 Google C++ 命名风格：`snake_case` 文件名和变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **C（固件）**：ESP-IDF 风格，组件通过 `idf_component_register` 注册，组件间通过 `REQUIRES` 声明依赖。
- **仓库当前未提交统一 lint/formatter 配置**；不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令。修改对应组件后运行该组件已有的构建/测试命令作为验证。

## 版本管理

版本单一来源是仓库根目录的 `VERSION` 文件（纯文本，不含换行）。发布前必须同步更新 `firmware/version.txt`，确保与 `VERSION` 内容一致——固件通过该文件向桌面端报告自身版本，版本不一致会导致 OTA 检测异常。

## 辅助脚本

`scripts/` 存放跨组件的构建与资源处理脚本，改动相关产物时按需调用；`scripts/ref/` 存放脚本的参考配置：

- `build-macos.sh` / `make-dmg.sh`：macOS 发布构建与 DMG 打包。
- `build-msi.bat`：Windows 签名 MSI 打包（WinSparkle 更新源）。
- `package-portable.bat`：构建 Windows 绿色便携版 ZIP 包，含预置 `config.toml` 模板和说明文件。
- `idf_cli.py`：ESP-IDF 编译/烧录/串口监控一体化脚本（`-c`/`-u`/`-s`/`-cus`，`-p COM17` 指定端口），Windows 上不便直接用 `idf.py` 时的便捷入口。配置文件为 `scripts/idf_cli.yaml`。
- `update-appcast.py`：根据 GitHub Release 更新 `website/public/appcast.xml`。
- `png_to_lvgl_argb_bin.py`：把 PNG 转成固件 LVGL 用的 ARGB 二进制资源，改 `ui_status` 图像资源后用。
- `slice_cat_sprites.py` / `tune_cat_sprites.py`：切片与调校状态界面精灵图。
- `probe_asr_websocket_ping.py`：探测 ASR WebSocket 连通性，调试 ASR 链路时用。

## 发布流程要点

推送与 `VERSION` 匹配的 `v<版本号>` 标签会触发 `.github/workflows/release.yml`：构建固件和 macOS 产物，发布 GitHub Release，并将固件 OTA/merged 镜像与 `manifest.json` 上传到阿里云 OSS。Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建并上传到对应 Release，然后手动运行 `Deploy Website to GitHub Pages` 工作流；该工作流会读取当前线上 appcast 和最新 GitHub Release，补齐可选 MSI 条目，若最新 Release 没有 MSI 会保留旧 Windows 条目。

完整发布步骤见 `Doc/Ref/release.md`。

## 重要约定

- 交互状态机在桌面端，不在固件中；修改交互流程时优先改桌面协调器。
- 固件通常只在新增/调整 `ui_state` 展示、硬件 I/O、BLE 协议或 OTA 行为时修改。
- 修改协议字段、状态枚举、配置项或发布产物格式时，同步检查 `Doc/Ref/`、macOS、Windows、网站和发布脚本。
- Windows 构建目录统一使用 `desktop/windows/build-x64`；旧的 `desktop/windows/build` 可能混入错误 VS/SDK 缓存，遇到链接异常时删除或忽略。
- `.gitignore` 整体忽略了 `desktop/windows/`，提交 Windows 端源码改动时必须用 `git add -f`，否则会被静默漏提交。

## 项目 Skills

本仓库在 `.agents/skills/` 下维护项目级 Skill，Agent 在相关场景会自动加载：

- `byted-web-search`：火山引擎豆包搜索，联网事实核查与信息检索场景优先使用。
- `sticks3-flash-ota`：M5Stack StickS3 固件烧录与 OTA 升级的标准流程；改完 `firmware/` 后需要把固件装到设备上验证时使用。
- `build-windows`：Windows 桌面端（VoiceStick.exe）构建与 CTest 流程；改完 `desktop/windows/` 下 C++ 源码后验证编译和测试。
- `build-firmware`：固件 ESP-IDF 构建流程；改完 `firmware/` 下 C 源码或 sdkconfig 后验证编译。

新增或修改 Skill 后，当前会话需要重启才能刷新可用技能列表。
