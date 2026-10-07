# VoiceStick 架构解读、现状体检与二期优化建议（既往评审跟进）

> 评审对象：`feat/stick-gateway` @ `d56c0fc`（VERSION 2.4.9，最新提交 2026-10-03；评审时工作树干净）。
> 评审日期：2026-10-07。
> 上游：`Doc/Plan/architecture-review-and-optimization-2026-09-22.md`（11 天前的全面架构评审）。本篇**不重复其缺陷清单**，做三件事：① 项目功能与架构全景解读；② 既往评审遗留项逐条抽查核查；③ 基于核查结果给出二期优化建议。
> 方法：源码与文档实读 + 规模统计（find/wc，排除第三方与生成物）+ git 统计 + CI/测试/版本一致性核对。未运行任何构建与真机验证（局限见附录 B）。
> 证据约定：`文件:行号` 为证；**【已复核】**＝本次实读/实跑确认；**【引自既往评审】**＝引用 9-22 评审结论、本次未重新实证；行号对应上述 commit。
> 本文档只做分析与方案，未修改任何产品代码。

---

## 0. 结论摘要

**整体判断**：项目自 9-22 评审后修复了 4 个 P0 中的 3 个、CI 从零建成 4-job 门禁、P1 三批推进——工程响应力很强。当前四端架构边界（桌面端唯一可信源、固件只报事实、小米固件零改动）在代码中真实落地，文档与复盘文化仍是最大资产。

但本次核查确认三件事：

1. **既往评审的高危遗留项仍开放且无任何跟踪载体**（无 issue、无 backlog 文档）：固件 BLE 鉴权（原 P0-2）、多连接表（A4）、配置竞争与非原子写（B8/B9）、烧录工具 UAF（B1）——抽查全部未修复。
2. **既往评审标记的"最高性价比②"（跨端 golden-frame 契约测试）至今未落地**，且本次抓到三处可作为立项依据的实锤漂移（Ogg granule 两端一致地错、protocol.md 帧时长写错、macOS 架构文档与源码不符）。
3. **macOS 分层出现新的结构性缺口**：测试 target 只链接 `VoiceStickCore`（2.0k 行），16.8k 行 App 代码（含 2,522 行协调器状态机）零覆盖——最新提交 `d56c0fc` 的提交信息就是活证据（"测试只链 Core 未拦截"）。

### Top 结论

| # | 级别 | 结论 | 证据 |
|---|---|---|---|
| 1 | P1 | 既往遗留高危项开放且无跟踪载体：P0-2 BLE 鉴权、A4 多连接表、B8/B9 配置、B1 UAF | 见 §3.2；`voice_ble.c:774`、`voice_stick_coordinator.h:644`【已复核】 |
| 2 | P1 | 跨端契约零 fixtures，"两端一致地错"已实锤 3 处（granule/60ms/架构文档） | `ogg_opus_muxer.cc:30`、`OggOpusMuxer.swift:32`、`protocol.md:52`【已复核】 |
| 3 | P1 | macOS App target 16.8k 行零测试；分层与文档要求（"核心行为进 Core"）相悖 | `Package.swift:56`；`d56c0fc` 提交信息【已复核】 |
| 4 | P1 | CI 缺 website 构建与 release-guard（版本/协议/Hub 一致性靠人工） | `ci.yml` 仅 4 job【已复核】 |
| 5 | P2 | 三份同源 Hub 已漂移；`desktop-architecture.md:11` 与源码不符 | `AGENTS.md:92-93` vs `CLAUDE.md:95`【已复核】 |
| 6 | P2 | 仓库卫生：根目录 9 个 .bat、m0/p1 跟踪与 ignore 并存、skills 双份同 MD5、`docs/superpowers` 遗留 | 见 §4 N6【已复核】 |
| 7 | 保持 | CI 4-job 门禁、反假绿红线、复盘文档化、Windows core/App 分层样板——优化不得破坏 | 见 §2.5 |

---

## 1. 项目功能与架构全景

