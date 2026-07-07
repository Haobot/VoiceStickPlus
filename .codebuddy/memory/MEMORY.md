# VoiceStick 项目长期记忆

## 项目概要
- VoiceStick：M5Stack StickS3 (ESP32-S3) 蓝牙按键语音输入设备
- 当前版本：v1.9.0（2026-07-06）
- 四大模块：固件 (C/ESP-IDF)、macOS (Swift)、Windows (C++20)、网站 (Vue 3)
- 核心数据流：StickS3 麦克风 → Opus → BLE → 桌面端 → ASR → 文本粘贴/字幕
- 状态机归属桌面端，固件是"哑终端"

## 关键约定
- 修改 VERSION 时同步更新 firmware/version.txt
- 修改 README.md 时同步更新 README.zh-CN.md
- 修改网站文案时同步更新 zh-CN.json 和 en-US.json
- 修改协议时同步更新 Doc/Ref/protocol.md 和所有实现端
- .gitignore 忽略 desktop/windows/，提交需 git add -f
- Windows 构建目录统一 desktop/windows/build-x64
- 设计文档放 Doc/Plan/（大写 P）

## 构建/测试命令
- Windows: build_win.bat 或 cmake + ninja + MSVC 2022 x64
- Windows 测试: ctest --test-dir desktop/windows/build-x64 --output-on-failure
- 固件: python scripts/idf_cli.py -cus -p COMxx
- macOS: cd desktop/macos && swift build
- 网站: cd website && npm run build
