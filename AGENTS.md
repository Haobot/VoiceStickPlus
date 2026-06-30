# VoiceStick — Agent 工作指南

本文件面向 AI 编码助手，帮助其在没有先验知识的情况下快速理解并正确修改本仓库。

本文件是仓库根目录 `CLAUDE.md` 的同源副本，面向通用 AI 编码助手，内容（项目概览、构建命令、代码架构）基本一致，以 `CLAUDE.md` 为权威源。修改整体性内容时两份同步更新，避免漂移。

## 项目概览

VoiceStick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。

- **固件端**：负责采集按键与音频，通过 BLE 上报给桌面端；同时负责电源管理、设备显示、BLE OTA 与 Wi-Fi / LAN HTTP(S) OTA。
- **桌面端**：是交互状态的唯一可信源，负责 BLE 配对与多设备连接、Opus → Ogg Opus 封装、ASR WebSocket、LLM 翻译与精修、悬浮窗/字幕、文本注入、配置管理和自动更新。
- **网站端**：负责落地页、浏览器端 USB 固件烧录，以及 Sparkle / WinSparkle 更新源。

核心音频数据流：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
```

桌面端不把 Opus 解码回 PCM；ASR 与调试音频缓存都使用同一份 Ogg Opus 流。

当前版本：`1.6.8`（见仓库根目录 `VERSION`）。发布前需确保 `firmware/version.txt` 与 `VERSION` 一致。

## 关键配置文件

| 文件 | 用途 |
|---|---|
| `VERSION` | 单一版本来源，纯文本，不含换行 |
| `firmware/version.txt` | 固件向桌面端报告的版本，发布前必须与 `VERSION` 一致 |
| `firmware/CMakeLists.txt` | ESP-IDF 项目入口，定义 `project(voice_stick)` |
| `firmware/main/CMakeLists.txt` | 主组件注册，声明 `SRCS` 与 `REQUIRES` |
| `firmware/main/idf_component.yml` | 主组件依赖（当前仅 `espressif/button: ^4.1.6`） |
| `firmware/components/*/CMakeLists.txt` | 各组件源码与依赖声明 |
| `firmware/components/*/idf_component.yml` | 组件级 ESP-IDF component manager 依赖 |
| `firmware/partitions_ota.csv` | 8 MB flash 分区表：两个 3 MB OTA app slot + 约 1984 KB `storage` |
| `desktop/macos/Package.swift` | SwiftPM 包定义，依赖 Sparkle、TOMLKit、CZlib |
| `desktop/windows/CMakeLists.txt` | Windows 桌面端 CMake，拆分为 `voicestick_core`、`VoiceStickApp`、`VoiceStickCtl` |
| `desktop/windows/src/version.h.in` | Windows 版本资源模板，由 CMake 从 `VERSION` 填充 |
| `website/package.json` | Node 项目配置，当前 `version` 为 `0.3.4` |
| `website/vite.config.js` | Vite 配置，`base: '/voicestick/'` |
| `website/public/appcast.xml` | Sparkle / WinSparkle 更新源 |
| `.github/workflows/release.yml` | 推送 `v*` 标签时触发：构建固件、构建 macOS、发布 Release、上传 OSS |
| `.github/workflows/deploy-website.yml` | 部署网站到 GitHub Pages，并更新 appcast |
| `scripts/idf_cli.yaml` | `idf_cli.py` 的配置文件 |
| `ArduFlux.json` | ArduFlux IDE 配置文件（非版本控制重点） |

仓库没有 `pyproject.toml`、`Cargo.toml`、`package.json`（根目录）等全局配置文件；各子系统使用各自原生构建配置。

## 仓库目录结构

```text
firmware/                       ESP-IDF C 固件（ESP32-S3）
  main/main.c                   主循环：按键、BLE、录音会话、UI 状态、电源管理、OTA
  main/CMakeLists.txt           主组件注册与依赖声明
  main/idf_component.yml        主组件依赖声明（当前仅依赖 espressif/button）
  components/
    stick_s3_board/             板级初始化：引脚、LCD、PMIC、I2S/codec
    audio_pipeline/             从 ES8311 读取 16 kHz 单声道 PCM，编码为 Opus
    voice_ble/                  GATT 服务、音频/状态通知、控制写入、BLE OTA 数据流
    voice_net/                  Wi-Fi STA 配网、mDNS/局域网发现、LAN HTTP(S) OTA pull
    bmi270/                     BMI270 IMU 驱动
    ui_status/                  ST7789/LVGL 状态界面、亮度、休眠、OTA 进度、Wi-Fi 信息
  managed_components/           ESP-IDF component manager 拉取的依赖
  partitions_ota.csv            分区表：两个 3 MB OTA app slot + 约 1984 KB storage
  version.txt                   固件向桌面端报告自身版本，发布前必须与 VERSION 一致
  CMakeLists.txt                ESP-IDF 项目入口
  sdkconfig                     当前 sdkconfig（被 .gitignore 忽略，勿随意提交）