### 1.1 产品功能（四端矩阵）

| 能力 | 固件 StickS3 | macOS | Windows | 网站 |
|---|---|---|---|---|
| 按键/双击/敲击/编码器/IMU 采集 | ✅ 只上报事实 | 消费 | 消费 | — |
| 音频采集 + HPF/AGC + Opus 编码 | ✅ 16kHz/mono/40ms | — | 小米链路二次编码（ATVV ADPCM→PCM→Opus） | — |
| BLE GATT（audio/state/control/OTA 五特征） | ✅ 服务端 | ✅ 中央 | ✅ 中央 | — |
| 交互状态机（状态唯一可信源） | ❌ 红线 | ✅ Coordinator | ✅ Coordinator | — |
| ASR | — | 火山/腾讯 | 火山/腾讯/本地 SenseVoice | — |
| LLM 翻译/精修（含流式 SSE） | — | ✅ | ✅（+本地 llama.cpp） | — |
| 文本注入/字幕/悬浮窗/托盘 | 只渲染 `ui_state` | ✅ | ✅ | — |
| 微信输入法模式（虚拟麦克风） | — | ❌（需 CoreAudio 驱动） | ✅ WASAPI | — |
| 体感鼠标 / 敲击映射 / 热词飞轮 | 采样上报 | ✅（v2.4.7 起） | ✅ | — |
| 固件升级 | BLE OTA 接收端 | BLE OTA + Sparkle 自更 | BLE OTA + COM 烧录工具 + WinSparkle | ✅ 浏览器 esptool-js |
| 商业化（离线授权/本地模型/内置凭据内测） | — | 内置凭据内测 | ✅ 完整 | 下载页 |
| 落地页/下载页/更新源（COS 主源 + GitHub 回退 + appcast） | — | — | — | ✅ |

输入设备两类并存：自研 StickS3（`VS-XXXX`，配固件）与小米蓝牙遥控器 2 Pro（`RC-XXXX`，**固件零改动**，ATVV 接入全部在桌面端；网关模式下 StickS3 可作小米的 BLE 中继）。

### 1.2 核心数据流

```text
StickS3 mic → ES8311/I2S PCM → HPF+AGC(软件) → Opus(40ms/32kbps) → BLE notify
                                          ↓
桌面端：AudioFrame → Ogg Opus 封装 → ASR WebSocket → LLM 精修/翻译 → 粘贴/字幕
                                          └→ Opus 解码 → PCM → 虚拟麦克风（微信输入法模式，不经 ASR）
小米 RC：HOGP 按键 → OS 原生消费（可选拦截重映射）
         ATVV ADPCM → 桌面端解码 → PCM 后处理 → Opus 重编码 → 汇入同一条 Ogg/ASR 管线
```

### 1.3 分层与职责边界（红线在代码中的落地方式）

- **固件是"外设"**：`main.c` 编排 + 8 个组件（audio_pipeline / voice_ble / ui_status / bmi270 / mini_encoder_c / stick_s3_board / power_log / gateway），交互语义零持有；唯一例外是双击时序检测。
- **桌面端是大脑，且两端分层不对称**：
  - Windows：`voicestick_core`（纯逻辑，可注入 `FakeBleCentral` 离线测试）+ Win32 壳——**分层样板**；
  - macOS：`VoiceStickCore`（15 文件 2.0k 行纯逻辑）+ `VoiceStickApp`（37 文件 16.8k 行，**含协调器状态机/配置/Ogg mux，不可测**）——见 §4 N1。
- **同源移植策略**：10 对模块两端逐行移植（`air_mouse_kin`↔`AirMouseKin`、`xiaomi_atvv_session`↔`XiaomiAtvvSession`、`hotword_selector`↔`HotwordSelector`、`ble_protocol`↔`BleProtocol` 等），行为一致性靠人肉纪律，无机器对拍。
- **协议四处手写**：`Doc/Ref/protocol.md`（748 行散文）+ 固件 C 字面量 + Swift + C++，零共享测试向量。
- **原型与产品并存**：`m0/`（本地 ASR/热词技术验证）、`p1/`（Python 语音输入 MVP）是路线图早期原型，现已 gitignore 但历史文件仍在索引（见 N6）。

