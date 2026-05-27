# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code 在本仓库工作时提供指导，工作语言为简体中文。

## 项目概述

Voice Stick 将 M5Stack StickS3 (ESP32-S3) 改造为桌面端蓝牙按键语音输入设备（支持 macOS、Windows，Linux 规划中）。按住设备按键录音，桌面端将音频流式传输至 ASR 服务，显示悬浮窗识别结果，并将最终文本粘贴到当前聚焦的输入框中。

## 整体架构

项目分为四个主要、低耦合的组件：

1. **`firmware/`** — StickS3 硬件的 ESP-IDF C 固件
   - `main/main.c` 编排按键、BLE 连接、录音会话、UI 状态、电源管理与 OTA 事件
   - `components/audio_pipeline/` 从 ES8311 I2S 麦克风读取 16 kHz 单声道 PCM，编码为 Opus，通过回调交给 BLE 层
   - `components/voice_ble/` 负责 GATT 服务、音频/状态通知、主机控制写入和 BLE OTA 数据流
   - `components/ui_status/` 负责 ST7789/LVGL 状态界面、亮度、休眠前显示准备和 OTA 进度
   - `components/stick_s3_board/` 集中维护 StickS3 引脚、LCD、PMIC、I2S/codec 等板级初始化
   - 固件仅上报原始按键事件并渲染主机下发的 `ui_state`；交互状态机不放在固件中

2. **桌面端应用** — 原生托盘应用，拥有完整的交互状态机
   - 桌面端是状态的唯一可信源（空闲 → 录音中 → 识别中 → 待确认 → 粘贴）；固件仅上报输入并渲染 UI
   - 将收到的 Opus 帧封装为 Ogg Opus 直接转发给 ASR（不解码/重编码）
   - 负责 BLE 配对、多设备连接、文本注入（剪贴板 + 模拟按键）、配置管理、自动更新
   - 各平台实现：
     - `desktop/macos/` — Swift/SwiftPM，目标 macOS 12+，使用 CoreBluetooth、Sparkle 自动更新
     - `desktop/windows/` — C++20/Win32/C++/WinRT，目标 Windows 10 1903+，使用 WinSparkle 自动更新
     - `desktop/linux/` — 占位工作目录
   - Windows 端通过 `voicestick_core` 复用配置解析、BLE 协议、Ogg Opus 封装、ASR 帧格式、LLM 翻译客户端、调试音频缓存和协调器状态机；Win32 外壳只负责托盘、窗口、BLE、剪贴板/按键注入与更新 UI。
   - macOS 端对应实现集中在 `desktop/macos/Sources/VoiceStickApp/`，其中 `VoiceStickCoordinator` 承载交互状态机，`BleCentral` 负责 CoreBluetooth，`OggOpusMuxer` 与 `ASRWebSocketClient` 负责音频转发链路，`InputInjector` 负责粘贴和回车注入。
   - Windows 端主入口为 `VoiceStickApp` 目标，核心库为 `voicestick_core`；平台外壳代码在 `win32_app`、`ble_central_win`、`input_injector_win`、窗口/对话框类中，跨平台业务逻辑优先放入 `voicestick_core` 并覆盖 `voicestick_windows_tests`。

3. **`website/`** — Vue 3 + Vite 站点，托管浏览器端 USB 固件烧录工具、Sparkle/WinSparkle 用的 appcast XML 更新源、落地页，通过 GitHub Pages 发布
   - `src/App.vue` 承载主要页面和 Web Serial 烧录流程
   - `src/i18n/` 保存中英文文案，新增 UI 文案时同步维护语言文件

4. **`docs/`** — BLE 协议规范、发布流程、火山引擎 ASR 帧格式说明

**BLE GATT 服务 UUID**: `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`
- `audio_tx`（通知, 0x5101）：Opus 音频帧 设备 → 主机
- `state_tx`（通知, 0x5102）：按键事件、电量、固件版本 设备 → 主机
- `control_rx`（无响应写, 0x5103）：ui_state、OTA 控制 主机 → 设备

完整帧格式和消息类型见 `docs/protocol.md`。

## 常用命令

### 固件（ESP-IDF v5.5.1，目标 esp32s3）
```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"   # 加载 IDF 环境
idf.py set-target esp32s3
idf.py build                              # 编译
idf.py -p /dev/cu.usbmodemXXXX flash monitor  # 烧录并打开串口监视器
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor  # 首次烧录，重写 OTA 分区表
```

### macOS 桌面端（SwiftPM）
```sh
cd desktop/macos
swift build              # 调试构建
swift run VoiceStickApp  # 本地运行
```
macOS 端当前没有专用测试目标；本地运行会请求蓝牙权限，文本注入依赖模拟 `Command-V` 和可选 Return，若按键事件被系统拦截，需要给终端或应用授予辅助功能权限。
发布构建 + 生成 DMG（在仓库根目录执行）：
```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

### Windows 桌面端（CMake + Ninja，MSVC 2022 x64）
仓库根目录提供了便捷构建脚本，无需手动进入 VS 开发者命令行：
```bat
:: 推荐的通用本地构建入口：自动查找 VS 2022，结束残留 VoiceStick/cmake/ninja/cl/link 进程，并重建 build-x64
build_win.bat

:: 本机发布辅助脚本：构建、运行 CTest，并生成本地 MSI；包含本机绝对路径和硬编码版本，复用前先检查内容
build_native.bat
```
`build_win.bat` 只构建，不运行 CTest。`test.bat` 目前只是占位脚本，不运行 CTest；需要测试时使用下方 `ctest` 命令。

手动构建（先进入 VS 2022 x64 开发者环境，或调用本机 `vcvars64.bat` 后执行）：
```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64

