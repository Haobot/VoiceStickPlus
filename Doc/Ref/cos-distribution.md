# 腾讯 COS 分发渠道约定（VoiceStick）

本文是 COS 国内分发渠道的**权威事实参考**，作为软件/固件侧与模型侧工作会话的交接媒介：模型侧从此文档读取桶信息回填 `model_manifest` 清单 URL 并做真机下载验证。方案背景见 `Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md`。

状态：2026-09-10 建立并完成首次模型上传；域名与凭据项见文末待办。

## 桶信息

| 项 | 值 |
|---|---|
| 桶名 | `voicestick-dl-1259040144`（含 APPID 后缀） |
| 地域 | `ap-shanghai` |
| 读写权限 | 公有读、私有写（写操作仅凭据方：CI 子账号 / 管理脚本） |

## 访问域名

| 域名 | 性质 | 状态 |
|---|---|---|
| `https://dl.davenger.cloud` | **COS 自定义域名，直出（非 CDN）**，已备案 | 规划正式域名；DNS 尚未配置（见待办） |
| `https://voicestick-dl-1259040144.cos.ap-shanghai.myqcloud.com` | COS 默认直出域名（腾讯通用证书，HTTPS 可用） | **立即可用**；正式域名就绪前的回退/联调地址 |

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

1. DNS：`dl.davenger.cloud` CNAME → `voicestick-dl-1259040144.cos.ap-shanghai.myqcloud.com`（当前 NXDOMAIN）；
2. 腾讯云申请免费 DV 证书，COS 控制台为桶绑定自定义域名 `dl.davenger.cloud` 并开启 HTTPS；
3. 创建最小权限 CAM 子账号（仅 `voicestick-dl-1259040144` 写权限），密钥入仓库 GitHub Secrets（`TENCENT_COS_SECRET_ID` / `TENCENT_COS_SECRET_KEY`）——配置前 CI 的 COS 步骤会如实失败；
4. 配置 COS 流量监控告警（替代防盗链的防护手段）；
5. 以上就绪后跑一次发布联调（`release.ps1` 或手动触发 `deploy-website.yml`），并从国内网络直连验证下载页、appcast、模型清单 URL。