### 1.4 资产规模盘点（本次实测）

| 维度 | 数值 | 备注 |
|---|---:|---|
| 固件源码 | 50 文件 / 16,129 行 C | 最大 `main.c` 3,370 行；`voice_ble.c` 1,750 |
| macOS Swift | Sources 52 文件 / 18,873 行（Core 2,047 + App 16,826）；Tests 15 文件 / 2,038 行 | 另 vendored COpus/CZlib 不计入 |
| Windows C++ | src 约 54,982 行；`core_tests.cc` 15,861 行 | 含生成表 `pinyin_data.cc` 3,769 行 |
| 网站 | `src` 5 文件 / 620 行 Vue + i18n JSON（zh/en 各 100 键） | `package.json` 版本 1.9.0 |
| 脚本 | Python 60 文件 / 14,331 行（另含 sh/ps1/bat） | E2E 真机工具 30+ |
| 文档 | 155 篇 Markdown / 23,235 行 | Plan 89/15,040、Expe 41/3,847、Ref 9/2,088、Guide 8/1,657、Agent 6/263、Rfc 2/340 |
| 原型 | m0 29 文件 3,202 行 / p1 49 文件 5,220 行 | 已 ignore，历史 48+61 文件仍被跟踪 |
| git 跟踪 | 1,891 文件（含根目录 24 个散落文件），源码+文档约 165k 行 | desktop 1,225（含 vendored） |
| 最大单文件 Top5 | `core_tests.cc` 15,861 / `ble_central_win.cc` 4,016 / `pinyin_data.cc` 3,769 / `voice_stick_coordinator.cc` 3,756 / `win32_app.cc` 3,378 | 固件 `main.c` 3,370 第 6 |

---

## 2. 工程现状体检

### 2.1 CI（`.github/workflows/ci.yml`，9-22 评审时为零门禁）

| job | 内容 | 缺口 |
|---|---|---|
| `macos` | `swift build` + `swift run VoiceStickTests`（macos-14） | 只证明可编译与 Core 测试；App 逻辑无测试可跑 |
| `firmware` | `idf.py build`（esp-idf-ci-action，v5.5.1，esp32s3） | 不跑 gateway host 测试（run_tests.py 仅 MSVC） |
| `windows` | Ninja **Debug**（显式关本地 ASR/LLM）+ ctest | Debug 保 assert 有效（NDEBUG 陷阱已规避✅） |
| `script-tests` | 5 个 Python 单测（scan/appcast/mirror/downloads/cos） | — |

**缺失**：website `npm run build`；跨端契约 fixtures；release-guard（版本/协议/Hub 一致性）；固件 host 测试。

### 2.2 测试金字塔现状

```text
        E2E 真机（scripts/e2e_test/，30+ 工具，L1/L3/L4/网关/ATVV/功耗）   ← 工具链丰富，未纳入 CI（合理：需真机）
      ─────────────────────────────────────────────
      跨端契约（golden bytes 对拍）                                     ← ❌ 不存在（本期首要建议）
    ───────────────────────────────────────────────
    集成（Windows L1 真实 ASR，无 key SKIP=77 不伪造）                  ← ✅ 有且纪律好
  ─────────────────────────────────────────────────────────
  单元：Windows core_tests（15.9k 行，FakeBleCentral）                  ← ✅ 厚
       macOS VoiceStickTests（2.0k 行，仅 Core 15 文件）                ← ⚠️ App 16.8k 行零覆盖
       固件 gateway host（4 目标，MSVC 硬编码路径，不进 CI）             ← ⚠️ E8 未闭环
       网站（0）                                                        ← ❌
```

### 2.3 版本与文档一致性

