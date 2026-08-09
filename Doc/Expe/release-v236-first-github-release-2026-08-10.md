# v2.3.6 首次正式 GitHub Release 发布（2026-08-10）

起因：把 VoiceStick 的发布/自更新体系从原仓库 `78/voicestick` 迁移到自己账号 `Haobot/VoiceStickPlus`，并以 **v2.3.6 作为首次正式版**发布（Windows MSI + 便携包 + 固件）。本文沉淀迁移与发布全过程的踩坑，操作手册见 `Doc/Guide/fork-publish-to-own-github.md`（顶部已补「当前状态」）。

## 一、硬编码迁移清单（已落地）

发布前把 16 个文件里的 `78/voicestick` / `78.github.io` / 阿里云 OSS 地址全部替换为 Haobot + GitHub Release 资产：

| 类别 | 文件 | 改动 |
|---|---|---|
| CI | `.github/workflows/release.yml` | 删 `build-macos:` job 与「Upload firmware to Aliyun OSS」步骤；固件 manifest 的 `ota_url`/`merged_url` 用 `${GITHUB_REPOSITORY}/releases/download/${tag}/...` 构造；`needs` 改 `[build-firmware]`；`request-website-deploy` 的 `--repo` 改 `${{ github.repository }}` |
| CI | `.github/workflows/deploy-website.yml` | appcast/Release 下载/`VITE_FIRMWARE_MANIFEST_URL` 改用 `${{ github.repository_owner }}.github.io/${{ github.event.repository.name }}` 与 `${{ github.repository }}` |
| 脚本 | `scripts/update-appcast.py` | 新增 `--appcast-url` 必填；macOS 参数改可选，支持「仅 Windows」发布（无 macOS zip 时不崩溃）；保留旧平台 item 逻辑泛化为 `existing_item(path, sparkle_os)` |
| 客户端 | `desktop/windows/CMakeLists.txt:149` + `win32_app.cc:92` | `VOICESTICK_APPCAST_URL` → `https://haobot.github.io/VoiceStickPlus/appcast.xml` |
| 客户端 | `desktop/windows/src/firmware_manifest.cc:209` | `DefaultManifestUrl()` → `https://github.com/Haobot/VoiceStickPlus/releases/latest/download/manifest.json` |
| 客户端 | `desktop/macos/.../AppConfig.swift` + `Info.plist` | `websiteURL`、`firmwareManifestURL`、`SUFeedURL` 同步 |
| 网站 | `website/src/App.vue` | GitHub 链接 + 下载/固件 URL 改 Haobot；**版本号从 `packageInfo.version` 改为 `import appVersion from '../../VERSION?raw'`**（根 `VERSION` 单一来源，修掉了 `package.json` version=1.9.0 与 VERSION=2.3.6 的脱节） |
| 网站 | `website/vite.config.js:5` | `base` → `'/VoiceStickPlus/'`（Pages 子路径） |
| 网站 | `website/public/appcast.xml` | 清成干净模板（旧 0.1.0 macOS item 残留旧作者链接） |
| 文档 | `Doc/Ref/release.md` / `Doc/Guide/fork-publish-to-own-github.md` / 经验文档 | 版本号示例 0.2.4→2.3.6、地址替换、补「迁移已落地」注记 |

## 二、发布流程实测（含坑）

### 坑 1：`gh release view` 在无 Release 时直接失败，导致部署 workflow 先炸一次

`deploy-website.yml` 的「Update appcast from latest release」步骤用 `set -euo pipefail`，`gh release view --json tagName`（无 tag 参数默认查 latest）在**没有任何 Release** 时输出 `release not found` 并退出码 1，整个步骤失败。

触发路径：先 push main（触发 `deploy-website.yml`）→ 再打 tag（触发 `release.yml` 建 Release）。main push 那次的部署必然在 Release 创建前执行 → 必炸。

**解决**：本次靠 Release 建成后手动 `gh workflow run deploy-website.yml --ref main` 重跑。**根治建议**（未做，避免在 CI 运行中改 workflow）：给 `gh release view` 加 `|| true` 兜底，无 Release 时跳过 appcast 更新只部署静态站。

### 坑 2：`request-website-deploy` 与「手动上传 MSI」竞态

`release.yml` 的 `request-website-deploy` job 在 Release 创建后立即触发 `deploy-website.yml`，而 MSI/便携包是**本地签名机手动 `gh release upload`** 的。两者并行时，部署读到的 Release 资产还没有 MSI → `update-appcast.py` 报 `Error: provide at least one platform item (macOS ZIP or Windows MSI).`。

