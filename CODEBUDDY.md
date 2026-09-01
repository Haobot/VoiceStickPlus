# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

本文件以根目录 `CLAUDE.md` / `AGENTS.md`（互为同源副本，`CLAUDE.md` 为权威源）为内容来源。修改本文件涉及的整体性内容时，请同步检查 `CLAUDE.md` / `AGENTS.md` 是否需要更新，避免三份文档漂移。

## 项目概览

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。设备负责采集按键、音频与 IMU 并通过 BLE 上报；**桌面端是状态唯一可信源**，负责交互状态机、ASR、文本显示与注入；网站负责落地页、浏览器端 USB 固件烧录（esptool-js）和 Sparkle/WinSparkle 更新源。

输入设备现有两类并存：自研 StickS3（`VS-XXXX`，配固件）与小米蓝牙遥控器 2 Pro（`RC-XXXX`，仅 Windows 端，固件零改动，ATVV 协议接入全部在桌面端；设计见 `Doc/Plan/xiaomi-remote-2-pro-support.md`，macOS 端暂缓待后续版本）。

核心音频数据流：

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
                                                              \-> Opus decode -> PCM -> 虚拟麦克风（微信输入法模式）
```

ASR 路径不把 Opus 解码回 PCM，ASR 与调试音频缓存共用同一份 Ogg Opus 流。微信输入法模式是例外：桌面端把 Opus 解码为 PCM 渲染到系统虚拟麦克风（如 VB-CABLE），不经 ASR 文本注入。

小米遥控器的音频在进协调器前归一化：ATVV BLE 裸 IMA ADPCM → 桌面端解码 PCM（平滑 + 增益限幅）→ 桌面端 Opus 编码，产出与 StickS3 相同的标准音频帧，下游零改动。

当前版本见根目录 `VERSION`（纯文本，无换行）；发布前必须确保 `firmware/version.txt` 与之一致。

## 技术栈与关键依赖

| 模块 | 语言/框架 | 构建工具 | 目标平台 |
|---|---|---|---|
| 固件 | C (ESP-IDF v5.5.1) | `idf.py` | ESP32-S3 (M5Stack StickS3) |
| macOS 桌面端 | Swift 5.9, AppKit, CoreBluetooth | SwiftPM（`desktop/macos/Package.swift`，依赖 Sparkle 2.6+/TOMLKit/CZlib） | macOS 12+ |
| Windows 桌面端 | C++20, Win32, C++/WinRT, Direct2D | CMake + Ninja + MSVC 2022 x64 | Windows 10 1903+ |
| 网站 | Vue 3, Vite, vue-i18n, esptool-js | Vite（Node 22） | 静态站点 (GitHub Pages) |

固件外部依赖由 ESP-IDF component manager 经各组件 `idf_component.yml` 管理：`espressif/button` ^4.1.6、`espressif/esp_codec_dev` ^1.3.4 + `78/esp-opus` ^1.0.5、`lvgl/lvgl` 9.2.0。

## 构建与测试命令

### 固件（ESP-IDF v5.5.1，目标 `esp32s3`）

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

首次从旧单应用分区表升级需擦除重刷：`idf.py -p <port> erase-flash flash monitor`。

Windows 上：`python scripts/idf_cli.py -cus -p COM17`（`-c` 编译、`-u` 上传、`-s` 监控、`-cus` 全做）。固件无自动化单元测试，验证方式为 `idf.py build` 编译通过 + 真机运行时测试。

### macOS 桌面端（SwiftPM）

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

发布构建（仓库根目录）：`SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release && scripts/make-dmg.sh`。macOS 无测试目标，验证方式为 `swift build` 编译通过 + 手动运行测试。

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

推荐从仓库根目录执行 `build_win.bat`（自动找 VS 2022、结束残留进程、重建 `desktop\windows\build-x64`，只构建不跑 CTest）。注意：该脚本历史上出现过链接失败仍报成功的情况，构建后应核对 `desktop\windows\build-x64\VoiceStick.exe` 的时间戳与体积。

手动构建（需先进入 VS 2022 x64 开发者环境）：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
```

测试：

```powershell
# 全部测试
ctest --test-dir desktop\windows\build-x64 --output-on-failure
# 单元测试（基于 assert，不支持按测试函数名过滤）
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
# 集成测试（需联网 + volcengine_api_key；无 key 返回 77 被标记 SKIP）
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_integration_tests
```

新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。