| 检查 | 状态 | 证据 |
|---|---|---|
| VERSION == firmware/version.txt == tag == CHANGELOG | ✅ 均 2.4.9 | 根 `VERSION`；`git describe`=v2.4.9 |
| 网站 package.json 版本 | ⚠️ 1.9.0，与产品版本无关且未标注 | `website/package.json:4` |
| 三份 Hub（AGENTS/CLAUDE/CODEBUDDY） | ❌ 已漂移：CLAUDE/CODEBUDDY 把 AGENTS 两条 bullet 并成一行且多一空行 | `AGENTS.md:92-93` vs `CLAUDE.md:92,95` |
| `desktop-architecture.md` 对 macOS Core 的描述 | ❌ 称 Core 含 "Ogg Opus mux、ASR 帧格式、配置解析"，实际三者都在 App target（`OggOpusMuxer.swift`、`AppConfig.swift`；Core 仅 15 文件） | `Doc/Agent/desktop-architecture.md:11` |
| `protocol.md` 帧时长 | ❌ `protocol.md:52` 写 60ms，固件实际 40ms | `audio_pipeline.c:32 AUDIO_FRAME_MS 40` |
| 网站 i18n zh/en 键奇偶 | ✅ 100/100 完全对齐（无守护脚本） | 本次脚本比对 |

### 2.4 提交结构

- 总 1,339 提交（2026-05-06 起）；近 3 月 731：`feat 248 / fix 178 / docs 172 / chore 55 / test 20 / refactor 12 / perf 4`。
- **refactor 仅 1.6%**，docs 占 23.5%——功能与事故修复持续挤压结构治理，与 §4 N7/N8 的上帝文件增长趋势一致。

### 2.5 做对了什么（优化时不得破坏）

1. **职责边界不是口号**：固件只报事实、桌面端唯一可信源、小米固件零改动，四端代码一致。
2. **反"假绿"纪律**：集成测试无 key SKIP=77、E2E 不 mock 真实链路、CI Windows 用 Debug 保 assert 有效并写明原因。
3. **复盘资产**：Doc/Expe 41 篇 + `claude-memory-distilled` 蒸馏 + 6 个 Skill，BLE CCCD 缓存/僵尸链路/OTA 流控死锁等真机教训全部红线化。
4. **事故驱动的防御**：CCCD 击穿、连接活性证明、僵尸梯度自愈、"绝不 unpair"等已在两端落地。
5. **Windows core/App 分层 + Fake 注入**是可测性样板（macOS 应向它看齐，而非反之）。

---

## 3. 既往评审遗留项核查

### 3.1 P0 核查（9-22 评审的 4 个 P0）

| 项 | 既往状态 | 本次核查 | 证据 |
|---|---|---|---|
| P0-1 macOS 可构建 | ✅ 已修复 + 加 CI | CI `macos` job 在；本次未本地重跑构建 | `.github/workflows/ci.yml`【已复核（静态）】 |
| P0-2 固件 BLE 鉴权 / Secure Boot | 🟡 部分推进 | **仍开放**：`control_rx`=`WRITE_NO_RSP`、`sm_mitm=0`、sdkconfig 仅 SOC 能力位无启用项 | `voice_ble.c:774,1191`；`firmware/sdkconfig:491-492`【已复核】 |
| P0-3 ASR 回调线程封送 | ✅ 已实现 | 已落地：`SetUiDispatcher`/`RunOnUiThread` | `voice_stick_coordinator.h:293,642,651`【已复核】 |
| P0-4 发布凭据门禁 | ✅ 已实现 | 扫描器与单测在库且进 CI | `scripts/scan_release_artifacts.py` + `ci.yml` script-tests【已复核】 |

### 3.2 关键 P1/P2 抽查（凡未特别标注均为本次实读）

