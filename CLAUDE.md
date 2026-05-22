# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 提供在本仓库工作时的指导。

## 项目概述

Voice Stick 将 M5Stack StickS3 (ESP32-S3) 改造为桌面端蓝牙按键语音输入设备（支持 macOS、Windows，Linux 规划中）。按住设备按键录音，桌面端将音频流式传输至 ASR 服务，显示悬浮窗识别结果，并将最终文本粘贴到当前聚焦的输入框中。

## 整体架构

项目分为四个主要、低耦合的组件：

1. **`firmware/`** — StickS3 硬件的 ESP-IDF C 固件
   - 从 ES8311 I2S 麦克风读取 16 kHz 单声道 PCM，编码为 Opus，通过 BLE 通知流传输
   - 向桌面端上报原始按键事件（主键/侧键的按下/释放）
   - 接收桌面端通过 `ui_state` 写入的状态，渲染对应 UI（配对、监听、思考、确认、电量等）
   - 负责低功耗行为：30 秒无操作熄屏，电池供电 5 分钟后进入深度睡眠；主键可唤醒
   - 支持基于双分区 OTA 表的 BLE 空中升级

2. **桌面端应用** — 原生托盘应用，拥有完整的交互状态机
   - 桌面端是状态的唯一可信源（空闲 → 录音中 → 识别中 → 待确认 → 粘贴）；固件仅上报输入并渲染 UI
   - 将收到的 Opus 帧封装为 Ogg Opus 直接转发给 ASR（不解码/重编码）
   - 负责 BLE 配对、多设备连接、文本注入（剪贴板 + 模拟按键）、配置管理、自动更新
   - 各平台实现：
     - `desktop/macos/` — Swift/SwiftPM，目标 macOS 12+，使用 CoreBluetooth、Sparkle 自动更新
     - `desktop/windows/` — C++20/Win32/C++/WinRT，目标 Windows 10 1903+，使用 WinSparkle 自动更新
     - `desktop/linux/` — 占位工作目录

3. **`website/`** — Vue 3 + Vite 站点，托管浏览器端 USB 固件烧录工具、Sparkle/WinSparkle 用的 appcast XML 更新源、落地页，通过 GitHub Pages 发布

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
发布构建 + 生成 DMG：
```sh
SPARKLE_PUBLIC_ED_KEY="..." ../../scripts/build-macos.sh --release
../../scripts/make-dmg.sh
```

### Windows 桌面端（CMake + Ninja，MSVC 2022 x64）
在仓库根目录的 PowerShell 中执行（需安装 VS 2022）：
```powershell
# 配置并构建
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S desktop\windows -B desktop\windows\build-x64 -G Ninja -DCMAKE_MAKE_PROGRAM="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"'
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C desktop\windows\build-x64'

# 运行全部测试
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir desktop\windows\build-x64 --output-on-failure'

# 运行单个测试（按名称正则过滤）
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir desktop\windows\build-x64 --output-on-failure -R <测试名称正则>'

# 运行
desktop\windows\build-x64\VoiceStick.exe
```

### 网站（Vite + Vue 3）
```sh
cd website
npm install
npm run dev        # 本地开发服务器
npm run build      # 生产构建
npm run preview    # 预览生产构建
```

## 重要约定与注意事项

- **状态机位于桌面端，不在固件中。** 固件仅上报按键事件并渲染收到的 `ui_state`。修改交互流程时，优先修改桌面端协调器；固件改动通常仅限于新增 `ui_state` 对应的画面。
- **音频流在主机端永不解码。** Opus 帧直接封装为 Ogg 页发送给 ASR，调试音频缓存写入的也是同一份 Ogg Opus 流。
- **配置文件路径：**
  - macOS：`~/Library/Application Support/VoiceStick/config.toml`
  - Windows：`%APPDATA%\VoiceStick\config.toml`
- **发布流程：** 推送与 `./VERSION` 匹配的 `v<版本号>` 标签。GitHub Actions 会构建 macOS 应用和固件，将固件 OTA/清单上传至阿里云 OSS，将产物发布到 GitHub Releases，并部署网站/appcast 到 GitHub Pages。签名后的 Windows MSI 需从本地签名机单独上传，然后重新运行 `Deploy Website to GitHub Pages` 工作流以收录 MSI 更新条目。详见 `docs/release.md`。
- **测试：** 仅 Windows 桌面端存在 CTest 测试套件（`desktop/windows/tests/`）。macOS Swift 与固件目前无专用测试套件。仓库内未提交 Lint 配置。
- **Windows 代码风格：** 遵循 Google C++ 命名规范：`snake_case` 文件名/变量，`CapWords` 类型名，`MixedCase()` 方法名，4 空格缩进。
- **固件引脚定义：** `firmware/components/stick_s3_board/include/stick_s3_board.h`
