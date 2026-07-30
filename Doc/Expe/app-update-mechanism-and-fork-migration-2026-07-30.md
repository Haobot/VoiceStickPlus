# 桌面端自更新机制与 fork 迁移要点（2026-07-30）

起因：用户问「检查应用更新」的原理，以及如何把整个发布/自更新体系搭建到自己的 GitHub。本文为排查结论沉淀，迁移操作手册见 `Doc/Guide/fork-publish-to-own-github.md`。

## 机制结论（记录时点，引用前以源码为准）

- 桌面端自更新是 Sparkle 框架族双端实现：macOS 用 Sparkle 2.x（`AppDelegate.swift:124-133`，`SPUStandardUpdaterController`），Windows 用 WinSparkle（`win32_app.cc:364-369`，24 小时自动检查 + 托盘菜单 `win_sparkle_check_update_with_ui()`）。两端共用一份 appcast：`website/public/appcast.xml` 部署到 GitHub Pages，靠 `sparkle:os="macos"/"windows"` 区分平台条目。
- macOS 端仅在 `Info.plist` 的 `SUPublicEDKey` 是真实公钥（非 `REPLACE_WITH` 前缀）时才启用 updater（`AppDelegate.swift:251-256`）；Windows 便携版跳过 WinSparkle 初始化（`win32_app.cc:371`），只有 MSI 安装版参与自更新。
- 签名模型两端不对称：macOS 强制 EdDSA（`sparkle:edSignature`，私钥在签名机、公钥内置客户端）；Windows 条目无 edSignature，靠 MSI 自身代码签名 + `sparkle:installerArguments="/passive"` 被动安装。
- `scripts/update-appcast.py` 每次发布重写 macOS item（带新签名），Windows item 有 MSI 参数才更新、否则**保留旧条目**（`update-appcast.py:70`）——因为 MSI 在本地签名机产出、晚于 CI 上传。
- `deploy-website.yml:42-43` 每次先 `curl` 拉回线上 appcast 再重写，所以**仓库内的 appcast.xml 是模板，不代表线上状态**。
- 固件 OTA 与桌面端自更新是两套独立链路：固件 manifest 硬编码指向阿里云 OSS（macOS `AppConfig.swift:170-172`、Windows `firmware_manifest.cc:209`），迁移时要一起改或换成 GitHub Release 资产。

## fork 迁移的关键事实

- 硬编码 `78/voicestick` / `78.github.io` 共 8 处（Windows CMake、macOS Info.plist + AppConfig、deploy-website.yml、update-appcast.py、appcast.xml 模板、App.vue、两份文档），清单见迁移手册。
- Sparkle EdDSA 密钥对不可复用原作者的：fork 必须用 `generate_keys` 重新生成，否则旧客户端验签必失败；已发出的旧客户端内置旧 appcast 地址与旧公钥，**无法跨源升级**，用户需手动换装一次新版本。
- 纯 GitHub 替代方案（免阿里云 OSS）：两处固件 manifest 地址改指 `releases/latest/download/manifest.json`，删 `release.yml` 的 OSS 上传步骤。

## 方法论教训

- 回答「如何迁移/如何搭建」类问题前，先全仓 grep 硬编码的组织名/域名（`github.com/78`、`78.github.io`），拿真实清单再答，避免凭印象漏项——本次第一轮只想到 appcast 和 Secrets，grep 后才补齐 App.vue、vite base、固件 manifest 等 8 处。
- 区分「同一文件名 ≠ 同一内容」：仓库里的 appcast.xml 与线上 appcast.xml 语义不同（模板 vs 状态），排查更新问题时应以线上 `curl` 结果为准，勿被仓库内占位模板误导。