| 项 | 状态 | 证据 |
|---|---|---|
| A4 固件多连接表（单值 `current_peer`） | ❌ 未做 | `voice_ble.c:52` 仍单值 `s_conn_handle`；AGENTS.md 仍以"显示层落表兜底"绕行【已复核】 |
| B1 烧录工具关窗 UAF | ❌ 未做 | 析构 `Cancel()` 后 `WaitForSingleObject(...,5000)` 有界等待即释放（`flash_tool_dialog.cc:209-215`）【已复核（形态）】 |
| B8 协调器 `config_` 跨线程竞争 | ❌ 未做 | `voice_stick_coordinator.h:644` 仍为普通 `AppConfig config_`，无原子换入【已复核】 |
| E2 appcast 跨 item 贪婪匹配 | ✅ 已修 | `update-appcast.py:24` 按 `<item>` 块切分【已复核】 |
| E8 固件 host 测试 CI 化 | ❌ 未做 | `run_tests.py:17` 仍硬编码 MSVC `vcvars64.bat` 绝对路径【已复核】 |
| P2 Ogg granule 1.5× 偏差 | ❌ 未修 | 两端仍 `960 * 48000 / sample_rate`（win:30 / mac:32 → 16k 下 +2880，应 +1920）【已复核】 |
| P3 protocol.md 60ms | ❌ 未修 | `protocol.md:52` vs `audio_pipeline.c:32`【已复核】 |
| B6 腾讯热词表 HTTP body 双错 | ✅ 既往称已修 | 【引自既往评审】本次未复核 |

### 3.3 核查结论

- **修复率可观（抽查 8 项修 2 + 此前三批推进），但全部"未做"项都停留在 9-22 评审文档的表格里**：无 GitHub issue（`.github/` 仅 workflows）、无 backlog 文档、后续 Plan 文档零引用——**遗留项不具备可跟踪性，是本次认定的首要流程缺陷**（§4 N4）。

---

## 4. 新发现问题（既往评审未覆盖）

### N1（P1）macOS App target 零测试，且与自家架构文档相悖

- `Package.swift:56`：测试 target `dependencies: ["VoiceStickCore", "COpus"]`——**16,826 行 App 代码（`VoiceStickCoordinator.swift` 2,522 行、`AppConfig.swift` 1,736 行、`BleCentral.swift` 1,277 行、`OggOpusMuxer.swift` 等）无任何测试触达**；Windows 把同职能（协调器/配置/Ogg/ASR 帧）全放可测 core，两端分层不对称。
- `Doc/Agent/desktop-architecture.md:11` 明确写着 Core 含 "Ogg Opus mux、ASR 帧格式、配置解析…新增核心行为优先放这里"——**文档描述与源码不符**，且这正是缺陷温床：最新提交 `d56c0fc` 修复的正是"测试只链 Core 未拦截、App 编译失败"的发布级事故。

### N2（P1）跨端契约零 fixtures，"三处一致地错"已实锤

- 仓库仅有 `scripts/e2e_test/fixtures/`（ATVV 录采），**无任何跨端 golden-frame 资产**；既往评审"最高性价比②"未落地。
- 实锤：① `ogg_opus_muxer.cc:30` / `OggOpusMuxer.swift:32` 按 960 样本/包累加 granule（16k 下 2880 tick＝60ms），固件实际 40ms（应 1920）→ 落盘 Ogg 时长虚高 50%，**两端一致地错**；② `protocol.md:52` 也写 60ms → 文档跟着一起错；③ 这类"同源移植把错误也移植过去"的家族缺陷，只有 golden 对拍能系统性拦截。

### N3（P1）CI 缺 website 构建与 release-guard

- `ci.yml` 无 `npm run build`：网站编译错要到 `deploy-website` 才暴露；且 COS 整站同步有"版本号构建时内联进 JS、必须重建"的血泪坑（`Doc/Expe/cos-website-fullsite-sync-traps-2026-09-27.md`），CI 预跑是最低成本保险。
- 无 release-guard job：VERSION/tag/CHANGELOG/网站 downloads 一致性、protocol 常量与三端源码对拍、Hub 三份 diff、i18n 键奇偶——目前全靠人工约定（本次体检即靠手工脚本完成）。

### N4（P1）既往遗留项无跟踪载体