desktop/macos/                  SwiftPM / AppKit 菜单栏应用（macOS 12+）
  Package.swift                 Swift 5.9，依赖 Sparkle、TOMLKit、CZlib
  Sources/VoiceStickApp/        Swift 源码
  Sources/CZlib/                zlib CRC 桥接
  Config/config.example.toml    配置示例（与 Windows 共享字段）
  Resources/                    图标、Info.plist 等资源

desktop/windows/                C++20 / Win32 / C++/WinRT 托盘应用（Windows 10 1903+）
  CMakeLists.txt                拆分为 voicestick_core、VoiceStickApp、VoiceStickCtl
  src/                          C++ 源码
  tests/core_tests.cc           核心库单元测试
  installer/                    WiX MSI 安装包定义
  third_party/                  cjson、tomlplusplus
  resources/                    图标、对话框资源、VoiceStick.rc
  build-x64/                    推荐构建目录（由 build_win.bat 创建）
  build-msi-x64/                签名 MSI 构建目录（由 scripts/build-msi.bat 创建）

desktop/linux/                  Linux 桌面端占位目录

website/                        Vue 3 + Vite 站点
  package.json                  Node 项目配置，当前 version 字段为 0.3.4
  vite.config.js                base: '/voicestick/'
  src/App.vue                   主页面与 Web Serial 烧录流程
  src/i18n/                     中英文文案（zh-CN.json、en-US.json、index.js）
  public/appcast.xml            Sparkle / WinSparkle 更新源

scripts/                        构建脚本、精灵图处理、LVGL ARGB 转换、ASR 探测、appcast 更新
Doc/                            BLE 协议（Doc/Ref/protocol.md）、发布流程（Doc/Ref/release.md）、
                                ASR 接口、实施方案 RFC（Doc/Plan/）
