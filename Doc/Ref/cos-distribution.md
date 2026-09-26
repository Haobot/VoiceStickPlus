# 腾讯 COS 分发渠道约定（VoiceStick）

本文是 COS 国内分发渠道的**权威事实参考**，作为软件/固件侧与模型侧工作会话的交接媒介：模型侧从此文档读取桶信息回填 `model_manifest` 清单 URL 并做真机下载验证。方案背景见 `Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md`。

状态：2026-09-10 建立并完成首次模型上传；**2026-09-26 桶切换**——实际使用的腾讯云账号 APPID 为 1329978361，新建桶 `voicestick-dl-1329978361` 接替旧桶 `voicestick-dl-1259040144`（旧桶属另一账号，弃用，其中的模型对象需迁到新桶）；DNS 与 GitHub Secrets 已于同日配置。剩余项见文末待办。

## 桶信息

| 项 | 值 |
|---|---|
| 桶名 | `voicestick-dl-1329978361`（含 APPID 后缀） |
| 地域 | `ap-shanghai` |
| 读写权限 | 公有读、私有写（写操作仅凭据方：CI 子账号 / 管理脚本） |

## 访问域名

| 域名 | 性质 | 状态 |
|---|---|---|
| `https://dl.davenger.cloud` | **COS 自定义域名，直出（非 CDN）**，已备案 | 规划正式域名；DNS 已解析（CNAME → 新桶默认域名），**证书与自定义域名绑定待完成**（见待办） |
| `https://voicestick-dl-1329978361.cos.ap-shanghai.myqcloud.com` | COS 默认直出域名（腾讯通用证书，HTTPS 可用） | **立即可用**；正式域名就绪前的回退/联调地址 |

模型侧回填清单 URL 的建议：直接回填正式域名 `https://dl.davenger.cloud/models/...`（域名是访问层，DNS/证书就绪后无需改清单）；联调验证期可临时用 myqcloud 直出域名。

## 桶内三分前缀布局

```text
/（bucket 根）
├── index.html、appcast.xml、downloads.json 等   # 整站（deploy-website.yml --base=/ 构建同步）
├── software/                                     # 软件产物（CI 从 GitHub Release 镜像，幂等覆盖）
│   ├── windows/v<版本>/                          #   Windows MSI（双语言）+ 便携包 + .sha256
│   └── macos/v<版本>/                            #   macOS Sparkle ZIP / DMG
├── firmware/                                     # 固件（release.yml 构建后直传）
│   ├── v<版本>/                                  #   ota bin / merged bin / .sha256 / manifest.json
│   └── latest/manifest.json                      #   稳定地址，桌面端与浏览器烧录器共用
└── models/                                       # 本地模型（人工/专用脚本上传，CI 不管）
    └── sense-voice-int8-2024-07-17/
        ├── model.int8.onnx
        ├── tokens.txt
        └── Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf
```

镜像路径规则（GitHub Release 资产 → COS 键）的唯一实现是 `scripts/mirror_urls.py`，`update-appcast.py` / `update-downloads.py` / `mirror_release_to_cos.py` 共用；改动布局必须先改它的单测（`scripts/test_mirror_urls.py`）。

## 模型对象清单（模型侧回填用）

