# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code 在本仓库工作时提供指导，工作语言为简体中文。仓库根目录的 `AGENTS.md` 是本文的同源副本，面向通用 AI 编码助手；修改本文涉及的整体性内容时同步更新 `AGENTS.md`，避免两份文档漂移；以本文为权威源。

## 项目概览

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。设备负责采集按键、音频与 IMU 并通过 BLE 上报；桌面端负责交互状态机、ASR、文本显示与注入；网站负责落地页、浏览器端 USB 固件烧录（esptool-js）和 Sparkle/WinSparkle 更新源。

核心音频数据流：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
                                                              \-> Opus decode -> PCM -> 虚拟麦克风（微信输入法模式）
```

ASR 路径不把 Opus 解码回 PCM，ASR 与调试音频缓存都使用同一份 Ogg Opus 流。微信输入法模式是例外：桌面端把 Opus 解码为 PCM 后渲染到系统虚拟麦克风（如 VB-CABLE），供微信输入法等应用作为音频输入源。

当前版本：`2.2.0`（见仓库根目录 `VERSION`）。发布前需确保 `firmware/version.txt` 与 `VERSION` 一致（当前两者均为 `2.2.0`）。

## 关键配置文件

| 文件 | 用途 |
|---|---|
| `VERSION` | 单一版本来源，纯文本，不含换行 |
| `firmware/version.txt` | 固件向桌面端报告的版本，发布前必须与 `VERSION` 一致 |
| `firmware/CMakeLists.txt` | ESP-IDF 项目入口（`project(voice_stick)`） |
| `firmware/main/CMakeLists.txt` | 主组件注册与依赖声明 |
| `firmware/partitions_ota.csv` | 8 MB 分区表：两个 3 MB OTA app slot + 约 1984 KB `storage`（SPIFFS） |
| `desktop/macos/Package.swift` | SwiftPM 定义（swift-tools 5.9），依赖 Sparkle 2.6+、TOMLKit 0.6+、CZlib |
| `desktop/windows/CMakeLists.txt` | Windows 端构建，拆为 `voicestick_core` + `VoiceStickApp` + 两个测试目标 |
| `desktop/windows/src/version.h.in` | Windows 版本资源模板，由 CMake 从 `VERSION` 填充 |
| `desktop/windows/installer/VoiceStick.wxs` | WiX MSI 安装包定义 |
| `desktop/linux/` | Linux 桌面占位目录，目前无活跃实现 |
| `website/package.json` | Node 项目配置（仅 `dev`/`build`/`preview` 脚本，无 lint/test） |
| `website/public/appcast.xml` | Sparkle/WinSparkle 更新源 |
| `.github/workflows/release.yml` | 推送 `v*` 标签触发构建与发布 |
| `.github/workflows/deploy-website.yml` | 网站部署与 appcast 更新 |
| `scripts/idf_cli.yaml` | `idf_cli.py` 的配置文件 |
| `requirements.txt` | Python 脚本依赖（`pyyaml` / `pyserial` / `Pillow`），不含 E2E 工具链依赖 |
| `ArduFlux.json` | 本机 ArduFlux 工具的 ESP32-S3 板卡/串口配置（辅助烧录，非构建必需） |

## 技术栈

| 模块 | 语言/框架 | 构建工具 | 目标平台 |
|---|---|---|---|
| 固件 | C (ESP-IDF v5.5.1) | `idf.py` | ESP32-S3 (M5Stack StickS3) |
| macOS 桌面端 | Swift 5.9, AppKit, CoreBluetooth | SwiftPM | macOS 12+ |
| Windows 桌面端 | C++20, Win32, C++/WinRT, Direct2D | CMake + Ninja + MSVC 2022 x64 | Windows 10 1903+ |
| 网站 | Vue 3, Vite, vue-i18n, esptool-js | Vite | 静态站点 (GitHub Pages) |

固件关键外部依赖：`espressif/button` ^4.1.6（main）、`espressif/esp_codec_dev` ^1.3.4 + `78/esp-opus` ^1.0.5（audio_pipeline）、`lvgl/lvgl` 9.2.0（ui_status），由 ESP-IDF component manager 通过各组件自己的 `idf_component.yml` 管理。

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

`voicestick_windows_tests` 基于 `assert`，目前不支持按测试函数名过滤；新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。`-R voicestick_integration_tests` 单独跑集成测试，需联网与 `volcengine_api_key`，无 key 时该测试返回 77 被 CTest 标记为 SKIP。

运行应用：

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI）：

```bat
scripts\build-msi.bat
```

注意：`build_native.bat`、`do_build.bat`、`desktop\windows\build.bat` 包含本机绝对路径或固定版本号，复用前必须先检查内容；根目录 `test.bat` 目前只是占位脚本，不运行 CTest。仓库根目录散落的 `*.log` 与 `%BUILD_LOG%` 等文件是历次本地构建的残留日志，不是源码。

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

交互状态机在桌面端，不在固件中。修改交互流程时优先改桌面协调器（macOS 的 `VoiceStickCoordinator` / Windows 的 `voice_stick_coordinator.cc`）。固件通常只在新增/调整 `ui_state` 展示、硬件 I/O、BLE 协议或 OTA 行为时修改（例外：双击手势的时序检测在固件端完成，桌面端只响应 `button_double_click` 事件，见 `Doc/Plan/primary-button-double-click.md`）。修改协议字段、状态枚举、配置项或发布产物格式时，同步检查 `Doc/Ref/`、macOS、Windows、网站和发布脚本。

### BLE 协议边界

GATT service UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| 特征 | UUID | 方向 | 属性 | 用途 |
|---|---|---|---|---|
| `audio_tx` | `…5101` | 设备 → 主机 | notify | Opus 音频帧 |
| `state_tx` | `…5102` | 设备 → 主机 | notify | 按键事件（含 `button_double_click`）、电量、固件版本、体感鼠标运动帧 |
| `control_rx` | `…5103` | 主机 → 设备 | write without response | `ui_state`、交互/敲击/体感鼠标设置、`ota_commit` 等；JSON 需控制长度避免 BLE MTU 溢出 |
| `ota_rx` | `…5104` | 主机 → 设备 | write / write without response | BLE OTA 控制与数据帧 |
| `ota_tx` | `…5105` | 设备 → 主机 | notify | BLE OTA 状态帧 |

完整帧格式见 `Doc/Ref/protocol.md`。v1.8.0 起 Wi-Fi STA 配网与局域网 OTA 已整体移除，固件升级只走 BLE OTA（串口烧录为回退手段）。

### 固件职责

固件只负责硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态，不持有桌面交互状态机。关键组件（`firmware/components/` 下共 6 个）：

- `firmware/main/main.c`：主循环，编排按键、BLE、录音会话、UI 状态、电源管理和 OTA 事件。
- `components/audio_pipeline/`：从 ES8311 读取 16 kHz 单声道 PCM，经 HPF 与软件 AGC（v2.1.2 起：target -6 dBFS、max +20 dB、噪声门、0.8FS 瞬时限幅，硬件 ALC 已关闭）后编码为 Opus 交给 BLE 层；开头 60ms 静音+淡入、drain 尾帧淡出以消除按键音。
- `components/voice_ble/`：GATT 服务、通知、控制写入、BLE OTA。
- `components/ui_status/`：ST7789/LVGL 渲染、亮度、休眠、OTA 进度。
- `components/bmi270/`：BMI270 IMU 驱动。
- `components/mini_encoder_c/`：MiniEncoderC 编码器驱动（I2C @0x42，顶部 Hat 排针 SDA=G8/SCL=G0 第二路总线，按钮/旋转增量/SK6812 LED，轮询式；探测失败优雅降级）。
- `components/stick_s3_board/`：板级初始化，引脚定义在 `include/stick_s3_board.h`。

板级硬件映射：

| 硬件 | 引脚/接口 | 说明 |
|---|---|---|
| 主键（正面） | GPIO11 | 协议 `primary`，push-to-talk 与深度睡眠唤醒 |
| 侧键 | GPIO12 | 协议 `secondary`，取消/体感鼠标/恢复上一次输入确认 |
| PMIC IRQ | GPIO13 | 电源管理芯片中断 |
| LCD 背光 | GPIO38 | PWM 调光 |
| IMU | BMI270 | I2C，体感鼠标与敲击检测 |
| MiniEncoderC 编码器 | I2C @0x42，顶部 Hat 排针 SDA=G8 / SCL=G0（第二路 I2C 总线） | 按钮等价主键，旋转映射方向键，录音时亮红灯；不能作为深睡唤醒源 |
| 音频 codec | ES8311 | I2S，16 kHz / 16 bit / mono |
| 显示屏 | ST7789 | 135 × 240 竖屏，SPI |

### 桌面端职责

桌面端是状态唯一可信源，负责 BLE 配对和多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译与精修、悬浮窗/字幕、文本注入、体感鼠标、配置管理和自动更新。

macOS 代码集中在 `desktop/macos/Sources/VoiceStickApp/`：`VoiceStickCoordinator`（状态机）、`BleCentral` / `BleProtocol`（CoreBluetooth 与协议）、`OggOpusMuxer` / `ASRWebSocketClient`（音频封装与 ASR）、`InputInjector`（粘贴与 Return 注入）、`OverlayController` / `SubtitleController` / `StatusController`（悬浮窗/字幕/状态）、`FirmwareManifest` / `FirmwareUpdateWindowController`（固件更新）。

Windows 端在 `desktop/windows/CMakeLists.txt` 中拆成四个源码目标（另有 `winsparkle_lib` 导入库）：

- `voicestick_core`：可测试核心库，包含配置解析、BLE 协议、Ogg Opus mux、ASR 帧格式、LLM 翻译/精修（含热词 few-shot 与改坏回退守卫）、热词候选挖掘（`hotword_candidate_miner`）、调试音频缓存、固件清单解析、日志、本地化和协调器状态机。
- `VoiceStickApp`：Win32 平台外壳，包含托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、配对/设置/固件更新等对话框。
- `voicestick_windows_tests`：基于 `assert` 的单元测试，源码在 `desktop/windows/tests/core_tests.cc`，用自定义 Fake/Mock 不联网验证核心库；由 CTest 注册为同名测试，不支持按测试函数名过滤。
- `voicestick_integration_tests`：L1 ASR 链路集成测试，源码在 `desktop/windows/tests/integration_tests.cc`，用真实 `AsrClientWin` 连火山 ASR（需 `%APPDATA%\VoiceStick\config.toml` 配 `volcengine_api_key` 且联网），无 key 时返回 77 被 CTest 标记为 SKIP，不伪造结果。

新增核心行为优先放入 `voicestick_core`，并在 `desktop/windows/tests/core_tests.cc` 覆盖；跨链路验证可补到 `integration_tests.cc`。

### 核心交互模型

固件上报原始按键事实，桌面端解释为交互行为并回写 `ui_state`：

| 状态 | 主键（正面） | 侧键 |
|---|---|---|
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音（双击直接注入 Enter，不录音） | 双击恢复上一次输入确认；单击无操作（体感鼠标入口已移除） |
| 录音中 | 释放结束录音 | 单击不取消当前录音 |
| 识别中 | 忽略新录音 | 单击取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 单击取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 单击取消待粘贴文本 |

补充说明：

- 双击检测在固件端完成（阈值见 `Doc/Plan/primary-button-double-click.md`），桌面端统一处理 `button_double_click`：主键双击取消当前活跃录音/字幕后注入 Enter；侧键双击执行「恢复上一次输入确认」（体感鼠标态下忽略）。
- 侧键单击的取消语义仅在有活跃录音/识别/待粘贴时生效；体感鼠标态下单击退出体感；真正空闲时单击无操作（体感入口已移除，`ToggleAirMouse` 不再由侧键触发）。
- 另有敲击检测（`tap_to_arrow` 配置，IMU 敲击映射为方向键），体感态下忽略。

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式。文本输出支持三种目标：`focused_app`（默认粘贴到当前焦点，默认自动按 Return）、`subtitle`（仅显示字幕）和 `wechat_input_method`（把 Opus 解码为 PCM 渲染到系统虚拟麦克风，供微信输入法等应用作为音频输入源，不经 ASR 文本注入）。识别结果可通过 OpenAI-compatible LLM 做翻译与精修，也可按设备单独覆盖输出设置。

## 配置

桌面端运行时配置文件位置：

- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows：`%APPDATA%\VoiceStick\config.toml`

关键配置项（完整字段见 `README.md`）：

- `asr_provider`：ASR 提供商，可选 `volcengine`、`voicestick_cloud` 或 `tencent`（腾讯为 v1.8.2 新增）。
- `volcengine_api_key` / `voicestick_api_key` / `voicestick_cloud_url`：火山直连密钥，或 VoiceStick Cloud 中转密钥与 WebSocket URL。
- `volcengine_boosting_table_id` / `volcengine_correct_table_id`：火山自学习平台热词表/替换词表 ID（控制台创建），作为 `corpus.boosting_table_id` / `corpus.correct_table_id` 发送；背景见 `Doc/Ref/volcengine-asr.md`（corpus 热词直传只在流式第一遍生效，二遍最终文本不吃直传，精修 prompt 会附加热词表由 LLM 兜底纠正）。
- `tencent_secret_id` / `tencent_secret_key` / `tencent_appid`：腾讯云 ASR 凭据（加载时自动 Trim 去前后空格）。
- `llm_base_url` / `llm_api_key` / `llm_model`：OpenAI 兼容 LLM，用于翻译与精修；`refine_enabled` 默认 `true`。
- `hotword_process_enabled` / `hotword_process_prompt`：热词处理（Windows），划词加词时用 LLM 提炼热词，复用 `llm_*` 连接配置；默认关闭。
- `hotword_mining_enabled`：热词候选挖掘（Windows，默认关闭）。两条挖掘通道共用计数存储：①精修 diff 挖掘（精修纠回不在表标识符时计数，无开关）；②LLM 主动提炼（本开关打开时，每会话完成后异步让 LLM 从最终文本提炼候选）。同一词达 3 次（`kHotwordCandidateThreshold`）弹托盘通知并在设置-热词区给出「加入/忽略」候选；计数存 `%APPDATA%\VoiceStick\hotword_candidates.json`。明确不做全自动入表，原因见 `Doc/Expe/hotword-two-pass-and-candidate-mining-2026-07-28.md`。
- `interaction_mode`：`hold_to_talk`（默认）或 `click_to_talk`，控制 focused_app/字幕模式的触发方式（托盘菜单可切）。wechat 模式的触发方式由 `[wechat_input_method].trigger_mode` 独立控制，不联动全局 `interaction_mode`。
- `paired_device_ids`：已配对设备 4 位十六进制 ID 列表，如 `C3D8,09AF`。
- `[output].target`：`focused_app`（默认）、`subtitle` 或 `wechat_input_method`；`[output].transform`：`original` 或 `translate`；可用 `[device.<id>.output]` 按设备覆盖。
- `[wechat_input_method]`：微信输入法模式专属配置，含 `trigger_mode`（wechat 专属触发方式，`hold_to_talk` 默认或 `click_to_talk`，与全局 `interaction_mode` 解耦）、`hotkey_hold` / `hotkey_click`（长按式/点按式各自记忆的触发热键，默认 `ctrl+win` / `ralt`）、`virtual_mic_playback_name` / `virtual_mic_capture_name`（虚拟麦克风播放/采集端设备名，通常对应 VB-CABLE 两端）、`auto_switch_default_recording_device`（录音期自动把系统默认录音设备切到虚拟麦克风采集端，松开切回）。
- `tap_to_arrow`：IMU 敲击映射方向键开关。
- `encoder_to_arrow` / `encoder_rotation_invert` / `encoder_rotate_cw_key` / `encoder_rotate_ccw_key`：MiniEncoderC 编码器旋转注入开关（默认 `true`）、方向翻转（默认 `false`，true 时顺时针→Up）与 cw/ccw 自定义按键（热键语法）。
- `encoder_rotate_fast_threshold` / `encoder_rotate_cw_fast_key` / `encoder_rotate_ccw_fast_key`：旋转快慢分档——固件 10ms 窗口计数的单窗口格速（steps × 100 格/秒）量化到 100 格/秒，直接比较会让 100~200 间阈值失效且偶发 2 步窗误判快，故桌面端对单窗口格速做 EWMA 平滑（α=0.5 按事件更新，与墙钟无关，新手势静默 >250ms 后从零冷启动，见 `desktop/windows/src/encoder_speed.h`），平滑估计 ≥ 阈值（默认 200）判为快速手势，改注快速档按键（默认 cw=`pagedown` / ccw=`pageup`，慢速逐行、快速翻页）；一次快速手势只注入一次并进入停转锁定，锁定期间屏蔽所有旋转输出（含减速段慢速事件与换向事件），直到静默 >250ms 判定停稳才恢复识别；快速档按键非法时回退普通按键。设置对话框中阈值为滑杆控件（范围 100–300，超出范围的配置值显示时钳制）。
- `encoder_rotate_decide_window_ms`：慢速注入延迟判定窗（默认 80ms，0 = 关闭延迟判定即立即注入）。慢速事件先挂起累计，窗内判快则整段丢弃（消除快甩加速段的误注入），到期由 30ms 定时器驱动的 `EncoderRotateTick()` 冲刷补注；慢转因此有 ≤80ms 注入延迟，连续慢转按窗成批注入、总量不变。仅 config.toml 高级项，不进设置对话框。
- `encoder_led_color`：编码器录音灯颜色（red/green/blue/yellow/purple/cyan/white/off），BLE 下发固件 NVS 持久化。
- `encoder_press_action` / `encoder_press_key` / `encoder_double_click_action` / `encoder_double_click_key`：编码器单击/双击动作（recording|key）与自定义按键；`press_action=key` 派生固件录音门控关闭，双击 recording 走 remote_button 切换起停。
以上编码器设置项仅 Windows 端消费。
- `air_mouse_*`：体感鼠标参数（`air_mouse_sensitivity_x/y`、`air_mouse_tau`、`air_mouse_invert_y`、`air_mouse_curve_*`、`air_mouse_control_mode`、`air_mouse_rate_*` 等），完整字段见 `desktop/macos/Config/config.example.toml` 与 `desktop/windows/src/app_config.cc`。

Windows MSI 还会把 `config.template.toml` 装到 `%ProgramFiles%\VoiceStick\` 下，首启复制到 `%APPDATA%`（升级不覆盖）。示例见 `desktop/macos/Config/config.example.toml`。

## 代码风格

- **Swift（macOS）**：标准 Swift/AppKit 命名。
- **C++（Windows）**：Google C++ 风格：`snake_case` 文件名和变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **C（固件）**：ESP-IDF 风格，组件通过 `idf_component_register` 注册，组件间通过 `REQUIRES` 声明依赖。
- **Vue/JS（网站）**：Vue 3 Composition API 风格。

仓库当前未提交统一 lint/formatter 配置；不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令。修改对应组件后运行该组件已有的构建/测试命令作为验证。

## 测试策略

- **Windows**：`desktop/windows/tests/core_tests.cc` 使用自定义 Fake/Mock 对 `voicestick_core` 中的状态机、配置解析、协议编解码、Ogg Opus mux 等进行单元测试（不联网）。`desktop/windows/tests/integration_tests.cc` 是 L1 ASR 链路集成测试，连真实火山 ASR，无 key 时返回 77 被 CTest 标记为 SKIP。运行命令：`ctest --test-dir desktop/windows/build-x64 --output-on-failure`。
- **macOS**：目前没有专用测试目标。验证方式主要是 `swift build` 编译通过和运行时手动测试。
- **固件**：没有自动化单元测试。验证方式是 `idf.py build` 编译通过和真机运行时测试。
- **网站**：没有自动化测试。验证方式是 `npm run build` 构建通过。
- **Python E2E 真机验证**：`scripts/e2e_test/` 是跨固件+Windows 端到端的半自动验证工具链（L0–L4），用真实 BLE 连接与真实 ASR/音频链路，不伪造结果。
  - L0 语料：`gen_corpus.py` / `verify_corpus.py` / `build_spiffs_image.py` 生成测试 PCM 语料并打包成 SPIFFS 镜像刷入固件。
  - L3 固件回放：`run_l3_firmware.py` 用独立 bleak BLE 连接（VoiceStickApp 必须先断开，StickS3 BLE 独占单连接），下发 `test_playback` 回放 PCM 驱动录音，订阅 `audio_tx` 收 Opus 帧统计首帧延迟与帧数，配合串口日志 `playback set` 确认回放生效。
  - L4 微信输入法：`run_l4_wechat.py` + `loopback_capture.py` 用 WASAPI 抓取 CABLE Output PCM，验证 Opus 解码->渲染->CABLE->微信识别链路（半自动，需人工按设备键说话并确认结果）。
  - 辅助：`scan_ble.py`（BLE 扫描）、`read_serial.py`（串口日志读取）、`replay_tencent_asr.py`（腾讯 ASR 回放）、`spectrogram_server.py`（调试音频频谱分析页，v2.1.2 新增）。
  - 依赖 `bleak` / `numpy` / `sounddevice`，**未列入根目录 `requirements.txt`**（该文件只含 `pyyaml` / `pyserial` / `Pillow`），运行前需另行 `pip install`。
  - 设计文档见 `Doc/Plan/windows-e2e-test-plan.md` 与 `Doc/Plan/windows-e2e-next-steps.md`。

## 安全注意事项

- API 密钥等凭据字段（`volcengine_api_key`、`tencent_secret_*`、`llm_api_key` 等）只存在于本机 `config.toml`，不要提交进仓库；示例配置使用占位符。
- Windows 便携包模板中使用占位符而非真实 Sparkle 公钥；真实签名证书与 Sparkle 私钥只存在于签名机。
- 集成测试与 E2E 工具链坚持「不伪造结果」原则：无凭据/无设备时 SKIP 或报错，不要为了让测试变绿而 mock 掉真实链路。
- 固件 OTA 与桌面端自动更新走官方渠道（GitHub Release + 阿里云 OSS + appcast），不要绕过签名校验逻辑。

## 发布流程

推送与 `VERSION` 匹配的 `v<版本号>` 标签会触发 `.github/workflows/release.yml`：

1. 构建固件（ESP-IDF v5.5.1，目标 `esp32s3`），生成 OTA bin、merged bin 与 `manifest.json`。
2. 构建并签名 macOS 产物（DMG、ZIP、Sparkle 签名）。
3. 创建 GitHub Release，合并固件与 macOS 产物。
4. 上传固件到阿里云 OSS 的版本目录和 `latest/` 目录。
5. 触发 `deploy-website.yml` 更新 `website/public/appcast.xml`。

Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建并签名，然后上传到对应 GitHub Release，再手动运行 `Deploy Website to GitHub Pages` 工作流以收录 MSI 条目。完整步骤见 `Doc/Ref/release.md`。

Windows 便携版（免安装 zip）用 `scripts\package-portable.ps1` 打包（PowerShell 脚本，用 .NET 写 UTF-8 文件规避 cmd 中文 `echo` 块在 GBK 代码页下的解析错位；脚本须存为 UTF-8 with BOM）；本机无签名证书时可用 `scripts\build-msi-unsigned.bat` 构建未签名 MSI 验证安装流程。打包产物放在 `dist/`（已被视为本地产物目录）。

`CHANGELOG.md` 是版本变更记录（最新发布条目为 v2.2.0）。发布新版本时应同步追加条目；注意该文件可能滞后于 `VERSION`（中间版本 v2.0.0/v2.1.0 条目缺失，条目从 v1.9.0 直接跳到 v2.1.1），以 `VERSION`（当前 `2.2.0`）为准。

## 项目 Skills

本仓库在 `.agents/skills/` 下维护项目级 Skill，相关场景会自动加载：

- `byted-web-search`：火山引擎豆包搜索，联网事实核查与信息检索场景优先使用。
- `sticks3-flash-ota`：M5Stack StickS3 固件烧录与升级流程；改完 `firmware/` 后需要把固件装到设备上验证时使用。
- `build-windows`：Windows 桌面端构建与 CTest 流程；改完 `desktop/windows/` 后验证编译和测试。
- `build-firmware`：固件 ESP-IDF 构建流程；改完 `firmware/` 后验证编译。
- `usb-jtag-flash-log`：ESP32-S3 USB JTAG 烧录与运行时串口日志采集；烧录后读不到日志、DTR 软复位、串口监控等场景使用，是 `sticks3-flash-ota` 路径 B 的深化补充。

仓库根目录另有一个 `skills/` 目录，存放 `byted-web-search` 与 `sticks3-flash-ota` 两个 Skill 的另一份拷贝（`skills-lock.json` 只记录 `byted-web-search` 的来源与哈希）；以 `.agents/skills/` 为准。新增或修改 Skill 后，当前会话需要重启才能刷新可用技能列表。

## 经验教训记忆

`Doc/Expe/claude-memory-distilled.md` 是从本仓库 Claude Code 约 80 条项目记忆蒸馏的长期参考（2026-07-17），按域分 9 章：固件/音频链路、烧录与串口日志、Windows 桌面端、微信输入法模式、交互/体感、测试方法论、排查方法论、遗留待办（第 8 章，含体感标定/深睡验证/VB-CABLE 授权/E2E next-steps 等待真机项）、过时条目。排查问题或改动相关模块前先按章节查阅；其中寄存器值、阈值、文件:行号均为记录时点结论，引用前以当前源码为准。

高频速查：

- 桌面端日志在 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（非 Roaming 的 `%APPDATA%`）。
- 任何「按住开始/松开结束」的音频会话，固件 `*_stop` 必须同步等 drain 完成再返回，否则 button_up 抢跑丢尾音。
- SendInput 注入第三方输入法热键必须带 scan code（`MapVirtualKey`），仅 wVk 时输入法不响应。
- BLE 音频拥堵根治组合（禁 Wi-Fi + 7.5ms interval + MSYS1 扩到 200 块）勿回退。
- ES8311 ALC 寄存器位域以 Linux 主线 `es8311.h` 为准，不信 `es8311_reg.h` 注释。
- 设备 EN 复位后 USB JTAG 重枚举，间隙内 boot 日志直接丢失，循环重开 pyserial 也盖不住；boot→广播耗时从主机日志反推（断连事件→`advertisement matched` 时间差），不要试图串口抓 boot。
- 排查 BLE 回连/僵尸链路先看 `Doc/Expe/ble-zombie-link-reboot-reconnect.md`；`link-layer connected` 的 `polls=0` 是典型僵尸，`polls=1` 也可能是垂死链路（ATT 挂起），勿凭 polls 单一判据下结论。
- 设备卡 Pairing 且重启 stick 无效、重启 Windows 端立愈 = 广告 watcher 静默失效，判据是日志长时间零 `advertisement matched`，见 `Doc/Expe/ble-watcher-silent-death-pairing-stuck.md`；应用自己的 radio reset 也会杀死 watcher，之后必须重建扫描。
- 旁路（非协调器状态机）要碰 overlay 先查 `coordinator_->HasActiveSession()`：会话活跃时反馈必须走托盘气泡，否则 `kAutoHideTimerId`/`pending_callback_` 共享资源被覆盖会踩掉确认倒计时的自动粘贴，见 `Doc/Expe/hotword-processing-implementation-2026-07-28.md`。
- 提升权限运行的 VoiceStick.exe 会锁定链接产物且 `build_win.bat` 杀不掉仍报成功，判据是 exe 时间戳；`ctest` 不在裸 cmd PATH，用 VS BuildTools 全路径。

## 给 Agent 的提示

- Windows 桌面端修改完成并构建通过后，自动重启 VoiceStick.exe 让改动生效（先结束运行中的进程避免锁定链接产物，启动后检查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 确认正常），无需用户另行指示。

- `.gitignore` 整体忽略了 `desktop/windows/`，提交 Windows 端源码改动时必须用 `git add -f`，否则会被静默漏提交。
- Windows 构建目录统一使用 `desktop/windows/build-x64`；旧的 `desktop/windows/build` 可能混入错误 VS/SDK 缓存，遇到链接异常时删除或忽略。
- 搜索仓库时请排除 `website/node_modules/` 和 `firmware/build/` 以免噪声过多。
- 修改网站 UI 文案时，必须同步更新 `website/src/i18n/zh-CN.json` 和 `website/src/i18n/en-US.json`。
- 修改根目录 `README.md` 时，必须同步更新 `README.zh-CN.md`。
- 修改 `VERSION` 时，必须同步更新 `firmware/version.txt`。
- 修改协议或公共数据结构时，必须同时更新 `Doc/Ref/protocol.md` 和所有实现端（固件 C、macOS Swift、Windows C++）。
- `Doc/` 下分四个子目录：`Ref/`（协议、发布流程、ASR 帧格式、低功耗等参考，另有 `OpenViking.md`）、`Plan/`（设计方案，大写 P，不再用 `Doc/Rfc/`）、`Guide/`（火山/腾讯 ASR WebSocket 接入、API 概览、air-mouse 调参等第三方服务接入指南）、`Expe/`（经验教训记录）。
- `scripts/` 下除各平台构建脚本外，还有 `probe_asr_websocket_ping.py`（ASR 连通性探测）、`probe_hotword_extraction.py`（离线探测 LLM 热词提炼链路，打印原始响应与候选过滤原因）、`update-appcast.py`（生成 `appcast.xml`）、`idf_cli.py`（Windows 上包装 `idf.py`）、`png_to_lvgl_argb_bin.py` / `slice_cat_sprites.py` / `tune_cat_sprites.py`（LVGL 图片资源处理）、`scripts/e2e_test/`（L0–L4 真机验证，见上节测试策略）等辅助脚本。
- MiniEncoderC 编码器键是 I2C 外设，不能作为深睡唤醒源；主键（GPIO11）仍是唯一唤醒键。
- Grove 口 5V 保持不启用（固件不动 PMIC BOOST_EN），MiniEncoderC 由顶部 Hat 排针供电。