VERSION                         单一版本来源（纯文本，不含换行）
ArduFlux.json                   ArduFlux IDE 配置文件（非版本控制重点）
```

注意仓库里大写 `Doc/` 与小写 `docs/` 同时存在：前者是本项目文档，后者是 superpowers 工具的工作目录（`docs/superpowers/plans/`、`docs/superpowers/specs/`），二者无关，修改文档时认准大写 `Doc/`。

## 技术栈

| 模块 | 语言 / 框架 | 构建工具 | 目标平台 |
|---|---|---|---|
| 固件 | C (ESP-IDF v5.5.1) | `idf.py` | ESP32-S3 (M5Stack StickS3) |
| macOS 桌面端 | Swift 5.9, AppKit, CoreBluetooth | SwiftPM | macOS 12+ |
| Windows 桌面端 | C++20, Win32, C++/WinRT, Direct2D | CMake + Ninja + MSVC 2022 x64 | Windows 10 1903+ |
| 网站 | Vue 3, Vite, vue-i18n | Vite | 静态站点 (GitHub Pages) |
| 脚本 | Python 3, Bash, Batch | — | — |

关键外部依赖：
- **固件**：
  - `espressif/button: ^4.1.6`（主组件与按键驱动）
  - `espressif/esp_codec_dev: ^1.3.4`（audio_pipeline 组件）
  - `78/esp-opus: ^1.0.5`（audio_pipeline 组件，Opus 编码）
  - `lvgl/lvgl: 9.2.0`（ui_status 组件）
  - `espressif/mdns: ^1.2`（voice_net 组件）
  由 ESP-IDF component manager 通过各 `idf_component.yml` 管理。
- **macOS**：`sparkle-project/Sparkle` (2.6+)、`LebJe/TOMLKit` (0.6+)。
- **Windows**：WinSparkle 0.9.2（FetchContent）、WiX Toolset v6、WinHTTP、bcrypt。

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

Windows 上不便直接用 `idf.py` 时，可用仓库提供的封装脚本：
```bat
python scripts/idf_cli.py -cus -p COM17
```

`idf_cli.py` 常用参数：
- `-c`：编译（compile）
- `-u`：上传/烧录（upload）
- `-s`：串口监控（serial monitor）
- `-cus`：编译 + 烧录 + 监控
- `-p COMxx`：指定串口

当前分区表 `firmware/partitions_ota.csv` 为两个 3 MB OTA app slot 加约 1984 KB `storage` 分区。

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

macOS 端目前没有专用测试目标。

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

推荐从仓库根目录使用：
```bat
build_win.bat
```

该脚本会自动查找 VS 2022、结束残留进程、删除并重建 `desktop\windows\build-x64`，只构建不运行 CTest。

手动构建（需先进入 VS 2022 x64 开发者环境）：
```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
```

运行全部 Windows 测试：
```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure
```

运行单个/一组测试（按 CTest 名称正则过滤）：
```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

