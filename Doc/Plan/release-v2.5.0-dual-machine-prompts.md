# v2.5.0 双机发布提示词（Windows 签名机 + Mac）

> 用途：把下面两段提示词分别交给两台机器上的 AI，协同发布 v2.5.0。
> 本文件未提交入库；拷给 Windows 后可删除或提交（发布前工作区须干净）。

## 给用户的使用说明

1. **先启动 Mac 侧**（Mac 是发布驱动方：bump / tag / 门禁 / 建 Release / mac+固件资产）。
2. 把「提示词 A」整段拷给 Windows 签名机的 AI——它在收到信号 A（tag 已推）之前会自旋等待，不会乱动。
3. 两台机器不需要直接通信：**所有握手都走 GitHub 可观测状态**（tag、Release 资产数）。
4. 若想让 Windows 经 git 拿到本文件，先 `git add Doc/Plan/... && git commit` 再推 main。

## 四个已拍板的决策（两份提示词一致）

| 决策 | 结论 | 依据 |
|---|---|---|
| 发布分支 | **main**（不是 feat/stick-gateway） | main 含全部 153 笔改动；feat/stick-gateway 严格落后 150 笔、无独有提交 |
| Release 创建方 | **Mac 用 gh 直接创建**（不走 publish-mac.yml stage/finalize 轨道） | v2.4.10 先例：Haobot 直接 gh 建 Release（2026-10-14），12 项资产，无 presign；本机 gh 已实测可用（2026-11-03）；预签名轨道是「Mac 无 gh」时代的方案，已过时 |
| 网页同步（Pages + COS 整站 + pages-mirror） | **全部 Windows 单边执行** | COS 写只能在有凭据的 Windows 机；Pages dispatch 在 main 上近期多笔成功 |
| bump + tag 先手 | **Mac 唯一执行** | tag 是双方共同等待信号；Windows 全程不 push git |

## 时序总览

```
Mac: bump→push main ──→ tag v2.5.0 ──→ 等 release.yml 门禁(~3min) ──→ 构建 mac 包 ──→ gh release create + 传 8 件
      └──信号A：tag+bump 可观测──→ Windows: 同步代码 → build_win + CTest(硬门禁) → MSI 构建+扫描
                                        └──信号B：Release 存在且 8 件──→ gh release upload 4 件 MSI
                                                  → COS 固件/MSI/mac 件 → 网页三件套（Pages dispatch + COS 整站 + pages-mirror）
Mac: 等 Release 12 件全齐 ──→ 终验（全渠道只读）──→ 双方汇报
```

---

## 提示词 A：Windows 签名机（Windows 侧）

````markdown
# 任务：VoiceStick v2.5.0 发布——Windows 签名机侧

你在 Windows 签名机上执行 v2.5.0 发布的 Windows 侧工作。这是一次双机协同发布：Mac 主机上的
AI 负责版本 bump、tag、CI 门禁、创建 GitHub Release 并上传 macOS+固件资产；你负责
Windows MSI、COS 全量直传（固件/MSI/mac 件）与网页三件套同步。动手前先读仓库内
AGENTS.md、Doc/Ref/release.md、.agents/skills/release-publish/SKILL.md。

## 背景事实（动手前逐条核对，不符就停下问用户）

- 目标版本 **2.5.0**；发布前 VERSION=firmware/version.txt=2.4.10。bump 由 Mac 做，你不同步改。
- GitHub 仓库 = `Haobot/VoiceStickPlus`（github remote）。**gitee 是平行 remote，本轮绝不 push**。
- 发布分支 = **main**。你全程**不做任何 git push**；同步只 fetch/merge。
- GitHub Release v2.5.0 由 Mac 创建——你只 `gh release upload`，**绝不 `gh release create`**。
- 固件由 release.yml（tag 触发）在 CI 构建并由 Mac 传上 Release——**你不构建固件**，
  COS 固件件从 Release 下载后再传。