# 运行全部测试
ctest --test-dir desktop\windows\build-x64 --output-on-failure

# 运行单个测试（按名称正则过滤）
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R <测试名称正则>

# 当前 Windows 测试目标名
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests

# 运行
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI 安装包，用于 WinSparkle 自动更新）：
```bat
scripts\build-msi.bat
```
生成的 MSI 需上传到对应 GitHub Release，然后手动运行 `Deploy Website to GitHub Pages` 工作流更新更新源。`build_native.bat` 也会构建、测试并生成 MSI，但当前包含本机绝对路径和硬编码版本，作为本机脚本使用前先检查内容。

### 网站（Vite + Vue 3）
```sh
cd website
npm install
npm run dev        # 本地开发服务器
npm run build      # 生产构建
npm run preview    # 预览生产构建
```
`website/package.json` 目前只定义了 `dev`、`build`、`preview`，没有 lint/test 脚本。修改网站后用 `npm run build` 作为最小验证；需要人工检查时再运行 `npm run dev` 或 `npm run preview`。`website/node_modules/` 已出现在工作树中，搜索仓库时应排除依赖目录以免噪声过多。

## 核心交互模型

桌面端是唯一的状态持有者，固件仅上报原始按键事件并渲染收到的 `ui_state`。核心按键行为：

| 状态 | 主键（正面） | 侧键 |
|---|---|---|
| 连接空闲 | 按住开始录音 | 恢复上一次输入确认 |
| 录音中 | 释放结束录音 | 不取消当前录音 |
| 识别中 | 忽略新录音 | 取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 取消待粘贴文本 |

支持两种交互模式：`hold_to_talk`（按住说话，默认）和 `click_to_talk`（点击切换录音状态）。
文本输出支持两种模式：`focused_app`（粘贴到当前聚焦窗口，默认自动按回车）、`subtitle`（仅显示字幕）；还支持通过 LLM 对识别结果进行翻译，可按设备单独配置。

## 配置文件

示例配置位于 `desktop/macos/Config/config.example.toml`，配置路径：
- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows：`%APPDATA%\VoiceStick\config.toml`

常用配置项：
- `asr_provider`: `volcengine`（火山引擎直连）或 `voicestick_cloud`（云端中转）
- `auto_enter`: 粘贴后是否自动按回车（默认 true）
- `debug_audio_cache`: 是否保存调试音频（Ogg Opus 格式）
- `interaction_mode`: 交互模式 `hold_to_talk` / `click_to_talk`

## 重要约定与注意事项

- **状态机位于桌面端，不在固件中。** 固件仅上报按键事件并渲染收到的 `ui_state`。修改交互流程时，优先修改桌面端协调器；固件改动通常仅限于新增 `ui_state` 对应的画面。
- **音频流在主机端永不解码。** Opus 帧直接封装为 Ogg 页发送给 ASR，调试音频缓存写入的也是同一份 Ogg Opus 流。
- **配置文件路径：**
  - 示例配置：`desktop/macos/Config/config.example.toml`
  - macOS：`~/Library/Application Support/VoiceStick/config.toml`
  - Windows：`%APPDATA%\VoiceStick\config.toml`
  - Windows 调试音频缓存：`%LOCALAPPDATA%\VoiceStick\DebugAudio`
- **发布流程：** 推送与 `./VERSION` 匹配的 `v<版本号>` 标签。GitHub Actions 会构建 macOS 应用和固件，将固件 OTA/清单上传至阿里云 OSS，将产物发布到 GitHub Releases，并部署网站/appcast 到 GitHub Pages。签名后的 Windows MSI 需从本地签名机单独上传，然后重新运行 `Deploy Website to GitHub Pages` 工作流以收录 MSI 更新条目。详见 `docs/release.md`。
- **测试：** 仅 Windows 桌面端存在 CTest 测试套件（`desktop/windows/tests/`），当前测试可执行文件/CTest 名称为 `voicestick_windows_tests`。测试目标只链接 `voicestick_core`，因此新增核心行为应优先写成不依赖 Win32 UI 的测试。macOS Swift、固件与网站目前无专用测试套件；仓库内未提交 Lint 配置。
- **Windows 目标拆分：** `voicestick_core` 包含配置解析、BLE 协议、Ogg Opus mux、ASR 帧格式、LLM 翻译、调试音频缓存、固件清单解析和协调器状态机；`VoiceStickApp` 包含 Win32 托盘/窗口、BLE 中央、剪贴板/按键注入、全局热键、WinSparkle 与对话框。能放进 `voicestick_core` 的行为优先放入核心库并写入 `voicestick_windows_tests`。
- **Windows 构建目录：** 使用 `desktop/windows/build-x64`。如果旧的 `desktop/windows/build` 曾用错误的 Visual Studio 环境配置，可删除或忽略；混用 x86 CMake 缓存与 x64 SDK 库会导致链接错误。
- **Windows 构建脚本副作用：** `build_win.bat` 会结束残留的 `VoiceStick.exe`、`ninja.exe`、`cmake.exe`、`cl.exe`、`link.exe` 并重建 `desktop/windows/build-x64`；运行前注意是否有其它构建任务正在使用这些进程。
- **Windows 代码风格：** 遵循 Google C++ 命名规范：`snake_case` 文件名/变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **固件引脚定义：** `firmware/components/stick_s3_board/include/stick_s3_board.h`
