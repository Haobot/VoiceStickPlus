# dl.davenger.cloud 整站同步三个叠加坑——旧 bundle 内联版本号、base 白屏、latest 目录残留

- 日期：2026-09-27
- 相关文件：`website/vite.config.js`、`website/src/App.vue`（`VERSION?raw` 版本内联）、`website/public/firmware/latest/`（未跟踪部署产物）、`scripts/publish_cos.py`、`scripts/cos_uploader.py`、`.github/workflows/deploy-website.yml`
- 相关提交：`1ae35702`（v2.4.4 发布）、`21421456`（public appcast 快照同步）
- 关联文档：`Doc/Ref/cos-distribution.md`（渠道约定）、`Doc/Expe/website-flasher-cors-failed-fetch-2026-08-10.md`（烧录器同源固件机制的由来）、`Doc/Expe/msi-missing-sherpa-dll-defender-sign-flake-2026-09-27.md`（MSI 内容门禁）

## 症状

v2.4.4 发布（仅固件轮）并补齐 MSI 后，用户报告两轮问题：

1. 「从 dl.davenger.cloud 下载的固件还是 `voicestick-firmware-sticks3-merged-2.4.1.bin`」；
2. 修复过程中用户再报「dl.davenger.cloud 无法访问」，浏览器控制台：

```
GET https://dl.davenger.cloud/VoiceStickPlus/assets/index-Cdgoxric.js net::ERR_ABORTED 404
GET https://dl.davenger.cloud/VoiceStickPlus/assets/index-CwFo6QFZ.css net::ERR_ABORTED 404
```

## 判据（下次快速识别）

- **资源 URL 带 `/VoiceStickPlus/` 前缀** = vite base 用了默认值（Pages 子路径形态）误传 COS。
- **页面正常渲染但下载/烧录指向旧版本** = 站点 JS 是旧构建：版本号经 `import appVersion from '../../VERSION?raw'` 在**构建时内联**进 bundle，仅更新 manifest/downloads.json 等数据文件、不重建 bundle，页面写死的默认 URL 永远指旧版本。
- **验证 index.html 引用必须 grep 全路径** `[^"]*assets/index-...`；用 `assets/index-...` 模式会把 `/VoiceStickPlus/` 前缀截掉，漏检 base 错误（本次二次返工的直接原因）。
- `firmware/latest/` 下出现非当前版本的 bin = 历史整站上传残留（upload 只增不删）。

## 根因（三层叠加）

1. **仅固件轮发布不会自动同步网页**：`deploy-website.yml` 只在 `website/**` 变更或手动触发时运行；固件-only Release 后 Pages/COS 的 appcast、downloads.json、烧录器固件全部停在旧版。且 appcast 依赖 Release 里的 MSI 资产——固件-only Release 直接刷网页会生成残缺 appcast（WinSparkle 指向缺失的包），所以补网页前必须先把 MSI 补传 Release。
2. **COS 整站构建必须显式 `--base=/`**：`website/vite.config.js` 默认 `base: '/VoiceStickPlus/'`（为 GitHub Pages 子路径设计）。裸 `npm run build` 产出的 index.html 引用 `/VoiceStickPlus/assets/...`，COS bucket 根没有该前缀 → 全资源 404 → 白屏。MSYS/Git Bash 下命令行传 `--base=/` 会被路径转换成 Git 安装路径（v2.4.1 发布踩过），**必须在 PowerShell 里跑**。
3. **`website/public/firmware/latest/` 残留**：该目录是 git 未跟踪的本机部署产物，`npm run build` 会原样拷进 dist 上传；而 `publish_cos.py firmware` 子命令只覆盖 `firmware/latest/manifest.json`，既不放 bin 也不清理旧版本 bin → `latest/` 目录残留累积（实测有 v2.4.1 的 bin 存活）。

## 修复（终版发布配方）

仅固件轮发布后同步静态网页的完整操作序列：

1. **程序对齐**（appcast 依赖 MSI 资产）：`scripts/build-msi.bat`（读 VERSION，双语 MSI 自动签名）→ 生成 `.sha256` → `scan_release_artifacts.py` P0-4 门禁 → `gh release upload <tag> --clobber` → `publish_cos.py software --msi-dir ... --version ...`。
2. **Pages 刷新**：`gh workflow run deploy-website.yml --repo Haobot/VoiceStickPlus --ref feat/stick-gateway`（workflow 从 latest release 拉固件 merged bin + 重生成 appcast/downloads.json + 部署 Pages；用 feat 分支的 workflow 版本，main 停在旧点）。
3. **COS 整站**：
   - 更新 `website/public/`：`firmware/latest/` 放当前版本 merged bin + manifest（manifest 的 `merged_url` 重写为 `https://dl.davenger.cloud/firmware/latest/<file>` 同源地址）；`appcast.xml`/`downloads.json` 从 Pages 拉最新版覆盖；
   - **PowerShell** 执行 `npm run build -- --base=/`；
   - `cos_uploader.collect_dir('website/dist', '')` + `upload_files(...)` 整站传 bucket 根；
   - **显式删除** COS `firmware/latest/` 下非当前版本的 bin（`client.delete_object`，upload 不会清理多余对象）；
   - `publish_cos.py pages-mirror` 转传 appcast/downloads.json（如 Pages 刷新晚于本步则再跑一次）。
4. **终验六点**：COS index.html 引用全路径无 `/VoiceStickPlus/`；新 JS 内联当前版本字面量且旧版本串 0 处；`firmware/latest/manifest.json` version 正确且 merged_url 同源；`latest/<当前版本>.bin` 200；`latest/<旧版本>.bin` 404；Pages 同路径 200。

## 验证

2026-09-27 v2.4.4 按上述配方执行后全绿（线上实测：index.html 引用 `/assets/index-Cdgoxric.js` + `/assets/index-CwFo6QFZ.css` 均 200、JS 内 `"2.4.4"` 内联且 `"2.4.1"` 0 处、`latest/merged-2.4.4.bin` 200、`latest/merged-2.4.1.bin` 404、Pages 同路径 200），用户确认恢复。

## 长期技术记忆/经验

- **版本号构建时内联**（`VERSION?raw` → bundle 字面量）：一切「只更新数据文件」的同步对页面内写死的默认 URL 无效；JS/CSS 的 content hash 随内容变化，base 参数变化会改 JS hash（CCZWhD6d→Cdgoxric）但 CSS hash 不变。
- **过渡窗口**：index.html 是 no-cache 立即生效，assets 是长缓存但首次上传需要时间——整站上传期间访问会撞上「新 index.html 引用的 assets 尚未就位」的瞬时 404（本次用户实测撞上）。窗口极小，无需预热机制，用户强刷即可。
- 固件 bin 文件名含版本号 + `max-age=31536000` 长缓存：版本化路径的缓存天然正确，无需主动刷新 CDN。
- `gh` 走 api.github.com 链路在本机稳定可用；curl 直连 github.com 国内超时——GitHub 侧验证用 `gh api` / `gh release download`。

## 遗留/观察项

- `publish_cos.py` 没有 `website` 子命令，整站上传目前是手工配方（本文档步骤 3）；上传频次低、配方已固化，暂不固化代码，下次再发整站时评估。
- COS 无对象生命周期清理：版本化路径（`firmware/v*`、`software/windows/v*`）的历史对象按设计保留；仅 `firmware/latest/` 需要每次手工清理旧 bin。