注意：`build_native.bat`、`do_build.bat`、`desktop\windows\build.bat` 含本机绝对路径或固定版本号，复用前先检查内容；根目录 `test.bat` 只是占位脚本，不运行 CTest；根目录散落的 `*.log` 是本地构建残留，不是源码。Windows 构建目录统一用 `desktop/windows/build-x64`，旧的 `desktop/windows/build` 可能混入错误 VS/SDK 缓存。

发布打包：`scripts\build-msi.bat`（签名 MSI，内部先用 `scripts\prepare_flash_payload.ps1` 准备 VoiceStickFlash 的 esptool payload 并生成 `flash_payload.wxs` 片段）；便携版 zip 用 `scripts\package-portable.ps1`（脚本须存为 UTF-8 with BOM）；无签名证书时用 `scripts\build-msi-unsigned.bat` 验证安装流程。产物放 `dist/`。

### 网站（Vue 3 + Vite）

```sh
cd website
npm install
npm run dev      # 本地开发
npm run build    # 最小验证（无 lint/test 脚本）
npm run preview
```

## 架构边界（重点）

### 状态机归属

交互状态机在**桌面端**，不在固件。修改交互流程优先改桌面协调器（macOS `VoiceStickCoordinator` / Windows `voice_stick_coordinator.cc`）。固件通常只在新增/调整 `ui_state` 展示、硬件 I/O、BLE 协议或 OTA 行为时修改。例外：**双击手势的时序检测在固件端完成**，桌面端只响应 `button_double_click` 事件（见 `Doc/Plan/primary-button-double-click.md`；小米遥控器再例外：语音键双击时序检测在桌面端适配层完成，合成同样的 `button_double_click`）。

修改协议字段、状态枚举、配置项或发布产物格式时，同步检查 `Doc/Ref/`、macOS、Windows、网站和发布脚本。

### BLE 协议

GATT service UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| 特征 | UUID | 方向 | 用途 |
|---|---|---|---|
| `audio_tx` | `…5101` | 设备→主机 notify | Opus 音频帧 |
| `state_tx` | `…5102` | 设备→主机 notify | 按键事件（含 `button_double_click`）、电量、版本、体感帧 |
| `control_rx` | `…5103` | 主机→设备 write w/o resp | `ui_state`、交互/敲击/体感设置、`ota_commit` 等；JSON 需控制长度避免 MTU 溢出 |
| `ota_rx` | `…5104` | 主机→设备 write | BLE OTA 控制与数据帧 |
| `ota_tx` | `…5105` | 设备→主机 notify | BLE OTA 状态帧 |

完整帧格式见 `Doc/Ref/protocol.md`。v1.8.0 起 Wi-Fi 配网与局域网 OTA 已移除，固件升级只走 BLE OTA（串口烧录为回退）。

小米蓝牙遥控器 2 Pro 不走本 service：讲 Google ATVV profile（`AB5E0001-5A21-4F05-BC7D-AF01F617B664`）+ 标准 HID/Battery（`0x1812`/`0x180F`），桌面端按 `RC-XXXX` 建档连接，固件不实现该协议；协议事实见 `Doc/Ref/protocol.md` ATVV 设备档案章节。

### 固件职责（`firmware/components/` 共 7 个组件）

固件只做硬件 I/O、音频编码、BLE 通信、电源管理和显示主机下发的 UI 状态：

- `firmware/main/main.c`：主循环，编排按键、BLE、录音会话、UI 状态、电源管理、OTA。
- `components/audio_pipeline/`：ES8311 读 16 kHz 单声道 PCM → HPF + 软件 AGC（target -6 dBFS、max +20 dB、噪声门、瞬时限幅，硬件 ALC 已关闭）→ Opus；开头 60ms 静音+淡入、drain 尾帧淡出消除按键音。
- `components/voice_ble/`：GATT 服务、通知、控制写入、BLE OTA。
- `components/ui_status/`：ST7789/LVGL 渲染、亮度、休眠、OTA 进度。
- `components/bmi270/`：BMI270 IMU 驱动。
- `components/mini_encoder_c/`：MiniEncoderC 编码器（I2C @0x42，顶部 Hat 排针 SDA=G8/SCL=G0 第二路总线，轮询式；探测失败优雅降级；**不能作为深睡唤醒源**）。
- `components/stick_s3_board/`：板级初始化，引脚定义在 `include/stick_s3_board.h`。
- `components/power_log/`：分模式功耗记账（纯观察组件），经 `control_rx`/`state_tx` 的 `power_log` 命令导出。

板级硬件映射：主键 GPIO11（协议 `primary`，push-to-talk + 深睡唤醒，唯一唤醒键）、侧键 GPIO12（协议 `secondary`）、PMIC IRQ GPIO13、LCD 背光 GPIO38 PWM、ES8311（I2S，16 kHz/16bit/mono）、ST7789（135×240 竖屏 SPI）、BMI270（I2C）。Grove 口 5V 不启用。