见 §3.3。建议建立 `Doc/Plan/backlog.md`（或 GitHub Issues）逐条登记：编号、来源（9-22 评审对应编号）、级别、状态、验证方式。

### N5（P2）三份同源 Hub 已出现实际漂移

`AGENTS.md:92-93`（两条 bullet）在 `CLAUDE.md:95`/`CODEBUDDY.md:95` 被并成一行，另多一处空行差异——"修改整体性内容时同步更新三份"靠人肉已失守。同理 `skills/sticks3-flash-ota/SKILL.md` 与 `.agents/skills/sticks3-flash-ota/SKILL.md` 为同 MD5 双份。

### N6（P2）仓库卫生

- 根目录 9 个 .bat（`_build_temp`/`_wt_build`/`_wt_ctest`/`build_incremental`/`build_manual`/`build_native`/`build_win`/`do_build`/`test.bat`），其中多个被 AGENTS.md 自己标注"含绝对路径/占位勿复用"；
- `m0/`、`p1/` 在 `.gitignore`（`/m0/`、`/p1/`）但历史 48+61 文件仍被跟踪——ignore 对已跟踪文件无效，语义暧昧；
- `docs/superpowers/`（17 文件）为历史产物未归档；根目录 `.publish-finalize`、`ArduFlux.json`、`skills-lock.json`、`_wt_*.bat` 等散落；
- `website/package.json` 版本 1.9.0 无说明。

### N7（P2）上帝对象与接口膨胀（结构治理欠账）

- 单文件 Top：`ble_central_win.cc` 4,016 / `voice_stick_coordinator.cc` 3,756 / `win32_app.cc` 3,378 / 固件 `main.c` 3,370 / macOS `VoiceStickCoordinator.swift` 2,522（116 函数）；
- `voice_stick_coordinator.h:75` 的 `BleCentral` 抽象接口 **67 个 virtual（36 个 Send*）**，随功能线性膨胀，Fake 与两平台实现被迫同步背负全部方法；
- 与 §2.4 refactor 仅 1.6% 互为因果。

### N8（P2）`core_tests.cc` 单文件 15.9k 行 + `main()` 手工注册

`core_tests.cc:15485` 起的 `int main()` 逐行手工调用全部测试函数——**漏注册即静默不跑，无任何告警**；单文件也使多人并行修改必然冲突。

### N9（P3）本地化三套实现无奇偶守护

网站 zh/en（100/100，当前对齐✅）、macOS `Localization.swift`（770 行）、Windows `localization.cc`（1,034 行）各自维护；网站键奇偶当前靠约定（本次手工验证对齐），桌面两端与网站之间无任何共享键清单。

### N10（P3）可观测性与文档分级

- `Doc/Plan` 89 篇/15,040 行无状态标注（done/superseded/active），既往评审已发现 `release-and-security.md` 版本过期类漂移；
- 既往评审 P3 所列"dropped 计数无读取、ATVV ERROR 只一行 WARN"等可观测性项本次未见闭环证据【引自既往评审】。

---

## 5. 优化方案（二期，分阶段）

### 5.1 阶段 0（本周）：把"人肉约定"变"机器校验"

| 动作 | 具体步骤 | 验收 |
|---|---|---|
| 0.1 跨端契约 fixtures 立项 | 新建 `tests/contract/fixtures/*.bin` + 期望解析 JSON；首批覆盖 audio/state/motion/OTA begin·data·end·abort·state、控制事件全集；固件 host、Swift runner、Windows 单测三端各解析一遍进 CI | 三 job 全绿；**顺手抓出并修复 granule 2880→1920 与 protocol.md 60ms→40ms**（作为 fixtures 首批战果） |
| 0.2 release-guard 脚本 + CI job | ① VERSION==firmware/version.txt==tag==CHANGELOG==网站 downloads；② protocol.md 常量反向 grep 三端源码；③ 三份 Hub 归一化 diff；④ 网站 i18n 键奇偶（桌面键清单可后补）；⑤ 复用既有 `scan_release_artifacts.py` 扫公开产物 | 任一不一致 CI 红；先修现存 N5 漂移再上闸 |
| 0.3 网站进 CI | `ci.yml` 加 `npm run build`（缓存 node_modules） | PR 上即可拦网站编译错 |
| 0.4 遗留 backlog 建立 | `Doc/Plan/backlog.md` 登记 9-22 评审全部未闭环项（P0-2、A1-A4/A6、B1/B8/B9、C1-C5、E7/E8、D1/D3/D9、granule…），标注级别/证据/验收 | 每项有编号、状态与验证方式；此后评审文档只引用编号 |

