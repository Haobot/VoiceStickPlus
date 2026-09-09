# RFC：腾讯 COS 国内分发面

- 日期：2026-09-09
- 状态：已实施（代码与 CI 链路落地，待 COS bucket/域名/Secrets 就绪后联调首次发布；实施将镜像目录 `msi/` 定名为 `windows/` 以涵盖便携版 zip）
- 范围：Windows 端 + 固件 OTA（核心）、网站整站国内化、macOS 端代码层预留

## 1. 背景与目标

VoiceStick 当前全部分发面托管在 GitHub：网站/appcast/downloads.json 在 github.io（Pages），MSI/固件 bin 在 github.com Release 资产。两者在国内网络 DNS 污染、间歇阻断、下载超时，导致国内用户「下载装不上、更新检查失败、固件 OTA 拉不动」。历史上存在过的阿里云 OSS 上传步骤已从 CI 移除（release.md 明确 "no Aliyun OSS upload"），国内通道实际缺失。

目标：以**腾讯云 COS 单 bucket + 已备案自有域名**为国内分发面，实现：

1. 自动构建发布后用户可从国内网站直接下载（网站整站 + 全部产物）；
2. 软件更新（WinSparkle/Sparkle appcast）与固件更新（manifest 检查）推送走国内源；
3. 本地执行更新（MSI 升级、BLE OTA 推固件）的下载流量走国内源。

非目标：

- 不引入 CDN 边缘加速（二期可选，本设计对 DNS 换指向前向兼容）；
- 不改变签名校验链（Sparkle EdDSA / MSI 代码签名 / 固件 sha256）；
- 不迁移 GitHub 侧链路——GitHub 保留为源站与国际/备胎渠道，Pages 继续部署；
- macOS CI job 维持禁用，仅代码层（SUFeedURL）就位；
- 不为存量用户做专门迁移机制（当前外部用户≈0，旧 feed 由现有 deploy workflow 继续更新，天然兜底）。

## 2. 总体架构

一个 COS bucket（公有读、私有写）绑定一个已备案域名（下文占位 `<dist-domain>`，如 `dl.example.com`），整站与分发产物同域、按路径分区：

```text
https://<dist-domain>/
├── index.html 等            # 整站（website/dist，Vite --base=/ 构建）
├── appcast.xml              # 更新 feed（Cache-Control: no-cache）
├── downloads.json           # 下载页数据（no-cache）
├── firmware/
│   ├── latest/manifest.json   # 稳定 manifest，客户端与浏览器烧录器共用（no-cache）
│   └── v<版本>/               # ota bin / merged bin / .sha256 / manifest.json（长缓存）
└── windows/v<版本>/           # 双语言 MSI + 便携包 + .sha256（由 CI 从 GitHub Release 镜像，长缓存）
```

职责划分：

- **GitHub Release 仍是唯一「源站事实」**：CI 构建产物、签名机上传的 MSI 都先进 Release；
- **所有 COS 写入由 GitHub Actions 完成**（coscli 上传），COS 写凭据只存在于 GitHub Secrets，签名机零新增工具；
- `release.ps1` 维持「只跟 GitHub 打交道」，verify 阶段扩展 COS URL 检查。

manifest schema 变更（向后兼容，新字段 optional）：

```json
{
  "hardware": "stick_s3",
  "version": "2.3.10",
  "min_version": "1.8.0",
  "ota_url": "https://<dist-domain>/firmware/v2.3.10/voicestick-firmware-sticks3-ota-2.3.10.bin",
  "ota_url_fallback": "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.10/voicestick-firmware-sticks3-ota-2.3.10.bin",
  "ota_sha256": "...", "ota_size": 123,
  "merged_url": "https://<dist-domain>/firmware/v2.3.10/voicestick-firmware-sticks3-merged-2.3.10.bin",
  "merged_url_fallback": "https://github.com/.../voicestick-firmware-sticks3-merged-2.3.10.bin",
  "merged_sha256": "...", "merged_size": 456
}
```

主 URL 指向 COS，`*_fallback` 指向 GitHub Release。旧 manifest 无 fallback 字段时解析端必须容错。

## 3. COS 基建准备清单（用户侧，实施前完成）

