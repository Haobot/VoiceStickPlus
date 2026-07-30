# 把发布/自更新体系迁移到自己的 GitHub

本文记录把 VoiceStick 的桌面端自更新（Sparkle/WinSparkle + appcast）与发布流水线从原仓库 `78/voicestick` 迁移到自己 GitHub 账号的完整步骤。适用于 fork 后独立发布的场景。

假设你的账号为 `<你的用户名>`，仓库名保持 `voicestick`，则 Pages 地址为 `https://<你的用户名>.github.io/voicestick/`，appcast 地址为 `https://<你的用户名>.github.io/voicestick/appcast.xml`。

## 一、仓库准备

1. 把仓库推到自己的 GitHub。注意 `desktop/windows/` 被 `.gitignore` 整体忽略（历史提交用 `git add -f` 强制加入），直接 clone 本仓库再 `git remote set-url origin <你的仓库>` 推送最省事，不要重新 `git init`。
2. 仓库 Settings → Pages → Source 选 **GitHub Actions**（`deploy-website.yml` 用 `actions/deploy-pages` 部署）。
3. 仓库名如果**不是** `voicestick`，改 `website/vite.config.js:5` 的 `base: '/voicestick/'` 为你的仓库名。

## 二、替换硬编码地址（共 8 处）

| 文件 | 行（记录时点） | 改什么 |
|---|---|---|
| `desktop/windows/CMakeLists.txt` | 117 | `VOICESTICK_APPCAST_URL` 改为你的 appcast 地址 |
| `desktop/macos/Sources/VoiceStickApp/Info.plist` | 35 | `SUFeedURL` 改为你的 appcast 地址 |
| `desktop/macos/Sources/VoiceStickApp/AppConfig.swift` | 169 | `websiteURL` 改为你的网站地址 |
| `.github/workflows/deploy-website.yml` | 42, 58, 65 | appcast 与 Release 下载地址；建议改成 `${{ github.repository }}` 变量一劳永逸 |
| `scripts/update-appcast.py` | 76 | appcast `<link>` |
| `website/public/appcast.xml` | 5, 18 | 模板里的 `<link>` 和 enclosure `url` |
| `website/src/App.vue` | 10-12 | 官网上的 GitHub 仓库/下载链接 |
| `website/README.md`、`Doc/Ref/release.md` | — | 文档引用，可后改 |

## 三、生成自己的 Sparkle 密钥（macOS 自更新必需）

EdDSA 密钥对**不能复用原仓库的**（公钥内置在客户端、私钥在签名机，你没有原私钥）：

```sh
# 从 Sparkle 发布包（https://github.com/sparkle-project/Sparkle/releases）拿 generate_keys 工具
generate_keys   # 生成并打印公钥
```

- 公钥写入 `desktop/macos/Sources/VoiceStickApp/Info.plist` 的 `SUPublicEDKey`（紧邻 `SUFeedURL` 下方，目前是占位符）。
- GitHub 仓库 Settings → Secrets and variables → Actions 添加 Secrets：
  - `SPARKLE_PUBLIC_ED_KEY`、`SPARKLE_PRIVATE_ED_KEY`（`release.yml` 中 macOS 构建必需）。

## 四、其余 GitHub Secrets（按需要的平台配）

- **macOS 签名/公证**：`MACOS_CERTIFICATE_P12`、`MACOS_CERTIFICATE_PASSWORD`、`APPLE_ID`、`APPLE_TEAM_ID`、`APPLE_APP_SPECIFIC_PASSWORD`。没有付费 Apple 开发者账号时 `release.yml` 的 macOS job 会失败，需要裁剪 workflow 或接受不签名产物。
- **固件 OSS**：Secrets `ALIYUN_OSS_ACCESS_KEY_ID` / `ALIYUN_OSS_ACCESS_KEY_SECRET` / `ALIYUN_OSS_ENDPOINT` / `ALIYUN_OSS_BUCKET`；Variables `ALIYUN_OSS_PUBLIC_BASE_URL` / `ALIYUN_OSS_PREFIX`。固件 manifest 地址硬编码在两处客户端：
  - `desktop/macos/Sources/VoiceStickApp/AppConfig.swift:170-172`
  - `desktop/windows/src/firmware_manifest.cc:209`
  
  **不想用阿里云的纯 GitHub 方案**：把这两处指向 GitHub Release 资产（`https://github.com/<你的用户名>/voicestick/releases/latest/download/manifest.json`），并删除 `release.yml` 中 OSS 上传步骤及对应 secrets 校验。改动最小，推荐。
- **Windows 签名**：不走 CI。在本地签名机跑 `scripts\build-msi.bat`（无证书时用 `scripts\build-msi-unsigned.bat` 验证流程），手动把 MSI 传到对应 GitHub Release，再手动触发 `Deploy Website to GitHub Pages` 工作流收录 MSI 条目到 appcast。

## 五、验证

1. 推一个与 `VERSION` 匹配的 `v<版本号>` 标签 → `release.yml` 跑通并产出 Release。
2. `deploy-website.yml` 自动更新 `website/public/appcast.xml` 并部署 Pages（或手动触发）。
3. `curl https://<你的用户名>.github.io/voicestick/appcast.xml` 应看到带真实签名/真实长度的条目。
4. 客户端点「检查应用更新」能发现新版本。

## 六、关键注意点

- **已发出的旧客户端内置的是原作者的 appcast 地址和公钥**，无法跨源升级；用户需手动安装一次你的版本，之后才能走你的自更新链路。
- Windows 便携版（portable zip）跳过 WinSparkle 初始化，本身不支持自更新，只有 MSI 安装版参与该链路。
- 仓库内 `website/public/appcast.xml` 是模板，线上状态由 CI 每次发布时先拉回线上文件再重写，不以仓库内文件为准。
- 固件 OTA（BLE OTA + OSS manifest）与桌面端 Sparkle 自更新是两套独立链路，迁移时互不影响，但第四节的两处 manifest 地址要一起改。
