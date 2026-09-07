# 下载页 + 一键发布 + 更新推送增强 设计

状态：设计已确认（2026-09-07），待开分支实施
决策：方案 A——数据流垂直切片（发布链路与数据格式 → 网站下载页 → Windows 桌面端增强）；
Windows MSI 维持签名机手动构建（不进 CI）；下载页数据源用同域静态 JSON；
分支 `feat/downloads-and-release`，基于 `main`。

## 背景与现状（2026-09-07 调查）

已有自动化基础（复用，不重建）：

- `release.yml`：`v*` tag 触发，固件自动构建上传 GitHub Release（ota/merged bin + .sha256 + `manifest.json`），并触发网站重部署；已支持 `workflow_dispatch`。
- `deploy-website.yml`：自动重写 `website/public/appcast.xml`（`scripts/update-appcast.py`），同步固件到 Pages `/firmware/`，已支持手动触发。
- 桌面更新推送：macOS Sparkle / Windows WinSparkle 均指向 `https://haobot.github.io/VoiceStickPlus/appcast.xml`。
- 固件 OTA：桌面端拉 `releases/latest/download/manifest.json` 比较版本，BLE OTA 下发。

缺口（本设计要补齐的）：

- 网站无下载页：落地页 hero 只有 2 个按钮，URL 用**构建期** VERSION 拼接，部署与 Release 不同步即 404；i18n `nav.download` key 预留未用。
- 发布流程散落多步手动操作（VERSION 对齐、tag、等 CI、上传 MSI、触发部署、验证 URL），易遗漏。
- WinSparkle 更新只有托盘菜单手动入口，无主动通知；更新检查发现更新即弹标准对话框。
- 固件升级检查只在设备连接时触发一次；强制升级阈值 `AppConfig::minimum_compatible_firmware_version` 为本地硬编码，无法随发布调整。
- 遗留 bug：`deploy-website.yml:88` 硬编码 `https://78.github.io/voicestick/appcast.xml`，与实际 `haobot.github.io/VoiceStickPlus` 不一致。

## 范围

| 项 | 在/不在 |
|---|---|
| 网站独立下载页（hash 路由） | ✅ 阶段二 |
| downloads.json 生成（同域静态） | ✅ 阶段一 |
| manifest.json 加 `min_version` | ✅ 阶段一（协议变更，同步文档与双端解析容错） |
| 本地一键发布脚本 `scripts/release.ps1` | ✅ 阶段一 |
| CI 双入口（现有 workflow_dispatch 完善） | ✅ 阶段一 |
| Windows 端更新气泡通知 / 固件主动提醒 / min_version 消费 | ✅ 阶段三 |
| Windows MSI 进 CI 自动签名构建 | ❌（签名证书是本地硬件/机器状态，维持手动） |
| macOS 端 Sparkle 体验增强、macOS 消费 min_version | ❌（后续工作；manifest 新字段对 macOS JSON 解析向后兼容） |
| 固件 OTA 传输机制本身 | ❌（现有 BLE OTA 不动） |

## 阶段一：发布链路与数据格式

### 1.1 manifest.json 协议扩展

新增字段 `min_version`（最低兼容固件版本）：

- 语义：设备当前版本 `< min_version` → 强制升级提示；`min_version ≤ 当前 < version` → 可选升级。
- 来源：仓库根新文件 `FIRMWARE_MIN_VERSION`（纯文本单行，与 `VERSION` 同模式：人工维护、机械消费），`release.yml` 读取写入 manifest。
- 兼容：字段缺失时消费端回退本地配置（见 3.3）。
- 同步义务：`Doc/Ref/protocol.md` manifest 字段表、`release.yml` 生成逻辑、Windows `firmware_manifest.cc` 解析；macOS `FirmwareManifest.swift` 忽略未知字段不受影响（消费端增强留后续）。

### 1.2 downloads.json 生成

新脚本 `scripts/update-downloads.py`（Python，与 `update-appcast.py` 同风格），在 `deploy-website.yml` 的 appcast 更新步骤后调用，从 GitHub Release（gh CLI / API）拉数据，生成 `website/public/downloads.json`：

```json
{
  "latest": {
    "version": "2.3.8",
    "date": "2026-09-07",
    "notes": "markdown 更新日志（Release body）",
    "min_firmware_version": "2.3.0",
    "assets": [
      {"name": "VoiceStick_2.3.8_zh-CN.msi", "platform": "windows",
       "url": "https://github.com/.../download/v2.3.8/...", "size": 0, "sha256": null}
    ]
  },
  "releases": [
    {"version": "2.3.7", "date": "...", "notes": "...", "assets": ["...同上结构..."]}
  ]
}
```

- `platform` 枚举：`windows` / `macos` / `firmware`（按资产名规则分类）。
- `sha256`：固件资产读 Release 上已有 `.sha256` 文件；MSI 默认 null，一键脚本上传 MSI 时顺带生成上传 `.sha256`（可选增强，有了就填）。
- `releases` 收最近 10 个版本（`gh release list --limit 10`），资产 URL 按 `releases/download/v<version>/<name>` 规律构造。
- 脚本可独立本地运行（输入 Release JSON → 输出 downloads.json），便于单测。

### 1.3 本地一键发布脚本 `scripts/release.ps1`

在 Windows 签名机上运行，串起全流程：

```
参数校验（--version 必填、--skip-msi 可选）
→ 校验 VERSION / firmware/version.txt / FIRMWARE_MIN_VERSION 一致与格式
→ [未 --skip-msi] scripts\build-msi.bat 构建+签名 MSI（双语言）
→ git commit 版本变更 → 打 tag v<version> → push
→ gh run watch 等 release.yml 完成（超时明确报 run URL）
→ [未 --skip-msi] gh release upload 双 MSI（+ .sha256），失败重试一次
→ gh workflow run deploy-website.yml
→ HEAD 请求验证 appcast / manifest.json / downloads.json / 全部资产 URL 返回 200
→ 输出发布报告（各项 ✅/❌ 清单）
```

