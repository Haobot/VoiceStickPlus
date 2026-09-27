---
name: release-publish
description: >-
  VoiceStick 发布操作全流程（GitHub Release + COS + 静态网页三渠道同步）。当用户说"发布 / 发新版 /
  发布到 COS 和 GitHub / 更新到网页 / 发固件 / 发程序"等，或固件/程序修改经真机验证后需要发版时使用。
  涵盖完整发布、仅固件轮、仅程序轮三种轮次，发行说明手写约定，静态网页整站同步配方，终验清单与全部已踩坑。
  权威流程文档 Doc/Ref/release.md；整站同步坑详见 Doc/Expe/cos-website-fullsite-sync-traps-2026-09-27.md。
---

# Skill: release-publish（发布操作）

发布渠道：GitHub Release（国际主源）+ COS `dl.davenger.cloud`（国内主源，桶 `voicestick-dl-1329978361`/ap-shanghai）+ 静态网页（GitHub Pages + COS 镜像整站）。执行环境 = Windows 签名机（COS 写只能在本机，CI 跨境上行必死）。

## 铁律（动手前先读）

1. **版本号必须 bump**（改根目录 `VERSION` + `firmware/version.txt`，纯文本无换行，两处同步）：设备 OTA 升级靠版本号比对，同版本覆盖发布 = 已装设备永远收不到新固件。
2. **本项目在 `feat/stick-gateway` 分支手工分步发布**（`release.ps1` 有 main 硬限制；v2.4.1 起惯例均手工分步，本 skill 即手工配方）。发布前工作区必须干净（`git status --porcelain` 无输出）、分支已推送。
3. **upload 只增不删**：COS `firmware/latest/` 的旧版本 bin 必须显式删除；本地 `dist/` 每次发布前 `rm -rf dist` 重建（防旧 `.sha256` 残留被原样上传——v2.4.2 曾双侧中招）。
4. **appcast 依赖 Release 里的 MSI 资产**：仅固件轮想刷网页前，必须先把 MSI 补传 Release，否则 appcast 生成残缺、WinSparkle 指向 404。
5. **CI 门禁必须真看结论**（`completed success`），continue-on-error 的 run 不算过。
6. GitHub 侧验证一律用 `gh api` / `gh release download`（走 API 链路）；curl 直连 github.com 国内超时属常态，不代表发布失败。
7. **MSI 必须内置凭据（内部测试模式）**：`build-msi.bat` 已默认注入本机 config.toml 凭据（开箱即用免输 API Key；显式 `VOICESTICK_EMBED_BUILTIN_KEYS=0` 才构建无凭据公开包）。构建日志必须出现 `Injecting built-in credentials into VoiceStick.exe`——若见 `building WITHOUT built-in credentials` 产物作废重建；发布扫描用 `--allow-builtin`（内测包命中仅告警，无它则嵌入凭据必 FAIL）。

## 发布轮次决策

```
改了什么？
├─ 固件 + 程序都改 → 流程 A：完整发布（全部步骤）
├─ 仅固件改       → 流程 B：仅固件轮（跳过 MSI 构建，但静态网页同步为必做项，
│                    且要先补 MSI 见「静态网页同步」第 0 步）
└─ 仅程序改       → 流程 C：仅程序轮（跳过固件构建；manifest 沿用旧版属预期，
                    见 Doc/Ref/release.md「SkipFirmware」说明）
```

## 流程 A/B 通用步骤（固件侧）

```sh
# 1. bump 版本并提交
printf 'X.Y.Z' > VERSION && printf 'X.Y.Z' > firmware/version.txt
git add VERSION firmware/version.txt && git commit -m "chore(release): 版本升级 A.B.C → X.Y.Z（仅固件：…；Windows MSI 无变更）"

# 2. 构建固件（OTA bin + merged 整包）
python scripts/idf_cli.py -c --merge          # 产物 firmware/build/voice_stick.bin、merged.bin

# 3. 收集 dist 产物（先清空防残留）并生成 .sha256 + manifest.json（未上传）
rm -rf dist && mkdir -p dist
cp firmware/build/voice_stick.bin dist/voicestick-firmware-sticks3-ota-X.Y.Z.bin
cp firmware/build/merged.bin       dist/voicestick-firmware-sticks3-merged-X.Y.Z.bin
python scripts/publish_cos.py firmware --dist dist --version X.Y.Z --min-version "$(cat FIRMWARE_MIN_VERSION)" --skip-upload
```