### 桌面端职责

桌面端负责 BLE 配对与多设备连接、交互状态机、Opus→Ogg Opus 封装、ASR WebSocket、LLM 翻译与精修、悬浮窗/字幕、文本注入、体感鼠标、配置管理、自动更新。

- **macOS**（`desktop/macos/Sources/VoiceStickApp/`）：`VoiceStickCoordinator`（状态机）、`BleCentral`/`BleProtocol`（CoreBluetooth 与协议）、`OggOpusMuxer`/`ASRWebSocketClient`（音频封装与 ASR）、`InputInjector`（粘贴与 Return 注入）、`OverlayController`/`SubtitleController`/`StatusController`、`FirmwareManifest`/`FirmwareUpdateWindowController`。
- **Windows**（`desktop/windows/CMakeLists.txt` 五个源码目标）：
  - `voicestick_core`：可测试核心库（配置解析、BLE 协议含 `DeviceClass` 设备类与 `VS-`/`RC-` 双前缀、Ogg Opus mux、ASR 帧格式、LLM 翻译/精修、热词候选挖掘、调试音频缓存、固件清单、日志、本地化、协调器状态机、VoiceStickFlash 烧录逻辑、小米遥控器接入 `xiaomi_atvv_protocol`/`xiaomi_atvv_session`/`ima_adpcm_decoder`/`pcm_postprocessor`/`audio_opus_encoder`——ATVV 会话/ADPCM 解码/PCM 后处理/Opus 归一化均为纯逻辑；`PairedDeviceEntry.hardware` 派生能力集驱动 `Send*` 跳过与托盘菜单显隐）。**新增核心行为优先放这里并在 `core_tests.cc` 覆盖。**
  - `VoiceStickApp`：Win32 外壳（托盘、窗口、BLE 中央、剪贴板/`SendInput` 注入、全局热键、WinSparkle、对话框）。
  - `VoiceStickFlash`：独立 COM 口固件烧录小工具（BLE OTA 兜底链路），Win32 GUI 外壳只做 UI + esptool 子进程；设计见 `Doc/Plan/windows-com-flash-tool.md`。
  - `voicestick_windows_tests`：基于 `assert` 的单元测试，自定义 Fake/Mock 不联网。
  - `voicestick_integration_tests`：L1 ASR 链路集成测试，连真实火山 ASR（需 `%APPDATA%\VoiceStick\config.toml` 配 `volcengine_api_key`）。

### 核心交互模型

固件上报原始按键事实，桌面端解释为交互行为并回写 `ui_state`：

| 状态 | 主键（正面） | 侧键 |
|---|---|---|
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音（**双击直接注入 Enter，不录音**） | 双击恢复上一次输入确认；单击无操作（体感入口已移除） |
| 录音中 | 释放结束录音 | 单击不取消当前录音 |
| 识别中 | 忽略新录音 | 单击取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 单击取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 单击取消待粘贴文本 |

补充：主键双击取消当前活跃录音/字幕后注入 Enter；侧键单击的取消语义仅在有活跃录音/识别/待粘贴时生效，体感鼠标态下单击退出体感；敲击检测（`tap_to_arrow`，IMU 敲击映射方向键）体感态下忽略。

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式。文本输出三种目标：`focused_app`（默认，粘贴到焦点并自动按 Return）、`subtitle`（仅字幕）、`wechat_input_method`（Opus→PCM 渲染到虚拟麦克风，不经 ASR）。识别结果可经 OpenAI-compatible LLM 翻译/精修，可按设备单独覆盖输出设置。

## 配置

