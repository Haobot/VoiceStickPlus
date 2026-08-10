# Voice Stick

> **派生项目**：本仓库派生自 [78/voicestick](https://github.com/78/voicestick)，原版 Voice Stick 项目由 [Xiaoxia](https://github.com/78) 原创。

[English](README.md) | 简体中文

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。

项目介绍页：[Hackster.io — VoiceStick Plus](https://www.hackster.io/haobot2018/voicestick-plus-d56e05)

按住 StickS3 正面按键开始录音，释放后桌面端把音频送到 ASR，显示识别文本，并在短暂确认倒计时后把最终结果粘贴到当前焦点输入框。识别文本可在输出前由 LLM 做精修或翻译。桌面端提供 macOS 与 Windows 客户端；浏览器烧录工具与更新源托管在网站上。

## 主要功能

- 两种触发方式：按住说话（`hold_to_talk`，默认）与点按说话（`click_to_talk`）。
- 双击手势（固件端检测）：主键双击取消当前会话并直接注入 Enter；侧键双击恢复上一次输入确认。
- 三种输出目标：粘贴到焦点应用（默认）、仅字幕显示、微信输入法模式——把 Opus 解码为 PCM 渲染到系统虚拟麦克风（如 VB-CABLE），供微信输入法等应用作为音频输入源（Windows）。
- LLM 翻译与精修；火山热词表/替换词表、划词加词与候选热词挖掘（Windows）。
- 体感鼠标：BMI270 IMU 驱动光标控制，体感态下主键映射为鼠标左键，侧键单击退出（Windows）。
- 敲击映射：IMU 敲击检测映射为方向键（`tap_to_arrow`，Windows）。
- MiniEncoderC 编码器：按钮等价主键，旋转映射方向键（慢速逐行/快速翻页分档），按键与旋转动作可按设备自定义（Windows）。
- 按设备覆盖：输出、设备交互（IMU 唤醒/敲击/体感灵敏度）与编码器设置均可按设备单独配置（托盘设备子菜单，Windows）。
- 多设备配对与连接，悬浮窗/字幕实时显示识别进度。
- 固件升级：BLE OTA 免线升级；Windows 另附 VoiceStickFlash COM 口烧录工具作为救砖兜底。

## 架构

设备负责采集按键与音频并通过 BLE 上报。桌面端是状态唯一可信源：持有交互状态机、ASR、文本显示与文本注入。网站负责落地页、浏览器端 USB 固件烧录和 Sparkle/WinSparkle 更新源。

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
                                                              \-> Opus decode -> PCM -> 虚拟麦克风（微信输入法模式）
```

ASR 路径不把 Opus 解码回 PCM；ASR 与调试音频缓存都使用同一份 Ogg Opus 流。例外是微信输入法模式：桌面端把 Opus 解码为 PCM 后渲染到系统虚拟麦克风，不经 ASR 文本注入。

## 目录结构

- `firmware/`：ESP-IDF C 固件，目标板 M5Stack StickS3 / ESP32-S3。
- `desktop/macos/`：SwiftPM/AppKit 菜单栏应用，macOS 12+。
- `desktop/windows/`：C++20 / Win32 / C++/WinRT 托盘应用，Windows 10 1903+。
- `desktop/linux/`：Linux 桌面端占位目录。
- `website/`：Vue 3 + Vite 站点，含 Web Serial 固件烧录工具、中英文落地页和 appcast 发布页。
- `Doc/`：BLE 协议、火山引擎 ASR 帧格式、发布流程参考（`Doc/Ref/`），以及实施方案 RFC（`Doc/Plan/`）。

构建命令、代码架构与贡献约定详见 [`CLAUDE.md`](CLAUDE.md) / [`AGENTS.md`](AGENTS.md)。

## 硬件目标

- 主板：M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- 正面按键：GPIO11，协议 `primary`，按住说话与深睡唤醒
- 侧面按键：GPIO12，协议 `secondary`，取消或恢复上一次输入确认
- PMIC 中断：GPIO13
- IMU：BMI270（体感鼠标与敲击检测）
- MiniEncoderC 编码器（可选）：I2C @0x42，顶部 Hat 排针 SDA=G8 / SCL=G0 第二路 I2C 总线；按钮等价主键，旋转映射方向键，录音时亮红灯；不能作为深睡唤醒源
- 音频编解码：ES8311 经 I2S，16 kHz / 16 bit / 单声道
- 显示屏：135 x 240 ST7789 竖屏
- LCD 背光：GPIO38 PWM

引脚定义在 `firmware/components/stick_s3_board/include/stick_s3_board.h`。

## 交互模型

固件上报原始按键事实（`button_down` / `button_up`，附带 `primary` 或 `secondary`）。桌面端持有交互状态机，并把 `ui_state` 回写给固件用于屏幕显示。

| 状态 | 主键（正面） | 侧键 |
| --- | --- | --- |
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音（双击直接注入 Enter，不录音） | 双击恢复上一次输入确认；单击无操作 |
| 录音中 | 释放结束录音 | 单击不取消当前录音 |
| 识别中 | 忽略新录音 | 单击取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 单击取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 单击取消待粘贴文本 |

双击检测在固件端完成，桌面端统一处理 `button_double_click`：主键双击先取消当前活跃录音/字幕再注入 Enter；侧键双击执行「恢复上一次输入确认」（体感鼠标态下忽略）。侧键单击的取消语义仅在有活跃录音/识别/待粘贴时生效；体感鼠标态下单击退出体感；真正空闲时单击无操作。

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式（微信输入法模式的触发方式由 `[wechat_input_method].trigger_mode` 独立控制）。文本输出支持 `focused_app`（默认粘贴到当前焦点，默认自动按 Return）、`subtitle`（仅显示字幕）和 `wechat_input_method`（虚拟麦克风，不经 ASR 文本注入）。识别结果可通过 OpenAI-compatible LLM 精修或翻译，也可按设备单独覆盖输出设置。

## BLE 协议

GATT 服务 UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| 名称 | UUID | 方向 | 属性 | 载荷 |
| --- | --- | --- | --- | --- |
| `audio_tx` | `…5101` | 设备 → 主机 | notify | Opus 音频帧 |
| `state_tx` | `…5102` | 设备 → 主机 | notify | 按键事件（含 `button_double_click`）、电量、固件版本、体感鼠标运动帧 |
| `control_rx` | `…5103` | 主机 → 设备 | write without response | `ui_state`、交互/敲击/体感鼠标设置、`ota_commit` |
| `ota_rx` | `…5104` | 主机 → 设备 | write / write without response | BLE OTA 控制与数据帧 |
| `ota_tx` | `…5105` | 设备 → 主机 | notify | BLE OTA 状态帧 |

完整帧格式见 `Doc/Ref/protocol.md`。修改 BLE 消息时需同步更新固件、macOS、Windows 和文档。

## 构建

### 固件（ESP-IDF v5.5.1，目标 `esp32s3`）

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Windows 上可用 `python scripts/idf_cli.py -cus -p COM17` 一体化完成编译+烧录+监控。仍使用旧单应用分区表的设备首次升级需先 `erase-flash`。当前分区表为两个 3 MB OTA app slot 加约 1984 KB `storage` 分区。

### macOS 桌面端（SwiftPM，macOS 12+）

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

应用是菜单栏附件，启动时请求蓝牙权限。文本注入模拟 `Command-V` 加可选 Return；若系统拦截按键，需授予辅助功能权限。

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

```bat
build_win.bat
```

或在 VS 2022 x64 开发者环境中手动构建：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
ctest --test-dir desktop\windows\build-x64 --output-on-failure
desktop\windows\build-x64\VoiceStick.exe
```

`VoiceStickApp` CMake 目标的产物是 `VoiceStick.exe`。固件升级走 BLE OTA（桌面端）或 USB 串口烧录，没有 Wi-Fi/LAN OTA 路径。

### 网站（Vue 3 + Vite，Node 22）

```sh
cd website
npm install
npm run dev      # 本地开发服务器
npm run build    # 最小验证
```

浏览器烧录器用 Web Serial 写入固件 manifest（同源于 `BASE_URL + 'firmware/'`，由 `deploy-website.yml` 从最新 GitHub Release 同步）中的 merged 镜像。

## 配置

配置路径：

- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows：`%APPDATA%\VoiceStick\config.toml`

从示例 `desktop/macos/Config/config.example.toml` 创建。

| 字段 | 说明 |
| --- | --- |
| `asr_provider` | `volcengine`、`voicestick_cloud` 或 `tencent` |
| `volcengine_api_key` | 火山引擎直连 API key（`X-Api-Key`） |
| `volcengine_boosting_table_id` / `volcengine_correct_table_id` | 火山自学习平台热词表 / 替换词表 ID（控制台创建），作为 `corpus.boosting_table_id` / `corpus.correct_table_id` 发送 |
| `voicestick_api_key` / `voicestick_cloud_url` | VoiceStick Cloud 中继 key 与 WebSocket URL |
| `llm_base_url` / `llm_api_key` / `llm_model` | OpenAI-compatible LLM，用于翻译与精修 |
| `refine_enabled` / `refine_prompt` | 用 LLM 精修 ASR 原文（去停顿空格、修标点、去口头语），默认 `false`（需手动开启）；prompt 留空用内置默认 |
| `hotword_process_enabled` | 划词加词时用 LLM 提炼热词（Windows），默认 `false`，复用 `llm_*` 配置 |
| `hotword_mining_enabled` | 每次识别会话后异步让 LLM 从最终文本提炼候选热词（Windows），同一词达阈值（3 次）经托盘通知与设置-热词区人工确认入表；默认 `false`（每会话多一次 LLM 调用），复用 `llm_*` 配置 |
| `hotword_process_prompt` | 提炼提示词覆盖，留空使用内置默认 |
| `interaction_mode` | `hold_to_talk` 或 `click_to_talk`（focused_app/字幕的触发方式；wechat 模式用 `[wechat_input_method].trigger_mode`） |
| `tap_to_arrow` / `tap_sensitivity` | IMU 敲击映射方向键及灵敏度（Windows），体感鼠标态下忽略 |
| `[wechat_input_method]` | 微信输入法模式：`trigger_mode`（独立触发方式）、虚拟麦克风端名、全局热键等（Windows），详见 `Doc/Ref/desktop-config.md` |
| `resource_id` | 火山引擎 resource ID |
| `asr_hotwords` | 逗号分隔的 ASR 热词，同时作为术语提示传给 LLM |
| `paired_device_ids` | 逗号分隔的 4 位十六进制 ID，如 `C3D8,09AF` |
| `device_theme_colors` / `device_overlay_positions` | 可选的按设备悬浮窗颜色与位置 |
| `auto_enter` | 粘贴后是否自动按 Return |
| `debug_audio_cache` / `debug_audio_dir` | 是否保存调试 Ogg Opus 及保存目录（Windows 默认 `%LOCALAPPDATA%\VoiceStick\DebugAudio`） |
| `[output].target` | `focused_app`、`subtitle` 或 `wechat_input_method`（虚拟麦克风，Windows） |
| `[output].transform` | `original` 或 `translate` |
| `[output].translation_target` | 目标语言代码，如 `en` 或 `zh-Hans` |
| `[device.<id>.output]` | 按设备覆盖 transform 与翻译目标 |
| `[device.<id>.interaction]` / `[device.<id>.encoder]` | 按设备覆盖交互设置（IMU 唤醒/敲击/体感灵敏度）与编码器设置（Windows，托盘设备子菜单「设备交互设置…」/「编码器设置…」） |

火山引擎支持的 `resource_id`：`volc.seedasr.sauc.duration`、`volc.seedasr.sauc.concurrent`、`volc.bigasr.sauc.duration`、`volc.bigasr.sauc.concurrent`。

请勿提交 API key。

## 配对流程

1. 烧录并启动 StickS3，屏幕显示 `VS-XXXX`。
2. 启动桌面端。
3. 打开 `Pair Device…`，在扫描列表中选中对应 `VS-XXXX` 并配对。
4. 配对后桌面端会扫描并连接该设备。重复以上步骤可配对更多设备。

也可手动编辑 `paired_device_ids`。保存了 ID 后，桌面端会忽略附近未配对的 VoiceStick 设备。

## 固件更新

- **BLE OTA**：桌面端在启动、设备连接/重连和手动刷新时检查按哈希签名的 manifest，校验大小与 SHA-256 后按已连接设备经 BLE 推送 OTA 镜像。
- **VoiceStickFlash（Windows）**：独立的 COM 口固件烧录小工具，随 MSI 与便携版分发（自包含 esptool 运行时，免系统 Python）。支持整包烧录（merged bin @ 0x0）、仅应用分区（@ 0x10000）、先完全擦除再整包三种模式，作为 BLE OTA 之外的救砖/回退链路。入口：托盘菜单「固件烧录工具…」或固件更新对话框「高级… COM 口烧录」。
- **USB 串口烧录**：浏览器烧录器（或 `idf.py flash`）在偏移 `0x0` 经 USB 写入 merged 镜像。

完整发布流程见 `Doc/Ref/release.md`。

## 调试音频

设置 `debug_audio_cache = true` 后，每段有效识别会保存为可播放的 Ogg Opus 文件。短于 0.5 秒的录音会被丢弃，不送 ASR。

## 许可证

本项目派生自 [78/voicestick](https://github.com/78/voicestick)（原版 Voice Stick，作者 Xiaoxia）。完整许可证文本见 [LICENSE](LICENSE)。