`-DryRun` 开关：打印将执行的步骤，不实际执行。

### 1.4 CI 双入口与修复

- `release.yml` / `deploy-website.yml` 的 `workflow_dispatch` 入口保留并核对（固件+网站方向）。
- 修复 `deploy-website.yml:88` 硬编码 URL 为 `https://haobot.github.io/VoiceStickPlus/appcast.xml`。

## 阶段二：网站下载页

- **路由**：不引入 vue-router。`App.vue` 按 `location.hash` 切换两个视图：落地页 / `#/download` 下载页；GitHub Pages 静态托管无需 404 回退配置。顶部导航启用 `nav.download` key。
- **下载页内容**（风格跟随现有落地页）：
  - 最新版本卡：按 Release 实际资产生成，按平台分组——Windows（MSI 双语言按当前语言高亮默认项）、macOS（zip/dmg 如有）、固件（ota / merged bin + sha256）；每项显示文件大小与校验和。**某平台组无资产时整组隐藏**（现状：macOS CI job 已禁用、便携版 zip 为手动产物不随 CI 上传，这两类资产存在才展示，发布侧不新增上传义务）。
  - 版本历史：最近版本列表，可展开资产链接与更新日志。
  - 降级：`downloads.json` fetch 失败 → 显示"无法加载版本信息"+ GitHub Releases 页直链。
- **hero 按钮改造**：运行时从 `downloads.json` 取最新版真实 URL（消除构建期拼接 404 风险）；加载中/失败回退现有构建期拼接 URL。
- **i18n**：`zh-CN.json` + `en-US.json` 同步新增全部 key（红线要求，两份 key 集合对称）。

## 阶段三：Windows 桌面端更新体验

### 3.1 程序更新气泡通知

WinSparkle 0.9.2 公开 API 已验证支持（本地构建缓存 `winsparkle.h`）：
`win_sparkle_set_did_find_update_callback` / `win_sparkle_check_update_without_ui` / `win_sparkle_check_update_with_ui`。

- 关闭 WinSparkle 自带定时检查（`win_sparkle_set_automatic_check_for_updates(0)`），`win32_app` 自己按 24h 间隔调 `win_sparkle_check_update_without_ui()` 静默检查（记录 last-check 时间，启动后延迟首查）。
- `did_find_update_callback` → 投递 UI 线程 → 托盘气泡（`Shell_NotifyIconW` + `NIF_INFO`）"发现新版本"。
- 气泡点击（`NIN_BALLOON_USERCLICK`）→ `win_sparkle_check_update_with_ui()` 弹标准对话框（自带更新日志与下载进度，无需自绘 UI）。
- 托盘"检查更新"菜单保留（现行为不变）；检查错误静默不打扰。

### 3.2 固件主动提醒

- 协调器新增定期 manifest 检查（每 12h + 设备连接时，现有连接时检查保留）。
- 对**已连接且版本落后**的设备发一次托盘气泡（点击进入现有固件升级对话框）；同一设备同版本会话内不重复提醒。
- `below_minimum` 强制升级提示语义不变（连接时仍弹现有 prompt）。

### 3.3 min_version 消费

- `firmware_manifest.cc` 解析 `min_version`（TDD：先在 `core_tests.cc` 写失败测试）。
- 判定：`current < min_version` → 强制（现有 `ShowFirmwareUpdatePrompt(is_below_minimum=true)` 路径）；`min_version ≤ current < version` → 可选。
- manifest 缺字段（旧 Release）→ 回退本地 `AppConfig::minimum_compatible_firmware_version`；本地配置降级为回退项，不再是唯一来源。

## 错误处理

| 场景 | 行为 |
|---|---|
| downloads.json fetch 失败 | 下载页显示降级 UI + GitHub Releases 直链；hero 按钮回退构建期 URL |
| downloads.json 资产缺 sha256/size | 前端隐藏该元信息，不报错 |
| manifest 无 min_version | 回退本地 AppConfig 配置 |
| gh run watch 超时 | 脚本报错并输出 run URL，人工介入 |
| gh release upload 失败 | 重试一次，再失败报错退出 |
| WinSparkle 检查失败 | 静默（仅手动检查时由 WinSparkle 自身 UI 提示） |

## 测试与验收

- **阶段一**：`update-downloads.py` 独立单测（固定 Release JSON 输入 → 断言输出）；`release.ps1 -DryRun` 冒烟；workflow YAML 语法核对；`actionlint`（如有）。
- **阶段二**：`npm run build`；i18n key 对称检查；两语言手动验证；hash 路由直开 `#/download` 可达。
- **阶段三**：`core_tests.cc` 新增 manifest `min_version` 解析/缺字段回退/强制与可选判定用例；`build_win.bat` + `ctest` 全绿；构建通过后按惯例重启 VoiceStick.exe 真机验证气泡与提醒。
- **端到端**：下次真实发布走 `release.ps1` 全流程——发布报告全 ✅、下载页 URL 全 200、气泡通知与固件提醒真机可见。

## 实施顺序与提交切分

1. 阶段一（脚本 + CI + 协议字段）：独立提交，每步可验证。
2. 阶段二（网站）：`npm run build` 验证后提交。
3. 阶段三（Windows 端）：TDD，测试先行，`ctest` 全绿后提交。

每阶段完成即提交（中文约定式提交），全链路真机验收放在真实发布时做。