- `scripts/release.ps1` 本轮**禁止使用**（会自建 Release/全量 COS，与双机分工互踩）。
- ci.yml 在 main 上预存红（2026-09-27 起），不是阻塞项；你的硬门禁是本地 CTest。
- 本机有 gh（已登录）与 TENCENT_COS_* 凭据。
- **所有 gh 命令必须显式 `--repo Haobot/VoiceStickPlus`**（不依赖 git remote 名解析，
  防止 origin 指向 gitee 时 gh 操作错仓库）。

## 时序与等待信号（对方是 Mac 主机上的 AI，靠 GitHub 状态握手；等不到就汇报，别发明降级路径）

| 信号 | 含义 | 怎么观测 |
|---|---|---|
| A | Mac 已推 bump（VERSION=2.5.0）+ tag v2.5.0 | `git ls-remote <github-remote> refs/tags/v2.5.0` 非空 → 开始步骤 1 |
| B | Release 已建立且 mac/固件 8 件齐 | `gh release view v2.5.0 --repo Haobot/VoiceStickPlus --json assets --jq "[.assets[].name]|length"` ≥ 8 → 才能做步骤 4 |

轮询间隔 60~90 秒；任一信号等超过 45 分钟未出现 → 停下向用户汇报「在等什么、已等多久」。

## 步骤

### 0. 收到信号 A 后：同步代码
- `git remote -v` 确认 github remote（`git@github.com:Haobot/VoiceStickPlus.git`）；
  没有就 `git remote add origin-github git@github.com:Haobot/VoiceStickPlus.git` 并用它。
- `git fetch <github-remote> && git checkout main && git merge --ff-only <github-remote>/main`
  （本地 main 有残留无法 ff：先向用户确认再 `git reset --hard <github-remote>/main`）。
- 核对：`type VERSION` 输出 2.5.0；bump commit 在 `git log -3` 内；`git status` 干净。

### 1. 构建 + CTest（MSI 前置硬门禁）
```bat
taskkill /IM VoiceStick.exe /F 2>nul
build_win.bat
ctest --test-dir desktop\windows\build-x64 --output-on-failure
```
⚠️ main 含语音编辑批次 1/2 的 Windows 平台代码（两笔 merge 信息标注「待 Windows 机
MSVC/CTest」），**本轮 CTest 是它首次真工具链验证。CTest 红 = 停止发布**，把失败输出
原样汇报给用户等指示；绿了才继续。

### 2. 构建签名双语 MSI
```bat
cmd /c "scripts\build-msi.bat"
```
- 也可 PowerShell 包装（`powershell -NoProfile -Command "scripts\build-msi.bat"`）；
  exit code 可能误报 1，以产物时间戳 + `Get-AuthenticodeSignature` 状态 Valid 为准。
- **凭据门禁**：构建日志必须出现 `Injecting built-in credentials into VoiceStick.exe`；
  出现 `building WITHOUT built-in credentials` = 产物作废，禁止发布。
- 产物：`desktop\windows\build-msi-x64\VoiceStick_2.5.0_zh-CN.msi` / `_en-US.msi`。
- 生成校验（Git Bash，两空格格式）：
  `cd desktop/windows/build-msi-x64 && sha256sum VoiceStick_2.5.0_zh-CN.msi > VoiceStick_2.5.0_zh-CN.msi.sha256`（en-US 同理）。
- P0-4 扫描（--allow-builtin 必带，内测包命中仅告警）：
  `python scripts/scan_release_artifacts.py --allow-builtin <上述 4 个文件>`。
- 坑：LNK1104/文件占用→先杀 VoiceStick.exe；signtool 0x80093102→等几秒重跑（Defender 竞态）。

### 3. 等 Signal B → 上传 MSI 到 Release（只传这 4 个，不动其它资产）
```bat
gh release upload v2.5.0 desktop\windows\build-msi-x64\VoiceStick_2.5.0_zh-CN.msi desktop\windows\build-msi-x64\VoiceStick_2.5.0_zh-CN.msi.sha256 desktop\windows\build-msi-x64\VoiceStick_2.5.0_en-US.msi desktop\windows\build-msi-x64\VoiceStick_2.5.0_en-US.msi.sha256 --repo Haobot/VoiceStickPlus --clobber
```
（失败重试一次；此时 Release 资产应达 12 件）

### 4. COS 直传（全部 COS 写都是你的；Mac 无凭据不碰 COS）