```sh
# 4. push 分支 + tag（tag 触发 release.yml 构建验证门禁）
git push origin feat/stick-gateway
git tag -a vX.Y.Z -m "VoiceStick X.Y.Z" && git push origin vX.Y.Z
gh run list --repo Haobot/VoiceStickPlus --workflow release.yml --limit 1   # 轮询至 completed success

# 5. P0-4 凭据扫描（内测包 --allow-builtin：命中仅告警；无它则嵌入凭据必 FAIL）
#    → 创建 Release → 上传 5 个资产
python scripts/scan_release_artifacts.py --allow-builtin dist/<ota.bin> dist/<ota.bin.sha256> \
    dist/<merged.bin> dist/<merged.bin.sha256> dist/manifest.json
gh release create vX.Y.Z --repo Haobot/VoiceStickPlus --title "VoiceStick X.Y.Z" --generate-notes
gh release upload vX.Y.Z dist/* --repo Haobot/VoiceStickPlus --clobber   # 失败重试一次

# 6. COS 直传（固件 v<tag>/ + latest/manifest.json）
python scripts/publish_cos.py --bucket voicestick-dl-1329978361 --region ap-shanghai \
    firmware --dist dist --version X.Y.Z --min-version "$(cat FIRMWARE_MIN_VERSION)"
```

## 流程 A/C 步骤（Windows MSI 侧）

```powershell
# 1. 构建签名双语 MSI（脚本全自动：构建→签名 exe→WiX 双语→签名→verify_msi_contents 门禁）
#    必须用 PowerShell 包装调 bat（MSYS 下 cmd /c 吞参数）；exit code 可能误报 1，
#    以产物时间戳 + 签名 Valid 为准（Get-AuthenticodeSignature）
#    ⚠️ 构建日志必须出现 "Injecting built-in credentials into VoiceStick.exe"——
#    内部测试模式默认注入本机 config.toml 凭据；见 "building WITHOUT built-in
#    credentials" 即产物作废（缺 API Key，用户升级后被要求手动输入），禁止发布
cmd /c 'scripts\build-msi.bat'
# 产物 desktop\windows\build-msi-x64\VoiceStick_X.Y.Z_zh-CN.msi / _en-US.msi

# 2. 生成 .sha256 → P0-4 扫描（--allow-builtin，内测包仅告警）→ 上传 Release → COS
#    sha256 格式：<hash>  <文件名>（两空格+换行）
python scripts/scan_release_artifacts.py --allow-builtin `
    desktop/windows/build-msi-x64/VoiceStick_X.Y.Z_zh-CN.msi `
    desktop/windows/build-msi-x64/VoiceStick_X.Y.Z_zh-CN.msi.sha256 `
    desktop/windows/build-msi-x64/VoiceStick_X.Y.Z_en-US.msi `
    desktop/windows/build-msi-x64/VoiceStick_X.Y.Z_en-US.msi.sha256
gh release upload vX.Y.Z <4 个 MSI 文件> --repo Haobot/VoiceStickPlus --clobber
python scripts/publish_cos.py --bucket voicestick-dl-1329978361 --region ap-shanghai `
    software --msi-dir desktop/windows/build-msi-x64 --version X.Y.Z
```

## 静态网页同步（每轮发布后必做，漏做 = 网页停在旧版）

```sh
# 0.（仅固件轮）appcast 需要 MSI 资产——先按流程 A/C 补构建上传当前版本 MSI

# 1. 刷 Pages（appcast/downloads.json/烧录器固件，workflow 从 latest release 动态拉取）
#    必须用 --ref feat/stick-gateway（main 停旧点，不含 zh-CN appcast 逻辑）
gh workflow run deploy-website.yml --repo Haobot/VoiceStickPlus --ref feat/stick-gateway
#    轮询至 completed success

# 2. COS 整站（含烧录器同源固件）：更新 public 静态文件 → --base=/ 构建 → 整站上传
#    2a. website/public/firmware/latest/ 放当前版本 merged bin + manifest
#        （manifest 的 merged_url 重写为 https://dl.davenger.cloud/firmware/latest/<file> 同源）
#        删掉该目录下旧版本 bin（未跟踪的本机部署产物，会被原样打进 dist）
#    2b. website/public/appcast.xml + downloads.json 从 Pages 下载最新版覆盖
#    2c. 构建——必须 PowerShell + --base=/（vite.config 默认 base=/VoiceStickPlus/ 为 Pages
#        设计；MSYS 下裸跑或传 / 会被路径转换 → COS 全资源 404 白屏）
powershell -NoProfile -Command "cd website; npm run build -- --base=/"
#    2d. 整站上传（upload_files 逐个传，任一失败抛异常终止）
python -c "import sys; sys.path.insert(0,'scripts'); from cos_uploader import require_credentials, collect_dir, upload_files; require_credentials(); upload_files('voicestick-dl-1329978361','ap-shanghai', collect_dir('website/dist',''))"
#    2e. 显式删 COS firmware/latest/ 下非当前版本的 bin（qcloud_cos client.delete_object）

