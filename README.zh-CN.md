# Voice Stick

[English](README.md) | 简体中文

Voice Stick 将 M5Stack StickS3（ESP32-S3）改造为桌面端蓝牙按键语音输入设备。

按住 StickS3 正面按键开始录音，释放后桌面端把音频送到 ASR，显示识别文本，并在短暂确认倒计时后把最终结果粘贴到当前焦点输入框。识别文本可在输出前由 LLM 做精修或翻译。桌面端提供 macOS 与 Windows 客户端；浏览器烧录工具与更新源托管在网站上。

## 架构

设备负责采集按键与音频并通过 BLE 上报。桌面端是状态唯一可信源：持有交互状态机、ASR、文本显示与文本注入。网站负责落地页、浏览器端 USB 固件烧录和 Sparkle/WinSparkle 更新源。

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> Desktop -> Ogg Opus -> ASR -> paste/subtitle
```

主机端不把 Opus 解码回 PCM；ASR 与调试音频缓存都使用同一份 Ogg Opus 流。

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
- IMU：BMI270
- 音频编解码：ES8311 经 I2S，16 kHz / 16 bit / 单声道
- 显示屏：135 x 240 ST7789 竖屏
- LCD 背光：GPIO38 PWM

引脚定义在 `firmware/components/stick_s3_board/include/stick_s3_board.h`。

## 交互模型

固件上报原始按键事实（`button_down` / `button_up`，附带 `primary` 或 `secondary`）。桌面端持有交互状态机，并把 `ui_state` 回写给固件用于屏幕显示。

| 状态 | 主键（正面） | 侧键 |
| --- | --- | --- |
| 未配对/未连接 | 不录音，屏幕显示 `VS-XXXX` | 无有效动作 |
| 连接空闲 | 按住开始录音 | 恢复上一次输入确认 |
| 录音中 | 释放结束录音 | 不取消当前录音 |
| 识别中 | 忽略新录音 | 取消正在进行的识别 |
| 确认倒计时中 | 暂停自动粘贴，进入手动确认 | 取消待粘贴文本 |
| 手动确认中 | 确认粘贴 | 取消待粘贴文本 |

支持 `hold_to_talk`（默认）和 `click_to_talk` 两种交互模式。文本输出支持 `focused_app`（默认粘贴到当前焦点，默认自动按 Return）和 `subtitle`（仅显示字幕）。识别结果可通过 OpenAI-compatible LLM 精修或翻译，也可按设备单独覆盖输出设置。

## BLE 协议

GATT 服务 UUID：`8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100`

| 名称 | UUID | 方向 | 属性 | 载荷 |
| --- | --- | --- | --- | --- |
| `audio_tx` | `…5101` | 设备 → 主机 | notify | Opus 音频帧 |
| `state_tx` | `…5102` | 设备 → 主机 | notify | 按键事件、电量、固件版本 |
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

浏览器烧录器用 Web Serial 写入固件 manifest（`VITE_FIRMWARE_MANIFEST_URL`）中的 merged 镜像。

## 配置

配置路径：

- macOS：`~/Library/Application Support/VoiceStick/config.toml`
- Windows：`%APPDATA%\VoiceStick\config.toml`

从示例 `desktop/macos/Config/config.example.toml` 创建。

| 字段 | 说明 |
| --- | --- |
| `asr_provider` | `volcengine`、`voicestick_cloud` 或 `tencent` |
| `volcengine_api_key` | 火山引擎直连 API key（`X-Api-Key`） |
| `voicestick_api_key` / `voicestick_cloud_url` | VoiceStick Cloud 中继 key 与 WebSocket URL |
| `llm_base_url` / `llm_api_key` / `llm_model` | OpenAI-compatible LLM，用于翻译与精修 |
| `refine_enabled` / `refine_prompt` | 用 LLM 精修 ASR 原文（去停顿空格、修标点、去口头语），默认 `true`；prompt 留空用内置默认 |
| `hotword_process_enabled` | 划词加词时用 LLM 提炼热词（Windows），默认 `false`，复用 `llm_*` 配置 |
| `hotword_process_prompt` | 提炼提示词覆盖，留空使用内置默认 |
| `interaction_mode` | `hold_to_talk` 或 `click_to_talk`（focused_app/字幕的触发方式；wechat 模式用 `[wechat_input_method].trigger_mode`） |
| `resource_id` | 火山引擎 resource ID |
| `asr_hotwords` | 逗号分隔的 ASR 热词，同时作为术语提示传给 LLM |
| `paired_device_ids` | 逗号分隔的 4 位十六进制 ID，如 `C3D8,09AF` |
| `device_theme_colors` / `device_overlay_positions` | 可选的按设备悬浮窗颜色与位置 |
| `auto_enter` | 粘贴后是否自动按 Return |
| `debug_audio_cache` / `debug_audio_dir` | 是否保存调试 Ogg Opus 及保存目录（Windows 默认 `%LOCALAPPDATA%\VoiceStick\DebugAudio`） |
| `[output].target` | `focused_app` 或 `subtitle` |
| `[output].transform` | `original` 或 `translate` |
| `[output].translation_target` | 目标语言代码，如 `en` 或 `zh-Hans` |
| `[device.<id>.output]` | 按设备覆盖 transform 与翻译目标 |

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
- **USB 串口烧录**：浏览器烧录器（或 `idf.py flash`）在偏移 `0x0` 经 USB 写入 merged 镜像。

完整发布流程见 `Doc/Ref/release.md`。

## 调试音频

设置 `debug_audio_cache = true` 后，每段有效识别会保存为可播放的 Ogg Opus 文件。短于 0.5 秒的录音会被丢弃，不送 ASR。

## 许可证

见 [LICENSE](LICENSE)。