1. 创建 bucket（建议地域 `ap-shanghai`，公有读私有写），记录 bucket 名与地域；
2. 腾讯云申请免费 DV 证书，绑定自定义域名 `<dist-domain>` 并开启 HTTPS；
3. DNS：`<dist-domain>` CNAME 指向 bucket 公网域名；
4. 创建子账号并授予仅该 bucket 的写权限策略；SecretId/SecretKey 存入仓库 GitHub Secrets：`TENCENT_COS_SECRET_ID` / `TENCENT_COS_SECRET_KEY`；
5. bucket 名、地域、分发域名写入 workflow `env:` 常量（非密钥）。

## 4. 发布侧改动

### 4.1 `.github/workflows/release.yml`

- 「Stage firmware checksums and manifest」步骤中，manifest 的 `ota_url`/`merged_url` 直接生成 COS 域名 URL，新增 `ota_url_fallback`/`merged_url_fallback` 生成 GitHub URL（两处 URL 均由现有变量拼接，改动集中在 python 内联脚本）；
- 新增「Upload firmware to COS」步骤：CI 内下载 `coscli`（官方二进制），上传 `firmware/v<版本>/`（bin + sha256 + manifest）与 `firmware/latest/manifest.json`；manifest 设 `Cache-Control: no-cache` 元数据，bin 设长缓存（如 `max-age=31536000`）；
- COS 上传失败即 job 失败，不静默跳过（凭据未配置时同样失败——遵守「不伪造结果」红线，不用 SKIP 掩盖）。

### 4.2 `.github/workflows/deploy-website.yml`

- 新增「镜像 Release 资产到 COS」步骤：`gh release list` 取最近 N 个 release（N 与 `update-downloads.py --limit` 共用同一取值，默认 10，保证 downloads.json 列出的每个资产都已被镜像），逐资产（固件 bin/sha256/manifest、双语言 MSI）`gh release download` 后上传 COS 对应路径（`firmware/v<版本>/`、`windows/v<版本>/`）；幂等覆盖，任一预期内资产下载失败即 fail（保证 downloads.json 的确定性路径映射成立）；
- 「Update appcast from latest release」步骤：`update-appcast.py` 新增 `--mirror-base https://<dist-domain>`，MSI/zip 的 enclosure URL 指向 COS 路径（长度与签名字段逻辑不变）；
- 「Generate downloads.json」步骤：`update-downloads.py` 新增同样的镜像参数，各版本资产与 sha256 链接生成 COS URL；
- 「Build website」改为双次构建：`npm run build`（base=`/VoiceStickPlus/`，供 Pages artifact）与 `npm run build -- --base=/`（供 COS）；COS 版 `website/dist` 全量上传 bucket 根（appcast.xml/downloads.json 随 dist 天然在根，no-cache 元数据单列设置）；
- 「Sync firmware to Pages origin」步骤的 manifest 落点同步改为 `website/public/firmware/latest/manifest.json`（与烧录器、客户端统一为 `latest/` 路径；Pages 版 `merged_url` 重写逻辑不变）；
- Pages 部署步骤保持不变（备胎站继续服务）。

### 4.3 `scripts/release.ps1`

- 不新增 COS 上传步骤；
- verify 阶段扩展：对 COS 域名下的 appcast.xml、downloads.json、`firmware/latest/manifest.json`、本版本 ota/merged bin、en-US MSI 逐 URL `HEAD` 检查 200。

### 4.4 `scripts/update-appcast.py` / `scripts/update-downloads.py`

- 各增加 `--mirror-base` 参数；appcast 的 enclosure（MSI/zip）与 downloads.json 的资产/校验和链接，URL 一律替换为 `mirror-base` 下的确定性路径；
- 参数缺失时行为与现状一致（仅指 GitHub），便于本地脚本级测试。

## 5. 客户端改动

### 5.1 Windows 桌面端

- `desktop/windows/src/win32_app.cc`（`VOICESTICK_APPCAST_URL` 宏，现 L112）：改为 `https://<dist-domain>/appcast.xml`；单源、无运行时回退（WinSparkle 仅支持单 URL；存量≈0，且 github.io feed 继续更新作为兜底）；
- `desktop/windows/src/firmware_manifest.cc`：
  - `FirmwareManifestClient::DefaultManifestUrl()` 改为 COS `https://<dist-domain>/firmware/latest/manifest.json`；
  - manifest 拉取失败时回退 GitHub `releases/latest/download/manifest.json`（保留现值作为回退常量）；
  - `FirmwareManifest` 结构体与 `ParseFirmwareManifest` 增加 `ota_url_fallback` 解析（optional，缺失容错）；
  - OTA 下载：`DownloadOtaSync` 失败且 manifest 带 fallback 时用 fallback URL 重试；sha256/size 校验对两个源一视同仁（内容一致性由校验保证）。

