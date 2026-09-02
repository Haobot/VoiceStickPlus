# 关键配置文件清单

本文承载仓库关键配置文件的完整清单，2026-08 由根目录 `AGENTS.md`/`CLAUDE.md` 的「关键配置文件」章节迁入；根指南只保留版本同步规则与指向本文的指针。

| 文件 | 用途 |
|---|---|
| `VERSION` | 单一版本来源，纯文本，不含换行 |
| `firmware/version.txt` | 固件向桌面端报告的版本，发布前必须与 `VERSION` 一致 |
| `firmware/CMakeLists.txt` | ESP-IDF 项目入口（`project(voice_stick)`） |
| `firmware/main/CMakeLists.txt` | 主组件注册与依赖声明 |
| `firmware/partitions_ota.csv` | 8 MB 分区表：两个 3 MB OTA app slot + 约 1984 KB `storage`（SPIFFS） |
| `desktop/macos/Package.swift` | SwiftPM 定义（swift-tools 5.9），依赖 Sparkle 2.6+、TOMLKit 0.6+、CZlib |
| `desktop/windows/CMakeLists.txt` | Windows 端构建，拆为 `voicestick_core` + `VoiceStickApp` + `VoiceStickFlash` + 两个测试目标 |
| `desktop/windows/src/version.h.in` | Windows 版本资源模板，由 CMake 从 `VERSION` 填充 |
| `desktop/windows/src/builtin_secrets.h.in` | Windows 内置凭据模板，由 CMake 从 `-D VOICESTICK_BUILTIN_*` 变量填充（见 `Doc/Agent/release-and-security.md`） |
| `desktop/windows/resources/config.template.toml` | Windows 运行时配置模板，构建时复制到 exe 旁并随 MSI 安装 |
| `desktop/windows/installer/VoiceStick.wxs` | WiX MSI 安装包定义（含 `SeedMsiConfigExec` 配置种子自定义动作） |
| `desktop/linux/` | Linux 桌面占位目录，目前无活跃实现 |
| `website/package.json` | Node 项目配置（仅 `dev`/`build`/`preview` 脚本，无 lint/test） |
| `website/public/appcast.xml` | Sparkle/WinSparkle 更新源 |
| `.github/workflows/release.yml` | 推送 `v*` 标签触发构建与发布 |
| `.github/workflows/deploy-website.yml` | 网站部署与 appcast 更新 |
| `scripts/idf_cli.yaml` | `idf_cli.py` 的配置文件 |
| `scripts/prepare_flash_payload.ps1` | 准备 VoiceStickFlash 自包含 esptool 运行时（python-embed + esptool，幂等；`VOICESTICK_PYTHON_EMBED_URL` 覆盖下载源、`VOICESTICK_PIP_INDEX_URL` 覆盖 pip 索引） |
| `scripts/extract_builtin_key.ps1` / `scripts/generate_msi_config.ps1` | MSI 打包密钥注入：前者从本机 exe/配置提取 7 项内置凭据供 cmake `-D` 注入；后者从本机 `%APPDATA%\VoiceStick\config.toml`（默认，`VOICESTICK_MSI_CONFIG_SOURCE` 可覆盖）提取密钥生成含 key 的 MSI config 产物（gitignored） |
| `requirements.txt` | Python 脚本依赖（`pyyaml` / `pyserial` / `Pillow`），不含 E2E 工具链依赖 |
| `ArduFlux.json` | 本机 ArduFlux 工具的 ESP32-S3 板卡/串口配置（辅助烧录，非构建必需） |