# 3. pages-mirror 转传 COS（appcast/downloads.json，幂等）
python scripts/publish_cos.py --bucket voicestick-dl-1329978361 --region ap-shanghai \
    pages-mirror --pages-base https://haobot.github.io/VoiceStickPlus
```

## 发行说明（generate-notes 太简略，必须手工补）

- `gh release edit vX.Y.Z --repo Haobot/VoiceStickPlus --notes-file <文件>`——中文、分类条目（新增/改进/修复/说明，参照 v2.4.2 / v2.4.4 风格），写用户可见变化；程序无功能变化要如实说明「版本号对齐」。
- **改 notes 后必须重跑 deploy-website + pages-mirror**：appcast 的 `release-notes` 字段生成自 Release body，WinSparkle 弹窗显示的就是它。

## 终验清单（逐项过，全绿才算完成）

```sh
# 1. manifest 版本：COS latest 与 GitHub releases/latest 均 = X.Y.Z
curl -s https://dl.davenger.cloud/firmware/latest/manifest.json | grep version
gh api repos/Haobot/VoiceStickPlus/releases/latest --jq '.tag_name, (.assets[].name)'

# 2. bin SHA 三方一致：本地 dist / COS 实下载 / manifest ota_sha256
curl -s -o /tmp/t.bin https://dl.davenger.cloud/firmware/vX.Y.Z/<ota.bin> && sha256sum /tmp/t.bin

# 3. COS MSI HEAD 200；Release 资产 = 5 固件件 + 4 MSI 件（完整轮）
curl -s -I https://dl.davenger.cloud/software/windows/vX.Y.Z/VoiceStick_X.Y.Z_zh-CN.msi

# 4. 网页（注意 grep 全路径 [^"]*assets/，用 assets/ 模式会截前缀漏检 base 错误）
curl -s https://dl.davenger.cloud/ | grep -o -a '[^"]*assets/index-[A-Za-z0-9_-]*\.\(css\|js\)'
#    → 引用必须无 /VoiceStickPlus/ 前缀；对引用的 JS 实下载 grep 当前版本字面量、旧版本串 0 处
# 5. 烧录器固件：firmware/latest/manifest.json version 正确且 merged_url 同源；
#    latest/<当前版本 merged>.bin 200、latest/<旧版本>.bin 404
# 6. appcast：COS appcast.xml version=X.Y.Z + zh-CN enclosure（macOS 0.3.x 历史 item 无害）
```

## 已踩坑速查

| 坑 | 处理 |
|---|---|
| MSYS 下 `cmd /c` 调 bat 吞参数/横幅 | PowerShell 包装；exit code 误报以产物时间戳+签名状态为准 |
| MSYS 下传 `--base=/` 被转成 Git 安装路径 | PowerShell 里跑 `npm run build -- --base=/` |
| 只更新数据文件不重建网页 | 版本号经 `VERSION?raw` 构建时内联进 bundle，页面默认下载 URL 永远旧版 |
| 验证引用用 `assets/...` grep 模式 | 会截掉 `/VoiceStickPlus/` 前缀漏检 base 错误，必须 `[^"]*assets/` |
| 重建 MSI 后旧 `.sha256` 残留被原样上传 | 每次 `rm -rf dist` 重建；MSI 目录同理注意 |
| 链接 VoiceStick.exe（LNK1104） | 构建前杀运行中进程；build-msi-x64 与开发 build-x64 目录独立 |
| signtool `0x80093102` 随机失败 | Defender 实时扫描竞态，SignAndVerify 已内置重试；再失败等几秒重跑 |
| 设备 OTA 后功能消失 | 先查 app 日志 `gateway_status` 看设备上报 mode（状态唯一可信源），再怀疑代码 |
| 发布过渡窗口（页面引用新 assets 未就位瞬时 404） | 正常现象，用户强刷即愈；无需预热机制 |
| COS 凭据缺失 | `TENCENT_COS_*` / `TENCENTCLOUD_*` 环境变量（本机 Git Bash 通常已带；后备 winreg 读 HKCU\Environment 注入） |

## 相关文件

- `scripts/release.ps1`（全编排参考，main 限制）· `scripts/publish_cos.py` + `scripts/cos_uploader.py`（COS 直传）· `scripts/scan_release_artifacts.py`（P0-4 门禁）· `scripts/build-msi.bat` + `scripts/verify_msi_contents.ps1`（MSI 门禁）· `scripts/update-appcast.py` / `update-downloads.py`（Pages 侧生成）
- 文档：`Doc/Ref/release.md`（权威流程）· `Doc/Ref/cos-distribution.md`（渠道约定）· `Doc/Expe/cos-website-fullsite-sync-traps-2026-09-27.md`（整站三坑定案）· `Doc/Expe/msi-missing-sherpa-dll-defender-sign-flake-2026-09-27.md`（MSI 门禁由来）
