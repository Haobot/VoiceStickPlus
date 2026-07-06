# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code 在本仓库工作时提供指导，工作语言为简体中文。仓库根目录的 `AGENTS.md` 是本文的同源副本，面向通用 AI 编码助手；修改本文涉及的整体性内容时同步更新 `AGENTS.md`，避免两份文档漂移；以本文为权威源。

## 项目概览

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。设备负责采集按键、音频与 IMU 并通过 BLE 上报；桌面端负责交互状态机、ASR、文本显示与注入；网站负责落地页、浏览器端 USB 固件烧录和 Sparkle/WinSparkle 更新源。

核心音频数据流：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
```

桌面端不把 Opus 解码回 PCM；ASR 与调试音频缓存都使用同一份 Ogg Opus 流。

当前版本：`1.9.0`（见仓库根目录 `VERSION`）。发布前需确保 `firmware/version.txt` 与 `VERSION` 一致。

## 关键配置文件

| 文件 | 用途 |
|---|---|
| `VERSION` | 单一版本来源，纯文本，不含换行 |
| `firmware/version.txt` | 固件向桌面端报告的版本，发布前必须与 `VERSION` 一致 |
| `firmware/CMakeLists.txt` | ESP-IDF 项目入口 |
| `firmware/main/CMakeLists.txt` | 主组件注册与依赖声明 |
| `firmware/partitions_ota.csv` | 8 MB 分区表：两个 3 MB OTA app slot + 约 1984 KB `storage` |
| `desktop/macos/Package.swift` | SwiftPM 定义，依赖 Sparkle、TOMLKit、CZlib |
| `desktop/windows/CMakeLists.txt` | Windows 端构建，拆为 `voicestick_core` + `VoiceStickApp` |
| `desktop/windows/src/version.h.in` | Windows 版本资源模板，由 CMake 从 `VERSION` 填充 |
| `desktop/linux/` | Linux 桌面占位目录，目前无活跃实现 |
| `website/package.json` | Node 项目配置 |
| `website/public/appcast.xml` | Sparkle/WinSparkle 更新源 |
| `.github/workflows/release.yml` | 推送 `v*` 标签触发构建与发布 |
| `scripts/idf_cli.yaml` | `idf_cli.py` 的配置文件 |

## 技术栈

| 模块 | 语言/框架 | 构建工具 | 目标平台 |
|---|---|---|---|
| 固件 | C (ESP-IDF v5.5.1) | `idf.py` | ESP32-S3 (M5Stack StickS3) |
| macOS 桌面端 | Swift 5.9, AppKit, CoreBluetooth | SwiftPM | macOS 12+ |
| Windows 桌面端 | C++20, Win32, C++/WinRT, Direct2D | CMake + Ninja + MSVC 2022 x64 | Windows 10 1903+ |
| 网站 | Vue 3, Vite, vue-i18n | Vite | 静态站点 (GitHub Pages) |

固件关键外部依赖：`espressif/button`、`espressif/esp_codec_dev`、`78/esp-opus`、`lvgl/lvgl`，由 ESP-IDF component manager 通过各 `idf_component.yml` 管理。

## 构建与测试命令

### 固件（ESP-IDF v5.5.1，目标 `esp32s3`）

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

首次从旧单应用分区表升级时，需要擦除后重刷：

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

Windows 上不便直接用 `idf.py` 时：

```bat
python scripts/idf_cli.py -cus -p COM17
```

`idf_cli.py` 常用参数：`-c` 编译、`-u` 上传、`-s` 串口监控、`-cus` 编译+上传+监控、`-p COMxx` 指定串口。固件没有自动化单元测试，验证方式为 `idf.py build` 编译通过和真机运行时测试。

### macOS 桌面端（SwiftPM）

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

发布构建（在仓库根目录执行）：

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

macOS 端目前没有专用测试目标，无法运行单个测试；验证方式主要是 `swift build` 编译通过和运行时手动测试。

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

推荐从仓库根目录使用：

```bat
build_win.bat
```

该脚本会自动查找 VS 2022、结束残留进程、删除并重建 `desktop\windows\build-x64`，只构建不运行 CTest。注意：`build_win.bat` 历史上曾出现链接失败仍报成功的情况，构建后应核对 `desktop\windows\build-x64\VoiceStick.exe` 的时间戳与体积。

手动构建（需先进入 VS 2022 x64 开发者环境）：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
```