### 5.2 macOS 桌面端

- `desktop/macos/Sources/VoiceStickApp/Info.plist` 的 `SUFeedURL` 改为 COS 域名 appcast；CI job 维持禁用，代码层就位即可。

### 5.3 网站

- 浏览器烧录器（esptool-js）fetch manifest 的路径统一为 `${BASE_URL}firmware/latest/manifest.json`（现无 `latest` 段，需同步改 fetch 路径）；
- `website/src/App.vue` / `downloads.js` 回退链保持：downloads.json 命中 → COS 资产 URL；加载失败或未命中 → GitHub Releases 直链（构建期逻辑不动）；
- Vite `base` 不改仓库配置（保持 `/VoiceStickPlus/`），COS 构建用 CLI `--base=/` 覆盖，双构建产物各自独立。

## 6. 错误处理与回滚

| 场景 | 行为 |
|---|---|
| CI 上传 COS 失败 / 凭据缺失 | job 红色失败，发布中断，不静默降级 |
| 客户端 manifest 拉取失败 | 回退 GitHub latest manifest URL |
| 客户端 OTA 下载失败 | 回退 manifest `ota_url_fallback`，校验不变 |
| appcast 不可达（COS 故障） | 单源不回退；用户经网站手动下载；网站也故障时 github.io 备胎站仍在 |
| COS 侧整体回滚 | revert workflow 即可，GitHub 链路全程未移除 |

## 7. 安全考量

- 签名链原样保留：macOS zip 的 Sparkle EdDSA、MSI 代码签名、固件 sha256/size 双校验；换源只换 URL 不换内容，不绕过任何校验；
- COS 子账号最小权限（仅目标 bucket 写）；凭据只存在于 GitHub Secrets 与 CI 运行时环境，不进仓库、不进签名机脚本；
- 公有读刷流量风险：不启用 Referer 防盗链（WinSparkle/WinHTTP 更新请求无 Referer，会误伤真实用户）；配置腾讯云 COS 流量监控告警，异常时人工介入；
- 全链路 HTTPS（bucket 自定义域名绑免费 DV 证书）。

## 8. 测试策略

- **TDD（Windows 单测）**：`desktop/windows/tests/core_tests.cc` 先扩展 `TestFirmwareManifestParsingAndVersionCompare`：fallback 字段存在 / 缺失 / 畸形三态解析断言（红）→ 实现 `ParseFirmwareManifest` 扩展（绿）；回退 URL 选择逻辑补独立用例；
- **脚本测试**：`update-appcast.py`/`update-downloads.py` 以固定输入（样例 release 元数据）本地运行，断言输出 XML/JSON 中 URL 指向 mirror-base 与确定性路径；无 `--mirror-base` 时输出与现状一致（回归）；
- **发布验收**：`release.ps1` verify 扩展的 COS URL HEAD 全绿；`Release Build` 与 `Deploy Website` workflow 成功；
- **真机验收**（发布后）：Windows 端 WinSparkle 检查更新→气泡→安装升级一次；固件更新对话框检查→下载（走 COS）→BLE OTA 推送成功；网站下载页国内网络直连下载 MSI 与固件 bin；
- 不伪造结果：无 COS 凭据/无设备时对应环节失败或 SKIP 并显式标注，不 mock 真实链路。

## 9. 风险与开放项

- `<dist-domain>` 具体值、bucket 名/地域待用户提供（实施前完成第 3 节清单）；
- COS 与 GitHub 双源内容一致性由 sha256 校验兜底，但镜像步骤失败即 fail 的策略要求 Release 资产命名严格稳定（现状已稳定）；
- COS 无 CDN 的直连速度对低频更新分发足够；网站首屏速度如需优化，二期接入腾讯云 CDN（DNS 换指向，bucket 结构与 URL 不变）；
- `latest/manifest.json` 与 `v<版本>/manifest.json` 双份维护由 CI 同一来源生成，无人工同步风险。

## 10. 验收标准

1. 国内网络（无代理）直连 `<dist-domain>`：网站可访问、下载页可下载最新 MSI 与固件；
2. 已装 Windows 客户端（新版本）检查更新走 COS appcast 并完成升级；
3. 固件更新检查与 OTA 下载走 COS，断 COS 后自动回退 GitHub 仍可完成更新；
4. `release.ps1` 一键发布后 verify 阶段所有 COS/GitHub URL 均返回 200；
5. GitHub Pages 与 Release 链路行为与现状一致（备胎可用）。
