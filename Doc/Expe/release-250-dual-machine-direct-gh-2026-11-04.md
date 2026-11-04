# v2.5.0 双机直 gh 发布轨道实战定案（推翻预签名轨道）与坑

- 日期：2026-11-04（发布当日收官）
- 相关文件：`Doc/Plan/release-v2.5.0-dual-machine-prompts.md`（双机提示词，85ae03e 入库）、`Doc/Ref/release.md`（过时章节见「遗留」）、`scripts/build-macos.sh`、`scripts/build-msi.bat`（Windows 侧修复 d670f36）
- 相关 commit：Mac 侧 9f54613（bump）→ tag v2.5.0 → Release+8 件；Windows 侧 eb30479/d670f36/b75674e/eb6a2f4（4 笔）；CHANGELOG 沉淀 cacc2cc/6f420be
- 旧文互链：`Doc/Expe/release-v236-first-github-release-2026-08-10.md`（v2.3.6 时代旧认知，预签名轨道背景）

## 轨道定案（本版实战全绿，为现行标准）

- **发布分支 = main**（feat/stick-gateway 落后 150+ 笔禁用）。
- **Mac 直接 `gh release create v<ver> --latest --notes-file RELEASE_NOTES.md`** + `gh release upload`（8 件：3 mac + 5 固件）——**不走 publish-mac.yml stage/finalize 预签名轨道**。推翻旧轨定案：ci/publish-finalize 分支从未成功过（Pages environment 拒 ci/* 部署）；v2.4.10（2026-10-14）已是直 gh 先例。
- **三信号握手**（两机不需直接通信，全走 GitHub 可观测状态）：A = bump commit + tag 推送（release.yml 门禁 `completed success` 才算数）；B = Release 建好 + 8 件上传（Windows 才传 MSI）；C = 12 件全齐（Windows 开始 COS+网页，Mac 只读终验）。
- **分工红线**：COS 全量直传与网页三件套归 Windows 单边（Mac 无 COS 凭据）；两机均绝不 push gitee、绝不碰对方资产（MSI 4 件归 Windows）。
- 唯一 CI 门禁 = release.yml（~3 分钟）；ci.yml 预存红不阻塞。

## 流程要点（Mac 侧）

- 发布前置：`swift build` + `swift run VoiceStickTests` 全过再动（本轮 2116/2116）；`git fetch` 后 `rev-list --left-right --count main...origin/main` = `0 0`；确认远端无同版本 tag/Release。
- 内测包门禁三查：日志必须出现 `Injecting built-in credentials into VoiceStickApp`（0 处 WITHOUT）；构建后 `BuiltinSecrets.swift` 已恢复占位；`codesign --verify --deep --strict` + `.signature` 非空。
- SHA 四方对账：本地 dist-fw bin ↔ 本地 `.sha256` ↔ Release `.sha256` ↔ `manifest.json` 的 `ota_sha256`/`merged_sha256`（本轮四方一致）；终验补第四方 COS 实下载。
- 发行说明一次性定稿（放仓库根目录**不入 main**）；发布后改 notes = Windows 侧需重跑 deploy-website + pages-mirror。

## 坑与教训

1. **Windows 长时间无信号 ≠ 失败**：信号 B 后 Mac 侧轮询 6h20m（远超 45min 汇报阈值 8 倍）资产恒 8 件，按约收尾晨报；Windows 侧实际于晨间完成全部工作并推回 4 笔修复。教训：等待侧只做「如实汇报等待状态 + 继续」，**绝不中途代做对方职责**（MSI/COS/网页写路径各有专属凭据与门禁）；停滞根因事后看是 Windows 机 build-msi.bat 的 vswhere -latest 随新装 VS 主版本漂移致工具链混用（d670f36 钉回 MSVC 2022）。
2. **build-macos.sh 原地盖源 Info.plist 版本戳且无恢复逻辑**（PlistBuddy Set `CFBundleShortVersionString`/`CFBundleVersion`，源=仓库文件）：每次 release 构建（无论 bump 与否）都会把工作区 Info.plist 改成本次 VERSION。功能零影响（下次构建从 VERSION 重盖），但按「bump 只改两文件」约定应 `git checkout` 恢复保持树干净；若想让 plist 随发布提交（历史有先例），须在 bump 步骤显式加入。
3. **SwiftPM 缓存会掩盖「构建是否真的验证了合并树」**：合并 153 笔后 `swift build` 仅 0.63s（缓存命中）也算全过，但真验证靠测试套件（2116/2116 覆盖批次 1/2）——发布前置跑测试不能省。
4. **拉取对端推回的提交后再终验**：Windows 侧完成时会推修复/appcast 快照回 main（本轮 4 笔），Mac 终验前先 `git pull --ff-only` 同步。

## 遗留 / 观察项

- **`Doc/Ref/release.md` 正文待回写**：「macOS-only Release Round (unattended CI rail)」章节仍描述预签名轨道，与本文定案冲突；skill `release-publish` 的「feat/stick-gateway 手工分步」「--ref feat/stick-gateway 刷 Pages」表述同需更新。以本文 + `Doc/Plan/release-v2.5.0-dual-machine-prompts.md` 为现行标准。
- RELEASE_NOTES.md / dist/ / dist-fw/ 为发布现场产物（未跟踪），留存供 SHA 溯源，可随时清理。
