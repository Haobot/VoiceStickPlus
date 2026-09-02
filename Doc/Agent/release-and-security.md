# 发布流程与安全注意事项（Agent 视角）

本文承载发布与安全相关细节，2026-08 由根目录 `AGENTS.md`/`CLAUDE.md` 的「安全注意事项」「发布流程」章节迁入；根指南只保留安全红线摘要与指向本文的指针。权威发布流程文档为 `Doc/Ref/release.md`，本篇只收 Agent 工作时常用的要点。

## 安全注意事项

- API 密钥等凭据字段（`volcengine_api_key`、`tencent_secret_*`、`llm_api_key` 等）只存在于本机 `config.toml`，不要提交进仓库；示例配置使用占位符。
- Windows 支持把凭据编译进 exe：cmake configure 传 7 个 `-D VOICESTICK_BUILTIN_*` 变量生成 `builtin_secrets.h`（`src/builtin_secrets.h.in` 模板），运行时 `AppConfig::Active*()` 访问器按「配置值优先，空则回退内置，不落盘」解析。这些值是发布构建注入的测试凭据，源文件模板不含真实密钥。
- MSI 打包链路（`extract_builtin_key.ps1` / `generate_msi_config.ps1` / `build-msi.bat`）在本机提取密钥生成含 key 的构建产物，产物均 gitignored；不要把生成的含 key config 提交进仓库。
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

Windows MSI 需在本地签名机用 `scripts\build-msi.bat` 构建并签名（脚本自动完成内置凭据注入、MSI config 生成与 flash payload 准备），一次产出 `VoiceStick_<版本>_zh-CN.msi` 与 `VoiceStick_<版本>_en-US.msi` 两个语言版（WiX 4 一次构建只产一个 culture，多语言 MSI 支持尚未落地，故逐 culture 构建；安装程序本地化文件在 `desktop/windows/installer/` 下的 `zh-CN.wxl`/`en-US.wxl` 与 `license-zh-CN.rtf`/`license-en.rtf`）。两个 MSI 都上传到对应 GitHub Release，再手动运行 `Deploy Website to GitHub Pages` 工作流收录 MSI 条目——**appcast 只收录 en-US 版**（WinSparkle 0.9.2 不支持按语言选 enclosure，`sparkle:language` 未实现；zh-CN 版仅供中文用户手动下载安装）。完整步骤见 `Doc/Ref/release.md`。

Windows 便携版（免安装 zip）用 `scripts\package-portable.ps1` 打包（PowerShell 脚本，用 .NET 写 UTF-8 文件规避 cmd 中文 `echo` 块在 GBK 代码页下的解析错位；脚本须存为 UTF-8 with BOM）；本机无签名证书时可用 `scripts\build-msi-unsigned.bat` 构建未签名 MSI 验证安装流程。打包产物放在 `dist/`（已被视为本地产物目录）。

`CHANGELOG.md` 是版本变更记录（最新已发布条目为 v2.3.6，另有 `Unreleased` 段落记录未发布的 VoiceStickFlash 工具）。发布新版本时应同步追加条目；注意该文件可能滞后于 `VERSION`（中间版本 v2.0.0/v2.1.0 条目缺失），以 `VERSION`（当前 `2.3.6`）为准。