运行应用：
```powershell
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI）：
```bat
scripts\build-msi.bat
```

注意：`build_native.bat` 会一并构建、测试并生成 MSI，但内部硬编码了旧版本号与本地路径，复用前需检查内容；`do_build.bat` 是更早期的本地构建脚本；根目录 `test.bat` 目前只是占位脚本，不运行 CTest。

### 网站（Vue 3 + Vite，Node 22）

```sh
cd website
npm install
npm run dev
npm run build
npm run preview
```

`website/package.json` 目前只定义了 `dev`、`build`、`preview`，没有 lint/test 脚本。修改网站后用 `npm run build` 作为最小验证。

## 代码组织与架构

### 固件端职责

固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，**不持有桌面交互状态机**。

- `firmware/main/main.c`：编排按键、BLE、录音会话、UI 状态、电源管理和 OTA 事件。
- `components/audio_pipeline/`：从 ES8311 I2S 麦克风读取 16 kHz 单声道 PCM，编码为 Opus 后通过回调交给 BLE 层。
- `components/voice_ble/`：实现 GATT 服务、音频/状态通知、主机控制写入和 BLE OTA 数据流。
- `components/ui_status/`：基于 ST7789/LVGL 渲染状态界面、亮度、休眠前显示、OTA 进度，以及由桌面端开关控制的 Wi-Fi 信息（SSID/IP）显示。
- `components/voice_net/`：Wi-Fi STA 配网、mDNS/局域网发现和 HTTP(S) LAN OTA pull（`esp_https_ota`）。凭据由桌面端经 `control_rx` 下发并持久化到 NVS，状态通过 `state_tx` 的 `wifi_status` 帧回报；Wi-Fi 必须等 BLE 稳定连接后再启动，OTA pull 前要过 main.c 注入的 park gate。Wi-Fi 射频按需启停：空闲倒计时归零或录音开始时自动关闭，下次操作命令到达时自动重启。契约见 `Doc/Ref/protocol.md`，计划见 `Doc/Plan/wifi-sta-ble-provisioning.md`、`Doc/Plan/lan-http-ota-pull-design.md`、`Doc/Plan/wifi-on-demand-power-management.md`。
- `components/bmi270/`：BMI270 IMU 驱动。
- `components/stick_s3_board/`：集中维护 StickS3 引脚、LCD、PMIC、I2S/codec 等板级初始化。引脚定义在 `firmware/components/stick_s3_board/include/stick_s3_board.h`。

### 桌面端职责

桌面端是状态唯一可信源，负责：BLE 配对和多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译、悬浮窗/字幕、文本注入、配置管理和自动更新。

### macOS 实现

代码集中在 `desktop/macos/Sources/VoiceStickApp/`：

- `VoiceStickCoordinator`：交互状态机。
- `BleCentral` / `BleProtocol`：CoreBluetooth 和协议解析。
- `OggOpusMuxer` / `ASRWebSocketClient`：音频封装和 ASR 链路。
- `InputInjector`：剪贴板粘贴和 Return 注入。
- `OverlayController` / `SubtitleController` / `StatusController`：悬浮窗、字幕和菜单栏状态。
- `FirmwareManifest` / `FirmwareUpdateWindowController`：固件更新检查和窗口。

### Windows 实现

`desktop/windows/CMakeLists.txt` 中拆成三个目标：

- `voicestick_core`：可测试核心库，包含配置解析、BLE 协议、Ogg Opus mux、ASR 帧格式、LLM 翻译、调试音频缓存、固件清单解析、日志、本地化和协调器状态机。
- `VoiceStickApp`：Win32 平台外壳，包含托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、配对/设置/固件更新/Wi-Fi 设置等对话框。
- `VoiceStickCtl`（`src/voice_stick_ctl.cc` + `src/ota_command.cc`）：命令行 OTA 工具，经 BLE 触发 Wi-Fi 配网与 LAN HTTP OTA pull，调试固件升级用。

新增核心行为优先放入 `voicestick_core`，并在 `desktop/windows/tests/core_tests.cc` 覆盖；测试目标名为 `voicestick_windows_tests`。

### 网站实现

`website/src/App.vue` 承载主要页面和 Web Serial 烧录流程；`website/src/i18n/zh-CN.json` 与 `website/src/i18n/en-US.json` 保存中英文文案，`index.js` 初始化 `vue-i18n`。新增或修改 UI 文案时同步维护两个语言文件。

### BLE 协议边界

BLE GATT 服务 UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

- `audio_tx`（notify，`0x5101`）：Opus 音频帧，设备 → 主机。
- `state_tx`（notify，`0x5102`）：按键事件、电量、固件版本、`wifi_status`（Wi-Fi/IP/OTA pull 进度）等，设备 → 主机。
- `control_rx`（write without response，`0x5103`）：`ui_state`、BLE OTA 控制、`wifi_set`/`wifi_clear`/`wifi_status_request`、`ota_pull`/`ota_commit`、`show_imu_debug`、`show_wifi_info`、`imu_wake_sensitivity` 等，主机 → 设备（受 BLE MTU 限制，JSON 需控制长度）。
- `ota_rx` / `ota_tx`：BLE OTA 数据通道。

完整帧格式见 `Doc/Ref/protocol.md`。修改 BLE 消息时，需要同步考虑固件、macOS、Windows 和文档。

## 核心交互模型

固件上报原始按键事实（`button_down` / `button_up`），桌面端解释为交互行为并回写 `ui_state`：

| 状态 | 主键（正面） | 侧键 |
|---|---|---|
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音 | 恢复上一次输入确认 |
| 录音中 | 释放结束录音 | 不取消当前录音 |
| 识别中 | 忽略新录音 | 取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 取消待粘贴文本 |

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式。文本输出支持 `focused_app`（默认粘贴到当前焦点，默认自动按 Return）和 `subtitle`（仅显示字幕）。识别结果可通过 OpenAI-compatible LLM 做翻译，也可按设备单独覆盖输出设置。

## 状态机归属

- **交互状态机在桌面端，不在固件中**；修改交互流程时优先改桌面协调器（`VoiceStickCoordinator`）。
- 固件通常只在新增/调整 `ui_state` 展示、硬件 I/O、BLE 协议或 OTA 行为时修改。
- 修改协议字段、状态枚举、配置项或发布产物格式时，同步检查 `Doc/Ref/`、macOS、Windows、网站和发布脚本。

## 配置文件

示例配置：`desktop/macos/Config/config.example.toml`

运行时配置路径：
- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows（标准安装）：`%APPDATA%\VoiceStick\config.toml`
- Windows（便携模式）：程序同目录下的 `config.toml`

常用配置项：
- `asr_provider`：`volcengine`、`voicestick_cloud` 或 `tencent`。
- `auto_enter`：粘贴后是否自动按 Return。
- `debug_audio_cache`：是否保存调试 Ogg Opus 音频。
- `prompt_tone_enabled`（Windows 桌面端）：是否在录音启停时播放提示音，默认 `true`。
- `show_imu_debug`（Windows 桌面端）：是否在设备屏幕上显示 IMU 加速度调试数值，默认 `false`。
- `show_device_wifi_info`（Windows 桌面端）：是否在设备屏幕上显示已连接 Wi-Fi 的 SSID 与 IP，默认 `false`。
- `interaction_mode`：`hold_to_talk` 或 `click_to_talk`。
- `[output].target`：`focused_app` 或 `subtitle`。
- `[output].transform`：`original` 或 `translate`。
- `[output].translation_target`：目标语言代码，如 `en` 或 `zh-Hans`。
- `[device.<id>.output]`：按设备覆盖输出/翻译设置。
- `[device.<id>.wifi_info]`：按设备持久化保存已连接 Wi-Fi 的 SSID 与 IP（由 Windows 端写入）。
- `refine_enabled`：是否对 ASR 原文做 LLM 精修（去停顿空格、修标点、去口头语），默认 `true`；翻译路径的精修已融入翻译 prompt，不受此开关额外调用影响。`refine_prompt` 可覆盖内置精修 prompt（为空用默认）。
- `asr_hotwords`：逗号分隔的 ASR 热词，同时作为术语提示传给 LLM。
- `paired_device_ids`：逗号分隔的 4 位十六进制 ID，如 `C3D8,09AF`。
- `resource_id`：火山引擎 resource ID，支持 `volc.seedasr.sauc.duration`、`volc.seedasr.sauc.concurrent`、`volc.bigasr.sauc.duration`、`volc.bigasr.sauc.concurrent`。
- `tencent_secret_id` / `tencent_secret_key` / `tencent_appid` / `tencent_engine_model_type` / `tencent_hotword_id`：腾讯云 ASR 配置（仅 `asr_provider = "tencent"` 时有效）。
- `device_theme_colors` / `device_overlay_positions`：按设备覆盖悬浮窗颜色和位置。
- `imu_wake_sensitivity`：IMU 拿起/晃动亮屏灵敏度，取值 `off` / `low` / `medium` / `high`。

Windows 调试音频缓存目录：
- 标准安装：`%LOCALAPPDATA%\VoiceStick\DebugAudio`
- 便携模式：程序同目录下的 `DebugAudio\`

**便携模式**：程序同目录存在 `config.toml` 时自动激活。数据存储于程序目录，禁用开机自启和自动更新。便携版构建用 `scripts/package-portable.bat`。

## 代码风格

- **Swift（macOS）**：遵循标准 Swift/APIKit 命名。使用 `swift build` 验证编译。
- **C++（Windows）**：遵循 Google C++ 命名风格：`snake_case` 文件名和变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **C（固件）**：ESP-IDF 风格，组件通过 `idf_component_register` 注册，组件间通过 `REQUIRES` 声明依赖。
- **Vue/JS（网站）**：使用标准 Vue 3 Composition API 风格。
- **仓库当前未提交统一 lint/formatter 配置**；不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令。修改对应组件后运行该组件已有的构建/测试命令作为验证。

## 测试策略

- **Windows**：`desktop/windows/tests/core_tests.cc` 使用自定义 Fake/Mock 对 `voicestick_core` 中的状态机、配置解析、协议编解码、Ogg Opus mux 等进行单元测试。运行命令：`ctest --test-dir desktop/windows/build-x64 --output-on-failure`。
- **macOS**：目前没有专用测试目标。验证方式主要是编译通过（`swift build`）和运行时手动测试。
- **固件**：没有自动化单元测试。验证方式是 `idf.py build` 编译通过，以及真机/开发板运行时测试。
- **网站**：没有自动化测试。验证方式是 `npm run build` 构建通过。

## 版本管理

版本单一来源是仓库根目录的 `VERSION` 文件（纯文本，不含换行）。发布前还需要同步更新 `firmware/version.txt`，因为固件通过该文件向桌面端报告自身版本，版本不一致会导致 OTA 检测异常。

推送与 `VERSION` 匹配的 `v<版本号>` 标签会触发 `.github/workflows/release.yml`。例如 `VERSION` 内容为 `1.6.8` 时，标签必须是 `v1.6.8`。

## 发布与部署流程

推送 `v<VERSION>` 标签会触发 GitHub Actions 工作流 `.github/workflows/release.yml`：

1. **构建固件**：使用 `espressif/esp-idf-ci-action@v1`（ESP-IDF v5.5.1，目标 `esp32s3`），生成 OTA bin 和 merged bin。
2. **构建 macOS**：在 macos-14 运行器上编译、签名、公证，产出 DMG、ZIP 和 Sparkle 签名文件。
3. **发布 GitHub Release**：合并固件和 macOS 产物并创建 Release。
4. **上传固件到阿里云 OSS**：OTA 镜像、merged 镜像和 `manifest.json` 同时上传到版本目录和 `latest/` 目录。
5. **触发网站部署**：自动运行 `deploy-website.yml`，更新 `appcast.xml`。

`deploy-website.yml` 还会读取当前 GitHub Release 的 macOS ZIP/签名与可选的 Windows MSI，调用 `scripts/update-appcast.py` 重写 `website/public/appcast.xml`。

Windows 特殊流程：
- Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建并签名。
- 将签名后的 MSI 上传到对应 GitHub Release。
- 手动运行 `Deploy Website to GitHub Pages` 工作流，使共享 appcast 收录 MSI 更新条目。

需要的 GitHub Secrets / Variables：
- `SPARKLE_PUBLIC_ED_KEY`、`SPARKLE_PRIVATE_ED_KEY`
- `MACOS_CERTIFICATE_P12`、`MACOS_CERTIFICATE_PASSWORD`
- `APPLE_ID`、`APPLE_TEAM_ID`、`APPLE_APP_SPECIFIC_PASSWORD`
- `ALIYUN_OSS_ACCESS_KEY_ID`、`ALIYUN_OSS_ACCESS_KEY_SECRET`
- `ALIYUN_OSS_ENDPOINT`、`ALIYUN_OSS_BUCKET`
- 仓库变量 `ALIYUN_OSS_PUBLIC_BASE_URL`、可选 `ALIYUN_OSS_PREFIX`

完整发布步骤见 `Doc/Ref/release.md`。

## 辅助脚本

`scripts/` 存放跨组件的构建与资源处理脚本；`scripts/ref/` 存放脚本的参考配置：

- `build-macos.sh` / `make-dmg.sh`：macOS 发布构建与 DMG 打包。
- `build-msi.bat`：Windows 签名 MSI 打包（WinSparkle 更新源）。
- `package-portable.bat`：构建 Windows 绿色便携版 ZIP 包，含预置 `config.toml` 模板和说明文件。
- `idf_cli.py`：ESP-IDF 编译/烧录/串口监控一体化脚本（`-c`/`-u`/`-s`/`-cus`，`-p COM17` 指定端口），Windows 上不便直接用 `idf.py` 时的便捷入口。配置文件为 `scripts/idf_cli.yaml`。
- `update-appcast.py`：根据 GitHub Release 更新 `website/public/appcast.xml`。
- `png_to_lvgl_argb_bin.py`：把 PNG 转成固件 LVGL 用的 ARGB 二进制资源。
- `slice_cat_sprites.py` / `tune_cat_sprites.py`：切片与调校状态界面精灵图。
- `probe_asr_websocket_ping.py`：探测 ASR WebSocket 连通性。
- `probe_wifi_provisioning.py`：探测/调试 Wi-Fi 配网与 LAN OTA 链路。

## 项目 Skills

本仓库在 `.agents/skills/` 下维护项目级 Skill，Agent 在相关场景会自动加载：

- `byted-web-search`：火山引擎豆包搜索，联网事实核查与信息检索场景优先使用。
- `sticks3-flash-ota`：M5Stack StickS3 固件烧录与 OTA 升级的标准流程；改完 `firmware/` 后需要把固件装到设备上验证时使用。
- `build-windows`：Windows 桌面端（VoiceStick.exe）构建与 CTest 流程；改完 `desktop/windows/` 下 C++ 源码后验证编译和测试。
- `build-firmware`：固件 ESP-IDF 构建流程；改完 `firmware/` 下 C 源码或 sdkconfig 后验证编译。

新增或修改 Skill 后，当前会话需要重启才能刷新可用技能列表。

## 安全与敏感信息

- **不要提交 API key**。`config.toml` 包含 `volcengine_api_key`、`voicestick_api_key`、`llm_api_key`、`tencent_secret_key` 等，均被 `.gitignore` 排除在版本控制外。
- macOS 应用请求蓝牙权限；文本注入使用模拟键盘事件，需授予辅助功能权限。
- Windows 应用使用 `SendInput` 进行粘贴，使用 WinHTTP 进行网络通信。
- 固件 OTA 更新时，桌面端会校验 `ota_size` 和 `ota_sha256` 后再写入设备。
- 发布产物（macOS DMG/ZIP、Windows MSI、固件 bin）均需签名或校验。
- Wi-Fi 凭据经 BLE 下发后持久化到 NVS；所有写日志路径必须把 `password` 字段脱敏为 `<redacted>`。

## 给 Agent 的重要提示

- 搜索仓库时请排除 `website/node_modules/` 和 `firmware/build/` 以免噪声过多。
- Windows 构建目录统一使用 `desktop/windows/build-x64`；旧的 `desktop/windows/build` 可能混入错误 VS/SDK 缓存，遇到链接异常时删除或忽略。
- `.gitignore` 整体忽略了 `desktop/windows/`，提交 Windows 端源码改动时必须用 `git add -f`，否则会被静默漏提交。
- `build_native.bat`、`do_build.bat`、`desktop\windows\build.bat` 包含本机绝对路径或固定版本号，复用前必须先检查内容。根目录 `test.bat` 目前只是占位脚本，不运行 CTest。
- 修改协议或公共数据结构时，必须同时更新 `Doc/Ref/protocol.md` 和所有实现端（固件 C、macOS Swift、Windows C++）。
- 修改网站 UI 文案时，必须同步更新 `website/src/i18n/zh-CN.json` 和 `website/src/i18n/en-US.json`。
- 修改 `VERSION` 时，必须同步更新 `firmware/version.txt`。