4a. 固件（v2.5.0/ + latest/manifest.json，dist 必须先清空防旧 .sha256 残留）：
```bash
rm -rf dist && mkdir -p dist
gh release download v2.5.0 --repo Haobot/VoiceStickPlus \
  --pattern "voicestick-firmware-sticks3-ota-2.5.0.bin" \
  --pattern "voicestick-firmware-sticks3-merged-2.5.0.bin" --dir dist
python scripts/publish_cos.py firmware --dist dist --version 2.5.0 \
  --min-version "$(cat FIRMWARE_MIN_VERSION)" --skip-upload   # 生成 .sha256 + manifest.json
python scripts/publish_cos.py firmware --dist dist --version 2.5.0 \
  --min-version "$(cat FIRMWARE_MIN_VERSION)"                 # 真传
```

4b. macOS 软件（Mac 件从 Release 下载，staging 目录镜像 COS 键位）：
```bash
mkdir -p staging/software/macos/v2.5.0
gh release download v2.5.0 --repo Haobot/VoiceStickPlus \
  --pattern "VoiceStick-2.5.0.*" --dir staging/software/macos/v2.5.0
python -c "import sys; sys.path.insert(0,'scripts'); from cos_uploader import require_credentials, collect_dir, upload_files; require_credentials(); upload_files('voicestick-dl-1329978361','ap-shanghai', collect_dir('staging',''))"
```

4c. MSI：
```bat
python scripts/publish_cos.py --bucket voicestick-dl-1329978361 --region ap-shanghai software --msi-dir desktop\windows\build-msi-x64 --version 2.5.0
```

### 5. 网页三件套同步（只有你做；前提=步骤 3/4 完成，Release 12 件全齐）

5.1 刷 Pages（appcast/downloads.json/烧录器固件，workflow 从 latest release 动态拉取）：
```bat
gh workflow run deploy-website.yml --repo Haobot/VoiceStickPlus --ref main
gh run list --repo Haobot/VoiceStickPlus --workflow deploy-website.yml --limit 1   （轮询至 completed success）
```
（skill 里「必须 --ref feat/stick-gateway」已过期——deploy-website 近期在 main 上多笔成功。）

5.2 备齐 `website/public/firmware/latest/`：先清空该目录（防旧 bin 打进 dist）→
`gh release download v2.5.0 --repo Haobot/VoiceStickPlus --pattern "voicestick-firmware-sticks3-merged-2.5.0.bin" --pattern "manifest.json" --dir website/public/firmware/latest`
→ 把 manifest.json 的 `merged_url` 改写为
`https://dl.davenger.cloud/firmware/latest/voicestick-firmware-sticks3-merged-2.5.0.bin`（COS 同源）。

5.3 appcast/downloads 从 Pages 取最新版覆盖 `website/public/appcast.xml` 与
`website/public/downloads.json`（`https://haobot.github.io/VoiceStickPlus/...`，curl 可直连）。

5.4 构建（必须 PowerShell 跑 --base=/；MSYS 裸跑会被路径转换 → COS 全资源 404 白屏）：
```bat
powershell -NoProfile -Command "cd website; npm run build -- --base=/"
```

5.5 整站上传 COS bucket 根：
```bat
python -c "import sys; sys.path.insert(0,'scripts'); from cos_uploader import require_credentials, collect_dir, upload_files; require_credentials(); upload_files('voicestick-dl-1329978361','ap-shanghai', collect_dir('website/dist',''))"
```

5.6 显式删 COS `firmware/latest/` 旧版本 bin（upload 只增不删；qcloud_cos client.delete_object，只留 2.5.0）。

5.7 pages-mirror 转传（appcast/downloads.json，幂等）：
```bat
python scripts/publish_cos.py --bucket voicestick-dl-1329978361 --region ap-shanghai pages-mirror --pages-base https://haobot.github.io/VoiceStickPlus
```

## 你的终验清单（逐项过，全绿才算完成）