### 5.2 阶段 1（1–2 周）：关闭高危遗留（按数据风险排序）

1. **B8/B9 配置竞争与非原子写**（数据丢失面最大）：`config_` 改 `shared_ptr<const AppConfig>` 原子换入；写盘改临时文件+原子替换并保留 `[license]`。验收：并发读写压力无竞争、掉电不丢配置。
2. **B1 烧录工具关窗 UAF**：析构改硬同步（Job Object + 取消令牌 + 等待 worker 真退出），验收：烧录中途关窗无崩溃。
3. **P0-2 固件 BLE 鉴权**（需产品决策与真机）：`control_rx/ota_rx` 加 `WRITE_ENC`+MITM、评估 Secure Boot v2、`test_playback` 收进调试构建；至少先在 README 明示威胁模型并列入发布阻断项。
4. **A4 多连接表**：`voice_ble` 按 `conn_handle` 维护 peer 表，替换单值；显示层"落表兜底"的绕行随之退场。
5. **E8 固件 host 测试 CI 化**：`run_tests.py` 改 CMake/CTest（gcc/clang 可跑）并进 `ci.yml` firmware job。

### 5.3 阶段 2（1–2 月）：结构收敛

1. **macOS 分层对齐 Windows（N1）**：把协调器状态机、`AppConfig`、`OggOpusMuxer`、ASR 帧格式下沉 `VoiceStickCore`（这正是 `desktop-architecture.md:11` 的既有要求）；测试 target 覆盖状态机；同时提供统一本地验证入口（`swift build && swift run VoiceStickTests` 一键脚本），杜绝 `d56c0fc` 式事故。
2. **同源模块对拍机制**：10 对逐行移植模块共享同一组输入/期望 fixtures，两端测试各跑一遍——把"移植靠自觉"变 CI 断言（与 0.1 的 fixtures 体系合并建设）。
3. **上帝对象拆分（N7）**：Windows `voice_stick_coordinator.cc` 按域拆（session/ASR/OTA/热词）；`BleCentral` 67 virtual 按域分组为 `IControlSender`/`IOtaSender`/`IGatewaySender`；固件 `main.c` 拆 power/settings/gesture/ota 编排（gateway 组件已有拆分先例）。
4. **`core_tests.cc` 拆分（N8）**：按模块拆翻译单元 + 注册宏（或引入 doctest 单头库），消灭 `main()` 手工注册清单。
5. **协议单一事实源深化**：protocol.md 的常量/默认值/能力清单由脚本反向校验源码（0.2 的扩展），控制帧补 JSON schema。

### 5.4 阶段 3（持续）：卫生与文档治理

1. 仓库卫生（N6）：根 .bat 归并 `scripts/`（删除已标废弃者）、m0/p1 显式归档（移入 `archive/` 或从索引移除并 README 标注）、`docs/superpowers/` 归档、skills 双份去重、`website/package.json` 版本标注。
2. Hub 生成式（N5）：单一源 + 脚本生成三份（保留各自头注释），至少先由 0.2 的 diff 闸守护。
3. `Doc/Plan` 状态标注（N10）：89 篇补 done/superseded/active 头部标记；新设计方案照旧放 `Doc/Plan/`。
4. 本地化键清单共享（N9）：提取三端公共键清单进 CI 奇偶校验。
5. 定期重构冲刺：把 refactor 占比从 1.6% 提到可观测水平，与 backlog 关闭节奏绑定。

