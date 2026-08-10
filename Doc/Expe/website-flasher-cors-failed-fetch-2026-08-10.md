# 网站浏览器烧录 Failed to fetch（GitHub release-assets 无 CORS 头）2026-08-10

起因：部署到 `https://haobot.github.io/VoiceStickPlus/` 的在线烧录页，Web Serial 已连通 ESP32-S3、stub 已跑起来，但下载固件时抛 `TypeError: Failed to fetch`。本文记录根因、排除过程与修复方案，属 `release-v236-first-github-release-2026-08-10.md` 发布链路的后续坑。

## 一、根因

页面在浏览器里 `fetch` 固件时链路是：

```text
GET https://github.com/.../releases/latest/download/manifest.json
  302 → GET https://release-assets.githubusercontent.com/...   ← 无 Access-Control-Allow-Origin
GET https://github.com/.../releases/download/vX.Y.Z/...merged...bin
  302 → GET https://release-assets.githubusercontent.com/...   ← 无 Access-Control-Allow-Origin
```

GitHub 对 `releases/latest/download/*` 返回 302，最终由 `release-assets.githubusercontent.com` 返回二进制，但该域响应**不带 CORS 头**。浏览器执行 CORS 检查，跨域 GET 被拦截 → fetch 抛 `TypeError: Failed to fetch`。esptool 的 stub 阶段不受影响（Web Serial 不涉跨域），所以报错只在「下载固件」环节出现。

**为什么 curl 能通**：curl 不执行 CORS 预检/检查，只发请求看状态码，所以 `curl -I` 全 200；必须带 `Origin:` 头模拟浏览器，且检查最终重定向目标的 `access-control-allow-origin` 才复现/确诊。

## 二、排除过的方案（都不可行）

| 方案 | 验证结果 |
|---|---|
| GitHub API 资产下载（`api.github.com/repos/.../releases/assets/<id>`） | API 首跳 302 带 `Access-Control-Allow-Origin: *`，但**重定向仍到 release-assets 域、最终响应无 CORS**，浏览器照样拦（CORS 模式重定向链上每个响应都要通过检查） |
| esptool-js 官方代理（`https://esptool-js.netlify.app/...`） | GET 直接拼 URL、URL 编码、POST JSON 均返回 404，该代理端点已不存在 |
| 阿里云 OSS 兜底 | v2.3.6 已彻底迁移到 GitHub Release（`Doc/Ref/release.md`），CI 不再带 OSS 凭据，无兜底通道 |
| `curl` / Node 无头验证 | 只能证明服务端可达，不能证明浏览器可跨域读取 |

## 三、修复方案（已实施）

**把固件同源托管到 GitHub Pages**，浏览器同源 fetch 不触发 CORS：

- `deploy-website.yml` 在构建前新增「Sync firmware to Pages origin」步骤：`gh release download` 把最新 Release 的 `merged-*.bin` 与 `manifest.json` 拉到 `website/public/firmware/`，再用 python 把 manifest 的 `merged_url` 重写为 `https://<owner>.github.io/<repo>/firmware/<filename>`，避免页面拿到 manifest 后仍去 fetch GitHub 域。
- `App.vue` 固件 URL 改为 `BASE_URL + 'firmware/'` 同源路径；删掉失效的 `VITE_FIRMWARE_MANIFEST_URL` 构建期注入（该 env 指向 GitHub 域，本就要被 CORS 拦）。

## 四、教训

1. **Web Serial 烧录器里，「连接/同步 bootloader 正常但下载固件 Failed to fetch」先怀疑固件源的 CORS**，不是 esptool 参数问题。
2. **验证 CORS 必须带 `Origin:` 头，且要跟随到最终重定向目标**，看目标响应的 `access-control-allow-origin`。只看第一跳（哪怕首跳 `ACAO: *`）会误判（GitHub API 通道就是这个坑）。
3. **GitHub Release 资产（`release-assets.githubusercontent.com`）不提供 CORS**，任何浏览器直连 fetch 都会被拦。要跨域取固件必须同源托管或自建代理。
4. 依赖第三方中转（esptool-js 官方代理）端点可能消失，方案前先验证存活。
5. 迁移后未在浏览器真机验证全链路（曾用 curl 验证 URL 200 即当成功），这次暴露了「curl 通过 ≠ 浏览器可用」。烧录链路验证须在浏览器开发工具里看 fetch 的 Network/CORS 报错，或 `curl -H "Origin: ..."` 逐跳核对响应头。

## 五、上线后 CI 暴露的第二个坑：新步骤缺 GH_TOKEN

改动推到 main 后 `deploy-website.yml` 自动触发但 **failure**：新加的「Sync firmware to Pages origin」步骤在 Actions runner 里报 `gh: To use GitHub CLI in a GitHub Actions workflow, set the GH_TOKEN environment variable`，退出码 4。

**原因**：原「Update appcast from latest release」步骤自带 `env: GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}`，但新步骤没带。Actions runner 里 `gh` CLI **不会自动继承 token**，必须每个用到 `gh` 的步骤显式声明。本地 Bash 验证时 `gh` 已登录（凭据缓存在用户目录），所以没暴露；这正是本地验证覆盖不到的 CI 特有机制。

**修复**：给新步骤补 `env: GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}`（与既有步骤一致）。

**教训**：凡是在 workflow 里用 `gh` CLI，**每个步骤都要显式写 `GH_TOKEN`**；本地 Bash 验证通过 ≠ Actions 能跑通。参考同类历史坑（`Doc/Expe/release-v236-first-github-release-2026-08-10.md` 坑 4：`gh` 不在 Bash PATH），Actions 环境与本地环境的差异要逐一核对。