1. Release 资产恰 12 件：dmg/.signature/zip + 固件 5 件（ota+merged+2×.sha256+manifest.json）+ MSI 4 件
2. `curl -s -I https://dl.davenger.cloud/software/windows/v2.5.0/VoiceStick_2.5.0_zh-CN.msi` → 200
3. `curl -s https://dl.davenger.cloud/firmware/latest/manifest.json` → version=2.5.0；
   `latest/voicestick-firmware-sticks3-merged-2.5.0.bin` 200、旧版本 bin 404
4. `curl -s https://dl.davenger.cloud/ | grep -o -a "[^\"]*assets/index-[A-Za-z0-9_-]*\.\(css\|js\)"` →
   引用无 `/VoiceStickPlus/` 前缀（grep 模式必须带 `[^"]*`，`assets/` 起头会漏检 base 错误）；
   对引用 JS 实下载 grep 2.5.0、旧版本串 0 处
5. `curl -sI https://dl.davenger.cloud/` 的 Content-Type 是 text/html（octet-stream = 上传丢头 → 白屏）
6. COS `appcast.xml`：version="2.5.0" + zh-CN MSI enclosure + mac zip item
7. Pages `https://haobot.github.io/VoiceStickPlus/appcast.xml` 同上

## 红线与异常

- 与本文不符（版本不对、Release 被别人建过、分支不干净）→ 停下问用户，不将错就错。
- Release notes（body）归 Mac 管。若用户要求改 notes 且 Mac 已改：你必须重跑 5.1 + 5.7
  （appcast 的 release-notes 取自 Release body）。
- 全程：不 push git、不删 tag、不碰 gitee、不用 release.ps1、不 `gh release create`。
- 结束后向用户汇报：各步骤结果、CTest 结论、12 件资产清单、终验逐项结论、任何偏离。
````

---

## 提示词 B：Mac 主机（Mac 侧，发布驱动方）

````markdown
# 任务：VoiceStick v2.5.0 发布——Mac 侧（发布驱动方）

你在 Mac（hwp 主机，工作目录 ~/Dev/DVG/VoiceStickPlus）上执行 v2.5.0 发布的 Mac 侧工作，
并且是**发布驱动方**：版本 bump、tag、CI 门禁、创建 GitHub Release、上传 macOS+固件资产都由
你做；Windows MSI、COS 全量直传与网页三件套由 Windows 签名机上的 AI 并行完成。动手前先读
AGENTS.md、Doc/Ref/release.md、.agents/skills/release-publish/SKILL.md。

## 本机能力事实

- gh 已装且登录 Haobot（push/api/release 实测可用；网络超时可加
  `https_proxy=http://127.0.0.1:5782`，端口以 `scutil --proxy` 为准）。
- 无 COS 凭据 → COS 全部由 Windows 侧完成，你只做只读终验。
- 签名资产：Sparkle 私钥 `~/.voicestick-sign/sparkle/ed-private-key`、自签证书在
  voicestick-sign 钥匙串（打包脚本自动解锁）；SwiftPM 拉依赖要代理。
- 固件不在本机构建：release.yml（tag 触发）CI 构建后从其 artifact 取件。
- ci.yml 预存红（main 上自 2026-09-27），不阻塞；唯一 CI 门禁 = release.yml（~3 分钟）。
- remote：origin=Haobot/VoiceStickPlus（SSH 可推）；**gitee 平行 remote 本轮绝不 push**。

## 与 skill/release.md 的有意差异（本轮流次，勿照抄旧文档）