- 运行时配置：macOS `~/Library/Application Support/VoiceStick/config.toml`；Windows `%APPDATA%\VoiceStick\config.toml`。完整字段说明见 `Doc/Ref/desktop-config.md`，示例见 `desktop/macos/Config/config.example.toml`。
- `asr_provider`：`volcengine` / `voicestick_cloud` / `tencent`，对应不同凭据字段。
- `interaction_mode`：`hold_to_talk`（默认）/ `click_to_talk`；wechat 模式触发方式由 `[wechat_input_method].trigger_mode` 独立控制。
- `[output].target`：`focused_app`（默认）/ `subtitle` / `wechat_input_method`；可用 `[device.<id>.output]` 按设备覆盖。
- 编码器配置（仅 Windows）：全局默认 `encoder_*`（旋转/快慢分档/按键/LED）+ 按设备覆盖 `[device.<id>.encoder]`（键名去前缀，结构镜像 `[device.<id>.output]`）；设置 UI 已从「设置」对话框移至托盘设备子菜单「编码器设置…」（仅 `encoder_present` 设备显示）。完整字段见 `Doc/Ref/desktop-config.md`。
- 设备交互配置（仅 Windows）：IMU 唤醒灵敏度 / 敲击映射方向键 / 敲击灵敏度 / 体感鼠标左右·上下灵敏度，全局默认顶层键（`imu_wake_sensitivity`/`tap_to_arrow`/`tap_sensitivity`/`air_mouse_sensitivity_x/y`，v1.8.x 旧配置仍可读）+ 按设备覆盖 `[device.<id>.interaction]`（键名一致）；设置 UI 已从「设置」对话框移至托盘设备子菜单「设备交互设置…」（所有设备显示）。其中体感鼠标灵敏度按设备覆盖，其余进阶 `air_mouse_*` 参数仍为全局。完整字段见 `Doc/Ref/desktop-config.md`。
- `paired_device_ids`：已配对设备 4 位十六进制 ID 列表，`VS-XXXX` 与 `RC-XXXX` 可混合。
- 小米遥控器配置（仅 Windows）：全局 `xiaomi_suppress_f5`（语音键附带 F5 抑制，默认开）+ 按设备覆盖 `[device.<id>.xiaomi]`（`gain_db`/`double_click_ms`，结构镜像 `[device.<id>.output]`，托盘设备子菜单「遥控器设置…」）。完整字段见 `Doc/Ref/desktop-config.md`。
- Windows MSI 会把 `config.template.toml` 装到 `%ProgramFiles%\VoiceStick\` 下，并由 `SeedMsiConfigExec` 自定义动作在**安装时**整份复制覆盖到 `%APPDATA%\VoiceStick\config.toml`（仅全新安装，升级不覆盖）。打包时 `generate_msi_config.ps1` 从本机 `%APPDATA%\VoiceStick\config.toml`（默认，`VOICESTICK_MSI_CONFIG_SOURCE` 可覆盖）提取密钥生成含 key 的构建产物（gitignored，密钥不进仓库）；`extract_builtin_key.ps1` 输出内置凭据供 cmake 注入 `builtin_secrets.h.in`。运行时 `Active*()` 优先读 config.toml，空则回退内置凭据，首启开箱即用。

## 代码风格

- Swift：标准 Swift/AppKit 命名。
- C++（Windows）：Google 风格——`snake_case` 文件名/变量、`CapWords` 类型、`MixedCase()` 方法、4 空格缩进。
- C（固件）：ESP-IDF 风格，组件经 `idf_component_register` 注册，依赖用 `REQUIRES` 声明。
- Vue/JS：Vue 3 Composition API。

仓库未提交统一 lint/formatter 配置；**不要臆造 `npm run lint`、Swift lint 或 C++ lint 命令**，修改后用对应组件已有的构建/测试命令验证。

## 测试策略

- Windows：`core_tests.cc` 用自定义 Fake/Mock 单测 `voicestick_core`（不联网）；`integration_tests.cc` 连真实火山 ASR，无 key 返回 77 被 SKIP。
- macOS / 固件 / 网站：无自动化测试，验证方式为各自构建命令通过 + 手动运行时测试。
- Python E2E 真机验证：`scripts/e2e_test/`（L0 语料、L3 固件回放、L4 微信输入法、ASR/热词离线评测、功耗导出），用真实 BLE 与真实 ASR/音频链路，**不伪造结果**。索引见 `Doc/Ref/e2e-test-toolchain.md`；依赖 `bleak`/`numpy`/`sounddevice`（**未列入**根目录 `requirements.txt`，需另行 `pip install`）。小米遥控器 ATVV 工具组：`atvv_capture.py`（真机 golden 采集）/`atvv_bench.py`（golden 会话离线 ASR 评测）/`atvv_probe.py`（延迟与静置探针）；golden fixtures 接 C++ 单测与集成测试回放，`fixtures/xiaomi/demo_synthetic/` 入库冒烟、真机采集目录 gitignore。

## 安全注意事项

- API 密钥等凭据只存在于本机 `config.toml`，不提交仓库；示例配置用占位符。
- 真实签名证书与 Sparkle 私钥只存在于签名机；便携包模板用占位符。
- 集成测试与 E2E 坚持「不伪造结果」：无凭据/无设备时 SKIP 或报错，不要为了让测试变绿而 mock 掉真实链路。
- 固件 OTA 与桌面自动更新走官方渠道（GitHub Release + 阿里云 OSS + appcast），不绕过签名校验。

## 发布流程

推送与 `VERSION` 匹配的 `v<版本号>` 标签触发 `.github/workflows/release.yml`：构建固件（OTA bin、merged bin、manifest.json）→ 构建签名 macOS 产物 → 创建 GitHub Release → 上传固件到阿里云 OSS → 触发 `deploy-website.yml` 更新 `website/public/appcast.xml`。

Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建签名后手动上传 Release——一次产出 `VoiceStick_<版本>_zh-CN.msi` 与 `VoiceStick_<版本>_en-US.msi` 两个语言版（WiX 4 一次构建只产一个 culture；本地化文件在 `desktop/windows/installer/` 的 `zh-CN.wxl`/`en-US.wxl` 与对应 license），再手动运行 `Deploy Website to GitHub Pages` 工作流收录 MSI 条目——**appcast 只收录 en-US 版**（WinSparkle 0.9.2 不支持按语言选 enclosure；zh-CN 版仅供手动下载）。完整步骤见 `Doc/Ref/release.md`。

`CHANGELOG.md` 可能滞后于 `VERSION`（中间版本条目可能缺失），**以 `VERSION` 为准**；发布时同步追加条目。

## 项目 Skills 与经验文档

- `.agents/skills/` 下维护项目级 Skill（`sticks3-flash-ota`、`build-windows`、`build-firmware`、`usb-jtag-flash-log`），相关场景自动加载。
- 排查问题或改动相关模块前先查 `Doc/Expe/`：`claude-memory-distilled.md` 是约 80 条项目记忆的蒸馏（BLE 僵尸链路、watcher 静默失效、热词处理、ASR/热词评测结论等）。其中寄存器值、阈值、文件:行号均为记录时点结论，**引用前以当前源码为准**。
- `Doc/` 子目录：`Ref/`（协议、发布流程等参考）、`Plan/`（设计方案，大写 P）、`Guide/`（第三方服务接入指南）、`Expe/`（经验教训）。

## 给 Agent 的提示（易踩坑）

- Windows 桌面端修改并构建通过后，自动重启 VoiceStick.exe 让改动生效（先结束运行中的进程避免锁定链接产物，启动后检查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）。
- `.gitignore` **整体忽略了 `desktop/windows/`**，提交 Windows 端源码必须用 `git add -f`，否则会被静默漏提交。教训：f75af4f5 曾漏提交新增的 `power_log_monitor.*`/`battery_monitor_dialog.*` 致全新克隆无法构建（CMakeLists.txt 引用缺失文件，被忽略的新文件在 `git status` 中不提示）；新增 Windows 源文件后必须 `git add -f` 并用 `git ls-files <path>` 验证确已跟踪（排查漏网：`git ls-files --others --ignored --exclude-standard desktop/windows/src/`）。
- 搜索仓库时排除 `website/node_modules/` 和 `firmware/build/`。
- 修改网站 UI 文案必须同步更新 `website/src/i18n/zh-CN.json` 和 `en-US.json`；修改根目录 `README.md` 必须同步 `README.zh-CN.md`；修改 `VERSION` 必须同步 `firmware/version.txt`。
- 修改协议或公共数据结构时，必须同时更新 `Doc/Ref/protocol.md` 和所有实现端（固件 C、macOS Swift、Windows C++）。
- MiniEncoderC 编码器键是 I2C 外设，不能作为深睡唤醒源；主键（GPIO11）仍是唯一唤醒键。
- Windows 桌面端构建若报 `C1083: winrt/base.h`（或 `winrt/Windows.Devices.Bluetooth.h`）找不到，是本机 Windows SDK 的 `Include/<ver>/winrt/` 仅含旧版 WRL 风格头文件、缺 C++/WinRT 投影头所致；`build_win.bat` 已用 SDK 自带 `cppwinrt.exe` 从 union metadata 一次性生成完整投影头到 `desktop/windows/generated_winrt/`（gitignored，且不受 `rd /s /q build-x64` 清理影响）并 prepend 到 `INCLUDE`。手动用 vcvars64 构建时须同样把该目录加入 `INCLUDE`。
- VoiceStickFlash（COM 口烧录工具）相关改动：除常规构建/CTest 外，用 `powershell -ExecutionPolicy Bypass -File scripts\prepare_flash_payload.ps1` 冒烟 payload（`python.exe -m esptool version`；含中文注释，脚本须保持 UTF-8 with BOM）；真机验收清单见 `Doc/Plan/windows-com-flash-tool.md` §7.2。