---

## 6. 优先级矩阵与验收指标

| 优先级 | 事项 | 验收指标 |
|---|---|---|
| P0 | 0.1 契约 fixtures | 三端 CI 全绿；granule/60ms 两处实锤被抓获并修复 |
| P0 | 0.2 release-guard | 版本/Hub/协议不一致必红；现存 N5 漂移清零 |
| P0 | 0.4 遗留 backlog | 9-22 未闭环项 100% 有编号与状态 |
| P1 | B8/B9 配置原子化 | 压力无竞争；掉电/被杀不丢配置与 `[license]` |
| P1 | 0.3 网站进 CI | PR 即验 `npm run build` |
| P1 | N1 macOS 分层下沉 | App 关键路径有测试；`swift build` 全量验证成为提交前必跑 |
| P1 | P0-2 固件鉴权（决策后） | 未配对写入被拒；`test_playback` 正式构建不可达 |
| P2 | A4 多连接表 / B1 UAF / E8 host 测试 | 入侵者连接不污染切换器；烧录中关窗无崩溃；host 测试进 CI |
| P2 | N7/N8 结构拆分 | 最大单文件 <2k 行；测试注册零手工清单 |
| P3 | N5/N6/N9/N10 卫生项 | Hub diff 闸绿；根目录无散落脚本；Plan 带状态标注 |

---

## 附录 A：证据索引（本次实测，@d56c0fc）

- **契约漂移**：`ogg_opus_muxer.cc:30`、`OggOpusMuxer.swift:32`、`Doc/Ref/protocol.md:52`、`firmware/components/audio_pipeline/audio_pipeline.c:32`。
- **macOS 测试边界**：`desktop/macos/Package.swift:54-57`；`Sources/VoiceStickCore/`（15 文件）；`Sources/VoiceStickApp/VoiceStickCoordinator.swift`（2,522 行）；`d56c0fc` 提交信息。
- **遗留项**：`firmware/components/voice_ble/voice_ble.c:52,774,779,1189-1191`；`firmware/sdkconfig:491-492`；`desktop/windows/src/voice_stick_coordinator.h:75,293,642,644`；`desktop/windows/src/flash_tool_dialog.cc:209-215`；`firmware/components/gateway/test/run_tests.py:17`；`scripts/update-appcast.py:24`。
- **测试与 CI**：`desktop/windows/tests/core_tests.cc:15485`（main 手工注册）、`:126`（FakeBleCentral）；`.github/workflows/ci.yml`（macos/firmware/windows/script-tests 四 job，无 website/contract/guard）。
- **文档漂移**：`AGENTS.md:92-93` vs `CLAUDE.md:92,95` vs `CODEBUDDY.md:95`；`Doc/Agent/desktop-architecture.md:11`；`website/package.json:4`。
- **卫生**：根目录 9 个 `.bat`；`.gitignore` 的 `/m0/`、`/p1/` 与 `git ls-files m0 p1`（48+61）；`skills/sticks3-flash-ota/SKILL.md` 与 `.agents/skills/` 同 MD5（350622b9…）。
- **统计口径**：规模为 find/wc 实测（排除 third_party/COpus/.build/node_modules/build）；提交统计 `git log --since='3 months ago'`（731/1,339）。

## 附录 B：方法与局限

- 本机未执行构建与测试（无 ESP-IDF/MSVC；Swift 构建需代理且耗时），CI、构建、运行时行为结论均为**静态核查**；"CI 在"不等于"CI 最近一次绿"，引用前建议看一眼 Actions。
- 标【引自既往评审】的条目（B6 修复、可观测性清单等）沿用 9-22 结论，未重新实证。
- 行号对应 `d56c0fc`（2026-10-03），此后代码变动即失效——与仓库既有约定一致。
- 既往评审的完整缺陷清单（P0-P3 全量）见上游文档，本文只核查与增量，不构成替代。
- 未逐项复读 m0/p1 内部代码，其结论仅限目录与 git 跟踪状态。