- 发布分支 = **main**（feat/stick-gateway 严格落后 150 笔无独有提交，禁用）。
- **不走 publish-mac.yml 预签名/finalize 轨道**（ci/publish-finalize 分支从未成功过——
  Pages environment 拒 ci/* 部署；.publish-finalize 只能挂在过期分支上）。v2.4.10 先例就是
  Mac 直接 gh 建 Release（12 件资产、无 presign json），本轮沿用。
- deploy-website.yml 在 main 上 dispatch 有效（近期多笔成功），网页同步整体归 Windows。

## 时序（Windows 侧在等你的信号）

- **信号 A**：你推完 bump commit（origin/main）+ tag v2.5.0 → Windows 开始同步/构建/CTest。
- **信号 B**：你建好 Release 且 8 件（3 mac + 5 固件）上传完 → Windows 才传 MSI。
- **信号 C**：Release 12 件全齐 → Windows 做 COS + 网页，你随后终验。

## 步骤

### 0. 前置核对（任一不符 → 停下问用户）
- `git status --porcelain` 干净（若有未跟踪的计划文档如本提示词落盘文件，先提交或移出）；
  当前分支 = main；`git fetch origin` 后 `git rev-list --left-right --count main...origin/main` = `0  0`。
- VERSION 与 firmware/version.txt 均为 2.4.10；FIRMWARE_MIN_VERSION = 0.3.0（不动）。
- `git ls-remote origin refs/tags/v2.5.0` 为空、`gh release view v2.5.0 --repo Haobot/VoiceStickPlus` 404
  （没有已存在的 v2.5.0）。
- 建议：`swift build && swift run VoiceStickTests` 过一遍再动手（main 合并树 153 笔提交）。

### 1. bump 版本（信号 A 之一半）
```sh
printf '2.5.0' > VERSION && printf '2.5.0' > firmware/version.txt
git add VERSION firmware/version.txt
git commit -m "chore(release): 版本升级 2.4.10 → 2.5.0（完整发布轮：固件 + Windows MSI + macOS）"
git push origin main
```
（纯文本无换行；只改这两个文件。）

### 2. 写发行说明 RELEASE_NOTES.md（放仓库根目录，随 Release 提交，不入 main）
- 中文，`# VoiceStick 2.5.0` 开头，分类 `## 新增 / ## 改进 / ## 修复 / ## 说明`，
  只写用户可见变化；风格对拍 `gh release view v2.4.10 --repo Haobot/VoiceStickPlus --json body -q .body`。
- 素材：`git log v2.4.10..HEAD --oneline`（153 笔）+ 近期 merge 信息。主要话题（以实际 log 为准归类）：
  语音编辑批次 1/2（字段内定位/句中插入/尾删移光标/追问「第几个」/整框搜索域/上下文撤销/
  Chromium AX 异步修复）、标点独词输入、本地离线 ASR（SenseVoice）与本地 LLM 精修对齐
  （内嵌 llama.cpp）、本地模型设置与下载、热词分区/智能热词管理+回收池/领域语境、
  BLE 断连诊断上报（固件）、音频卡顿修复（固件 DMA 环与时间基）、S2 息屏动态功耗（固件）、
  Windows 侧电池监测/虚拟麦克风直通改名/会话历史/使用事件、纠错召回闭环、浮窗 liquid glass、
  设置窗口滚动修复。程序性变化放「说明」；固件要点单独提示用户需 OTA 升级。
- **一次性定稿**：stage 之后改 notes 会让 Windows 的网页同步白做（appcast release-notes
  取自 Release body）。

### 3. 打 tag 并等门禁（信号 A 完成）
```sh
git tag -a v2.5.0 -m "VoiceStick 2.5.0" && git push origin v2.5.0
gh run list --repo Haobot/VoiceStickPlus --workflow release.yml --limit 1   # 轮询至 completed success（~3 分钟）
```
- 必须看到 `completed success`（continue-on-error 的 run 不算）。
- 门禁失败：向用户说明后 `git push origin :refs/tags/v2.5.0 && git tag -d v2.5.0`，修复重打重推。

### 4. 构建 macOS 包（内测包）
```sh
https_proxy=http://127.0.0.1:5782 SPARKLE_PRIVATE_ED_KEY="$(cat ~/.voicestick-sign/sparkle/ed-private-key)" VOICESTICK_ARCHS=arm64 scripts/build-macos.sh --release
scripts/make-dmg.sh
```
- 代理端口以 `scutil --proxy` 为准（脚本独立 scratch path 会重新拉 SwiftPM 依赖）。
- **凭据门禁**：日志必须出现 `Injecting built-in credentials into VoiceStickApp`；
  构建后 `git status` 中 `BuiltinSecrets.swift` 必须已恢复占位。
- 校验：`codesign --verify --deep --strict build/VoiceStick-2.5.0.app`；
  `build/VoiceStick-2.5.0.signature` 非空（release 构建缺 Sparkle 签名会直接报错）。
- 产物：`build/VoiceStick-2.5.0.zip`（build-macos.sh）、`.signature`、`.dmg`（make-dmg.sh）。

### 5. 取固件件 + 本地 P0-4 扫描
```sh
rm -rf dist dist-fw && mkdir -p dist dist-fw
cp build/VoiceStick-2.5.0.zip build/VoiceStick-2.5.0.signature build/VoiceStick-2.5.0.dmg dist/
gh run download <release.yml 的 run id> --repo Haobot/VoiceStickPlus \
  --name voicestick-firmware-2.5.0 --dir dist-fw    # 5 件：ota+merged bin、2×.sha256、manifest.json
python3 scripts/scan_release_artifacts.py --allow-builtin dist/* dist-fw/*
```

### 6. 建 Release + 上传 8 件（信号 B）
```sh
gh release create v2.5.0 --repo Haobot/VoiceStickPlus --title "VoiceStick 2.5.0" \
  --notes-file RELEASE_NOTES.md --latest
gh release upload v2.5.0 --repo Haobot/VoiceStickPlus --clobber \
  dist/VoiceStick-2.5.0.zip dist/VoiceStick-2.5.0.signature dist/VoiceStick-2.5.0.dmg \
  dist-fw/voicestick-firmware-sticks3-ota-2.5.0.bin dist-fw/voicestick-firmware-sticks3-ota-2.5.0.bin.sha256 \
  dist-fw/voicestick-firmware-sticks3-merged-2.5.0.bin dist-fw/voicestick-firmware-sticks3-merged-2.5.0.bin.sha256 \
  dist-fw/manifest.json
```
（上传失败重试一次。只传这 8 件，别碰 MSI——那是 Windows 的 4 件。）

### 7. 等信号 C（Release 12 件全齐）
```sh
gh release view v2.5.0 --repo Haobot/VoiceStickPlus --json assets --jq "[.assets[].name] | length"
```
轮询 60~90 秒直到 = 12；>45 分钟未齐 → 汇报等待状态。Windows 传完 MSI 后还要做 COS+网页，
终验里网页项若暂未就绪，等 5~10 分钟重试，仍失败则如实汇报（可能是对方未完成）。

### 8. 最终终验（全渠道只读，全绿才算发布完成）
1. Release 资产 12 件，逐名核对 v2.4.10 模板
2. `curl -s https://dl.davenger.cloud/firmware/latest/manifest.json | grep version` → 2.5.0
3. bin SHA 三方一致：本地 dist-fw / COS 实下载 / Release `.sha256` / manifest `ota_sha256`
4. `curl -s -I https://dl.davenger.cloud/software/windows/v2.5.0/VoiceStick_2.5.0_zh-CN.msi` → 200
5. `curl -s https://dl.davenger.cloud/ | grep -o -a '[^"]*assets/index-[A-Za-z0-9_-]*\.\(css\|js\)'` →
   引用无 `/VoiceStickPlus/` 前缀；对引用 JS 实下载 grep 2.5.0、旧版本串 0 处
6. `curl -sI https://dl.davenger.cloud/` 的 Content-Type 是 text/html（octet-stream = 丢头白屏）
7. COS `appcast.xml` 与 Pages `https://haobot.github.io/VoiceStickPlus/appcast.xml`：
   version="2.5.0" + zh-CN MSI enclosure + mac zip item；downloads.json 2.5.0
8. COS firmware/latest：`latest/voicestick-firmware-sticks3-merged-2.5.0.bin` 200、旧 bin 404

## 红线与异常

- 与本文不符 → 停下问用户。绝不推 gitee、绝不 force push main、绝不碰 MSI 资产与 COS/网页写路径。
- 发布后若用户要求改 notes：你 `gh release edit v2.5.0 --repo Haobot/VoiceStickPlus --notes-file RELEASE_NOTES.md`，
  然后**显式提醒用户**「Windows 侧需重跑 deploy-website dispatch + pages-mirror」。
- 网页侧异常（白屏/404/版本旧）属 Windows 职责——如实汇报等用户转发，不要自己上网页修复路径。
- 结束后向用户汇报：tag/门禁结果、Release 建立、8 件上传、信号 C 到达时间、终验逐项结论、任何偏离。
````