运行全部 Windows 测试：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure
```

按 CTest 名称正则过滤：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

`voicestick_windows_tests` 基于 `assert`，目前不支持按测试函数名过滤；新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。

运行应用：

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI）：

```bat
scripts\build-msi.bat
```

注意：`build_native.bat`、`do_build.bat`、`desktop\windows\build.bat` 包含本机绝对路径或固定版本号，复用前必须先检查内容；根目录 `test.bat` 目前只是占位脚本，不运行 CTest。

### 网站（Vue 3 + Vite，Node 22）

```sh
cd website
npm install
npm run dev      # 本地开发服务器
npm run build    # 最小验证
npm run preview  # 预览生产构建
```

`website/package.json` 目前只定义了 `dev`、`build`、`preview`，没有 lint/test 脚本。修改网站后用 `npm run build` 作为最小验证。修改网站 UI 文案时，必须同步更新 `website/src/i18n/zh-CN.json` 和 `website/src/i18n/en-US.json`。

## 架构边界

### 状态机归属

交互状态机在桌面端，不在固件中。修改交互流程时优先改桌面协调器（macOS 的 `VoiceStickCoordinator` / Windows 的 `voice_stick_coordinator.cc`）。固件通常只在新增/调整 `ui_state` 展示、硬件 I/O、BLE 协议或 OTA 行为时修改。修改协议字段、状态枚举、配置项或发布产物格式时，同步检查 `Doc/Ref/`、macOS、Windows、网站和发布脚本。

### BLE 协议边界

GATT service UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| 特征 | UUID | 方向 | 属性 | 用途 |
|---|---|---|---|---|
| `audio_tx` | `…5101` | 设备 → 主机 | notify | Opus 音频帧 |
| `state_tx` | `…5102` | 设备 → 主机 | notify | 按键事件、电量、固件版本、体感鼠标运动帧 |
| `control_rx` | `…5103` | 主机 → 设备 | write without response | `ui_state`、交互/敲击/体感鼠标设置、`ota_commit` 等；JSON 需控制长度避免 BLE MTU 溢出 |
| `ota_rx` | `…5104` | 主机 → 设备 | write / write without response | BLE OTA 控制与数据帧 |
| `ota_tx` | `…5105` | 设备 → 主机 | notify | BLE OTA 状态帧 |

完整帧格式见 `Doc/Ref/protocol.md`。

### 固件职责

固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。关键组件：

- `firmware/main/main.c`：主循环，编排按键、BLE、录音会话、UI 状态、电源管理和 OTA 事件。
- `components/audio_pipeline/`：从 ES8311 读取 16 kHz 单声道 PCM，编码为 Opus 后交给 BLE 层。
- `components/voice_ble/`：GATT 服务、通知、控制写入、BLE OTA。
- `components/ui_status/`：ST7789/LVGL 渲染、亮度、休眠、OTA 进度。
- `components/bmi270/`：BMI270 IMU 驱动。
- `components/stick_s3_board/`：板级初始化，引脚定义在 `include/stick_s3_board.h`。

### 桌面端职责

桌面端是状态唯一可信源，负责 BLE 配对和多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译与精修、悬浮窗/字幕、文本注入、配置管理和自动更新。

macOS 代码集中在 `desktop/macos/Sources/VoiceStickApp/`：`VoiceStickCoordinator`（状态机）、`BleCentral` / `BleProtocol`（CoreBluetooth 与协议）、`OggOpusMuxer` / `ASRWebSocketClient`（音频封装与 ASR）、`InputInjector`（粘贴与 Return 注入）、`OverlayController` / `SubtitleController` / `StatusController`（悬浮窗/字幕/状态）、`FirmwareManifest` / `FirmwareUpdateWindowController`（固件更新）。

Windows 端在 `desktop/windows/CMakeLists.txt` 中拆成三个目标：

- `voicestick_core`：可测试核心库，包含配置解析、BLE 协议、Ogg Opus mux、ASR 帧格式、LLM 翻译/精修、调试音频缓存、固件清单解析、日志、本地化和协调器状态机。
- `VoiceStickApp`：Win32 平台外壳，包含托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、配对/设置/固件更新等对话框。

新增核心行为优先放入 `voicestick_core`，并在 `desktop/windows/tests/core_tests.cc` 覆盖。

### 核心交互模型

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

## 代码风格

- **Swift（macOS）**：标准 Swift/AppKit 命名。
- **C++（Windows）**：Google C++ 风格：`snake_case` 文件名和变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **C（固件）**：ESP-IDF 风格，组件通过 `idf_component_register` 注册，组件间通过 `REQUIRES` 声明依赖。
- **Vue/JS（网站）**：Vue 3 Composition API 风格。

仓库当前未提交统一 lint/formatter 配置；不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令。修改对应组件后运行该组件已有的构建/测试命令作为验证。

## 测试策略

- **Windows**：`desktop/windows/tests/core_tests.cc` 使用自定义 Fake/Mock 对 `voicestick_core` 中的状态机、配置解析、协议编解码、Ogg Opus mux 等进行单元测试。运行命令：`ctest --test-dir desktop/windows/build-x64 --output-on-failure`。
- **macOS**：目前没有专用测试目标。验证方式主要是 `swift build` 编译通过和运行时手动测试。
- **固件**：没有自动化单元测试。验证方式是 `idf.py build` 编译通过和真机运行时测试。
- **网站**：没有自动化测试。验证方式是 `npm run build` 构建通过。

## 发布流程

推送与 `VERSION` 匹配的 `v<版本号>` 标签会触发 `.github/workflows/release.yml`：

1. 构建固件（ESP-IDF v5.5.1，目标 `esp32s3`），生成 OTA bin、merged bin 与 `manifest.json`。
2. 构建并签名 macOS 产物（DMG、ZIP、Sparkle 签名）。
3. 创建 GitHub Release，合并固件与 macOS 产物。
4. 上传固件到阿里云 OSS 的版本目录和 `latest/` 目录。
5. 触发 `deploy-website.yml` 更新 `website/public/appcast.xml`。

Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建并签名，然后上传到对应 GitHub Release，再手动运行 `Deploy Website to GitHub Pages` 工作流以收录 MSI 条目。完整步骤见 `Doc/Ref/release.md`。

## 项目 Skills

本仓库在 `.agents/skills/` 下维护项目级 Skill，相关场景会自动加载：

- `byted-web-search`：火山引擎豆包搜索，联网事实核查与信息检索场景优先使用。
- `sticks3-flash-ota`：M5Stack StickS3 固件烧录与升级流程；改完 `firmware/` 后需要把固件装到设备上验证时使用。
- `build-windows`：Windows 桌面端构建与 CTest 流程；改完 `desktop/windows/` 后验证编译和测试。
- `build-firmware`：固件 ESP-IDF 构建流程；改完 `firmware/` 后验证编译。

新增或修改 Skill 后，当前会话需要重启才能刷新可用技能列表。

## 给 Agent 的提示

- `.gitignore` 整体忽略了 `desktop/windows/`，提交 Windows 端源码改动时必须用 `git add -f`，否则会被静默漏提交。
- Windows 构建目录统一使用 `desktop/windows/build-x64`；旧的 `desktop/windows/build` 可能混入错误 VS/SDK 缓存，遇到链接异常时删除或忽略。
- 搜索仓库时请排除 `website/node_modules/` 和 `firmware/build/` 以免噪声过多。
- 修改网站 UI 文案时，必须同步更新 `website/src/i18n/zh-CN.json` 和 `website/src/i18n/en-US.json`。
- 修改根目录 `README.md` 时，必须同步更新 `README.zh-CN.md`。
- 修改 `VERSION` 时，必须同步更新 `firmware/version.txt`。
- 修改协议或公共数据结构时，必须同时更新 `Doc/Ref/protocol.md` 和所有实现端（固件 C、macOS Swift、Windows C++）。
- 设计方案文档统一放在 `Doc/Plan/`（大写 P），不再使用 `Doc/Rfc/`。