对象键与 `%LOCALAPPDATA%\VoiceStick\models\` 下的目录结构完全一致。完整性由桌面端下载器做 SHA-256 校验把关（COS 的 ETag 不是 SHA-256，勿比对）。

| 对象键 | 字节数 | SHA-256 |
|---|---:|---|
| `models/sense-voice-int8-2024-07-17/model.int8.onnx` | 239233841 | `c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51` |
| `models/sense-voice-int8-2024-07-17/tokens.txt` | 315894 | `f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc` |
| `models/sense-voice-int8-2024-07-17/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf` | 1107409472 | `b139949c5bd74937ad8ed8c8cf3d9ffb1e99c866c823204dc42c0d91fa181897` |

三个对象已上传并核验（2026-09-10）：字节数逐对象核对一致，源文件上传前本地 SHA-256 已复核匹配上表。`Cache-Control: max-age=31536000`（版本化不可变内容，长缓存）。

模型侧消费端已接入（2026-09-10）：桌面端 `model_manifest` 清单 URL 回填四源回退——`dl.davenger.cloud` 正式域名直出（DNS 就绪前快速失败自动回退，无需改清单）→ myqcloud 直出 → ModelScope 免费分流 → GitHub Release。真机验证：删缓存全量重下 1.34GB，三文件均经 myqcloud 直出完成，下载器 SHA-256 校验与独立复核均与上表一致；Range 206 断点续传能力已预检。离线分发包由 `scripts/pack_local_models.py` 打包（`voicestick-models-full-v1.zip`，解压到 `%LOCALAPPDATA%\VoiceStick\models\` 即可），zip 上传 COS `models/` 顶层待发布节奏。

上传复用入口：`scripts/cos_uploader.py`（`upload_files`/`collect_dir`），凭据走环境变量 `TENCENT_COS_SECRET_ID` / `TENCENT_COS_SECRET_KEY`。

## CORS 与防盗链策略（硬性约束）

- **Referer 防盗链：全桶禁止配置**（含 models/、software/、firmware/）。桌面端下载链路（模型下载器、WinSparkle、固件 WinHTTP 客户端）UA 为 `VoiceStick/1.0`，**不发送 Referer**，任何 Referer 白名单都会直接 403。防护依赖公有读 + 腾讯云 COS 流量监控告警（异常刷量人工介入），不用防盗链。
- **CORS：暂不配置。** 当前无跨域场景——整站与浏览器烧录器（esptool-js）同源在 `dl.davenger.cloud` 下，GitHub Pages 备胎站的烧录器 fetch Pages 同源资产。若未来 Pages 版站点需直连 COS，仅为对应域名追加 GET 只读 CORS 规则。
- 传输全 HTTPS（自定义域名绑腾讯云免费 DV 证书后；myqcloud 默认域名自带通用证书）。

## 写权限与凭据

- CI（GitHub Actions）写 software/、firmware/、整站：仓库 Secrets `TENCENT_COS_SECRET_ID` / `TENCENT_COS_SECRET_KEY`（**待配置**，见下）。按最小权限原则应为专用 CAM 子账号（仅此桶写权限），不要使用主账号凭据。
- models/ 上传为人工/专用脚本操作（不走 CI），同样通过上述凭据形态。
- 签名机 `release.ps1` 不直连 COS，只与 GitHub 交互。

## 剩余待办（渠道完全就绪前）

1. ~~DNS：`dl.davenger.cloud` CNAME → 桶默认域名~~ **已完成**（2026-09-26，指向 `voicestick-dl-1329978361.cos.ap-shanghai.myqcloud.com`）；
2. ~~腾讯云申请免费 DV 证书，绑定自定义域名并开启 HTTPS~~ **已完成**（2026-09-26：证书 `b6l8Zl22` 签发并托管到 COS 自定义域名，强制 HTTPS 已开，`https://dl.davenger.cloud` 全链路实测通过。注意两点：证书控制台「已签发」还需完成「托管/去托管」才会真正下发到 COS 前端，实测部署传播约需 10~20 分钟逐节点收敛；期间部分节点回通用证书属中间态）——桶内尚缺的对象（v2.4.0 bin/MSI、index.html，CI 时代从未成功镜像）待本机直传补齐；
3. ~~CAM 子账号与仓库 Secrets~~ Secrets 已配置（2026-09-26）；~~CI 上行路径不可用~~ **修法已落地（2026-09-26）**：CI 海外 runner 跨境上行不可用（~8KB/s + ~130s 断连，≥1MB 分块必死；两轮诊断 run 36244545602 / 36245561012，与 CAM/桶策略无关），故 COS 写入全部移交签名机：发布流程 `scripts/release.ps1` → `scripts/publish_cos.py`（固件/软件/manifest 直传 + Pages 小文件转传 + 整站同步；凭据 `TENCENT_COS_*` 或 `TENCENTCLOUD_*` 环境变量），CI 侧 COS 步骤已删除（`release.yml` 瘦身为构建验证门禁、`deploy-website.yml` 只管 Pages）。云侧 URL 拉取（COS 迁移任务/云函数境内中转）保留为可选自动化增强；**剩余：首次真实发布跑通全链路**；
4. **模型对象迁移**：旧桶 `models/` 下三个已核验对象需重新上传到新桶（本机国内网络直传，`scripts/publish_cos.py` 或 `scripts/cos_uploader.py` 均可，`Cache-Control: max-age=31536000`），上传后按上表逐对象核对字节数与 SHA-256；
5. 配置 COS 流量监控告警（替代防盗链的防护手段）；
6. ~~联调~~ **已跑通（2026-09-26/27，v2.4.1）**：COS 先行（本机构建+直传+验证）→ GitHub Release v2.4.1（tag 打在 feat/stick-gateway，CI 构建验证绿，9 资产）→ deploy-website（feat 分支 ref，Pages 更新）→ pages-mirror 转传 COS。终验：正式域名 9 URL 全 200、downloads.json/appcast 双侧 latest=2.4.1、SHA 抽验一致。经验：真跑时若非 release.ps1 一键流程，注意 MSYS 下 bat 需 PowerShell 包装、环境变量以注册表为准、downloads.json 顶层键为 releases/latest。