**解决**：等 MSI 上传完再手动重跑 `deploy-website.yml`。这是设计使然（release.yml 只收固件资产，MSI 必须事后补），但流程上要记住：**上传 MSI 后必须重跑一次网站部署**。

### 坑 3：Pages 未启用时 `curl appcast.xml` 404，但不致命

部署里 `curl -fsSL https://haobot.github.io/VoiceStickPlus/appcast.xml -o ... || true` 有兜底，404 不影响流程。用户需先在 Settings → Pages → Source 选 **GitHub Actions**。启用后 `gh api repos/Haobot/VoiceStickPlus/pages` 返回 `build_type: "workflow"`、`html_url: https://haobot.github.io/VoiceStickPlus/`。注意部署日志里 `Deploy to GitHub Pages` 的 `error_count: 10` 是 **Node 20 deprecation warning 误报**，不代表部署失败——以 `gh api .../pages` 和实际 `curl` 结果为准。

### 坑 4：Bash 工具里 `gh` 不在 PATH

`gh` 装在 `C:\Program Files\GitHub CLI\gh.exe`，Bash 工具与 PowerShell 的 PATH 都可能不含它。**必须用完整路径调用**：`& "C:\Program Files\GitHub CLI\gh.exe" ...`。Monitor/Bash 脚本里裸写 `gh` 会静默失败（`command not found`），导致监控循环永远等不到结果——排查 Monitor 超时先怀疑这个。

### 坑 5：PowerShell 读 octet-stream 给字节数组

`Invoke-WebRequest` 对 `application/octet-stream`（GitHub Release 资产）返回字节数组而非文本，直接 `.Content` 打印是一串数字。要用 `[System.Text.Encoding]::UTF8.GetString([byte[]]$bytes)` 转文本再 `ConvertFrom-Json`。

### 坑 6：build_win.bat 用 Bash 调用需绝对路径

`cmd //c build_win.bat` 会报「不是内部或外部命令」，因为 Bash 工具的工作目录或路径解析问题。用 `cmd //c "C:\Dev\FFE\George\voicestick\build_win.bat"`（引号 + 绝对路径）才行。

## 三、产物验证要点

- **exe 内嵌地址**：重建后必须核对 `VoiceStick.exe` 二进制里确实含新地址、无旧地址——用 PowerShell `[IO.File]::ReadAllBytes` + ASCII 搜索 `haobot`/`VoiceStickPlus`，确认无 `78.github`/`aliyuncs` 残留（`strings` 在 Windows 上不可靠）。
- **MSI 体积变化**：旧 MSI 23,478,272B → 新 MSI 23,474,176B（地址替换后正常），appcast 里 `length` 必须与上传的 MSI 实际字节数一致（本次 23474176）。
- **CI 固件 vs 本地固件体积不同**：CI 用同源码独立构建（merged 1,555,984 vs 本地 1,551,136），环境差异（bootloader 等）导致体积微差，**以 CI 产物为权威分发版本**。manifest.json 里 sha256 与实际资产匹配即可。
- **appcast 内容**：本次仅 Windows，appcast 应只含 `sparkle:os="windows"` 条目，url 指向 Haobot Release 的 MSI。
- **全链路 URL 验证**：逐个 `Invoke-WebRequest` 确认 200——appcast、`releases/latest/download/manifest.json`（302 → 资产）、MSI/便携包/固件 ota/merged 资产、网站首页、Release 页面。

## 四、本次发布产物

- Tag `v2.3.6`（轻量标签，无后缀），Release 非 draft 非 prerelease
- 资产：`VoiceStick_2.3.6.msi`（23.4MB，签名 Valid）、`VoiceStick_Portable_v2.3.6.zip`（28.2MB，含 FlashTool）、固件 ota/merged bin + sha256、`manifest.json`
- Pages：`https://haobot.github.io/VoiceStickPlus/`（appcast.xml 已含 v2.3.6 MSI 条目）
- 提交：`2f7c66f`（迁移）+ tag `v2.3.6`

## 五、后续事项

- **macOS 未发布**：本次禁用 macOS job，旧 macOS 客户端仍走旧作者 appcast/公钥，无法跨源升级，需用户手动换装。恢复 macOS 需回填 `release.yml` 的 `build-macos:` job 及其 Secrets（`SPARKLE_*`/`MACOS_*`/`APPLE_*`）并重新生成 Sparkle EdDSA 密钥对。
- **根治 deploy-website 无 Release 炸一次**：给 `gh release view` 加兜底。
- **便携包不参与自更新**：只有 MSI 安装版走 WinSparkle，便携版跳过了。
