# Typeless 逆向结论借鉴与"越用越准"用户数据飞轮方案

> 状态:提案(待实施)。来源:2026-10-15 对 Typeless 2.8.2 (arm64, `Typeless-2.8.2-arm64.dmg`) 的静态逆向分析 + 本仓库代码盘点。
> 目标读者:负责推进优化的 AI/开发者。文档自包含,无需回溯原始分析对话。
> 原始分析工件(临时,可能已清理):解混淆主进程 `/tmp/typeless-main-deob.js`、ASAR 解包 `/tmp/typeless-asar/app`、REA JS 应用图 `/tmp/typeless-asar-analysis.json`。DMG 原件在 `/Users/hwp/Dev/DVG/Typeless/`,可复现。

---

## 一、背景:Typeless 是怎么做的(逆向事实)

Typeless(macOS AI 听写输入法,Electron 33 + 6 个 Swift 原生 dylib 经 koffi FFI 调用)的完整听写闭环:

```
CGEventTap/IOHID 监听裸按键(默认 Fn 触发)
  → 渲染进程 getUserMedia 采音(单声道/16bit,关闭 EC/NS/AGC)
  → 主进程 Opus 持续编码(chunkSessionWorker)
  → 并行采集焦点上下文(AX:应用 bundle id/窗口标题/域名 + 输入框 DOM id/光标状态/字段全文)
  → multipart POST api.typeless.com/ai/voice_flow(audio/ogg + 上下文 + 模式)
  → 服务端返回 refine_text/delivery/external_action
  → 客户端注入(CGEvent 逐字击键合成纯文本;富文本走 剪贴板+模拟⌘V+还原)
  → ★ 注入后启动"编辑追踪":监听按键 + 轮询焦点字段,diff 用户修改 vs 注入文本,存库上报
```

对"越用越准"最关键的三个设计:

1. **编辑追踪(edited-text tracking)**——上屏之后才算完。用户对 AI 文本的手动修改是最高价值信号(用户即 ground truth),带状态机(`edited_text_status: NOT_EXTRACTED→…` + `edited_text_attempts` 重试计数 + `track_timeout` 超时兜底)。
2. **持久化会话历史**——本地 SQLite(drizzle 迁移),`history` → `history_v2` 两代表演进,音频与文本分离存路径,行级云同步状态机。
3. **上下文分层 + 黑白名单**——每次请求带焦点应用快照;服务端下发 app/URL 黑白名单(版本化+日缓存)控制采集面。

### 1.1 Typeless `history_v2` 表结构(逆向还原,可直接裁剪复用)

v1 字段(迁移 0000~0010 累积):`id, refined_text, audio(blob), audio_context, status, duration, app_version, edited_text, edited_text_status(默认 NOT_EXTRACTED), edited_text_attempts(默认 0), languages, mic_device, detected_language, hasRevertedAI, ax_text, ax_html, created_at, updated_at, debug_info, audio_local_path, focused_app, audio_metadata, focused_app_name, focused_app_bundle_id, focused_app_window_title, focused_app_window_web_title, focused_app_window_web_domain, focused_app_window_web_url, mic_device_info(blob), mode(默认 voice_transcript), mode_meta, client_metadata`

v2 增量(0011~0013):`user_id, server_updated_at, sync_status(默认 pending_upload), sync_attempt_count, synced_from_cloud` + 部分索引 `idx_history_v2_push_candidate_v2 (user_id, sync_status, created_at, sync_attempt_count) WHERE status='completed' AND refined_text 非空`(0013 扩展为:或 `mode='voice_command' 且 mode_meta 的 delivery 为 external`)——部分索引圈定"待上传候选"。

### 1.2 Typeless 原生层要点(与文本注入/上下文相关)

- 纯文本注入:CGEvent 击键合成(`CGEventCreateKeyboardEvent/CGEventPost`),按当前键盘布局逐字映射 keycode,**不用** AXUIElementSetValue。
- 富文本注入:存档 NSPasteboard → 写入 → CGEvent 模拟 ⌘V → 还原剪贴板(带 `org.nspasteboard.TransientType` 标记)。
- 读选区:AX 参数化读取(支持 Google Docs 的 AXTextMarker 区间);失败回退"模拟 ⌘C → 读剪贴板 → 还原"。
- 上下文:`AXEnhancedUserInterface` 开关(浏览器增强树)、可见文本分页收集 + CaptureTimeout 超时。
- 采集面:Typeless 把输入框**字段全文**随音频上传——本方案明确不照搬(见 §五)。

---

## 二、VoiceStick 现状盘点(已有资产)

| 资产 | 位置 | 状态 |
|---|---|---|
| 热词评分裁剪 | `desktop/macos/Sources/VoiceStickCore/HotwordSelector.swift`(与 Windows `hotword_selector.{h,cc}` 1:1) | score = 1.0·log1p(count) + 0.5·exp(-age/30d) + 2.0·manual;LLM prompt 上限 50 词;corpus 预算 80 tokens |
| 候选热词挖掘 | `desktop/windows/src/hotword_candidate_miner.{h,cc}` | **仅 Windows**。数据源=LLM 精修 diff(refined 有而 original 无的标识符样式词);阈值 3 次人工确认;`counts/dismissed/notified` JSON store;**CJK 未挖掘**;模块内已有磁盘事务收敛(B13) |
| 划词加词 | `desktop/windows/src/selection_hotword_manager.{h,cc}` | **仅 Windows**,LLM 提炼热词 |
| 跨轮精修上下文 | `desktop/windows/src/refine_history.{h,cc}` | **内存环形缓冲**,5 轮 / 120s TTL,不落盘 |
| 调试音频留存 | `debug_audio_cache` → Ogg Opus 文件 | 每段有效识别保存,<0.5s 丢弃 |
| 评测基建 | `scripts/e2e_test/asr_bench/` | 纯 stdlib 协议、真实凭据;热词专项见 `Doc/Plan/hotword-eval-and-prioritization.md` |
| 本地 ASR/精修 | SenseVoice(sherpa-onnx,不支持热词)+ 本地 Qwen3-4B KV 续写跨轮纠错 | 见 `Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md` |
| 云端 ASR 热词能力 | 火山 boosting/correct 表、腾讯词表 | 桌面端已实现同步(`TencentASRVocabClient.swift` 等) |
| 持久化会话历史 | **无** | 全部 JSON/TOML 本地文件,无 SQLite |
| 用户编辑行为追踪 | **无** | 粘贴后不感知用户修改 |

**平台 ASR 热词限额(已调研,见 hotword RFC §一):**

| 平台 | 机制 | 限额 |
|---|---|---|
| 腾讯实时识别 | 签名参数 `hotword_list`(词\|权重 1-11) | 每请求 **128 词**;词表 30 张×1000 词 |
| 火山 bigmodel | `corpus.context.hotwords` 直传 | 双向流式 **100 tokens**(官方建议 30-50 短词);二遍 5000 词 |
| 火山 | `corpus.boosting_table_id` / `correct_table_id` | 流式 100 tokens;二遍 5000 词;correct 命中强替 |
| 火山 | `corpus.context.dialog_ctx` | **800 tokens / 20 轮**——当前未使用,见 P1-6 |

---

## 三、差距 → 目标

核心命题:**让识别"越用越准",用户热词库成为可积累、可迁移的核心资产。**

当前断点:挖掘只有"LLM 精修 diff"单通道;会话数据不持久;粘贴后用户修改完全丢失;热词无上下文分层;没有能量化"变准了多少"的闭环。Typeless 的启示:**上屏之后才算完**——编辑追踪 + 持久化历史 + 上下文分层把一次性转写变成复利资产。

---

## 四、AI 模型复用结论(直接回答)

- **Typeless 的 AI 模型不可复用**:识别/润色全在其服务端(`refine_text`/`delivery` 是响应字段),客户端逆向拿不到模型。
- **可复用的是数据模型/状态机/表结构**:`history_v2` schema、`edited_text` 状态机、热词候选 `counts/dismissed/notified` store(与本仓库 `HotwordCandidateStore` 同构,互相印证)、黑白名单 schema(反向用作"不追踪应用"名单)、mode/delivery 枚举。
- 本仓库现有模型栈(火山/腾讯/SenseVoice + Qwen3-4B)已等价,无需替换。

---

## 五、落地路线

### P0 — 数据底座 + 编辑追踪(最高优先)

> **落地注记（2026-10-15）**：P0-3 通道 2 的纠错对素材基础已落地——精修 diff 最小替换对提取与落库见 `correction_pairs` 模块（wetype 方案 §2 同构），编辑 diff（edited_text 对）接入随 P0-2 编辑追踪落地。

**P0-1 SQLite 会话历史表**(macOS 用 GRDB,Windows 直接 sqlite3;不引入 Electron 栈的 drizzle)

裁剪版 schema(VoiceStick 定位本地优先,砍掉 Typeless 的云侧字段):

```sql
CREATE TABLE history (
  id TEXT PRIMARY KEY,              -- 会话 UUID
  created_at INTEGER NOT NULL,      -- epoch 秒
  device_id TEXT,                   -- 触发设备(StickS3 / 小米遥控)
  asr_provider TEXT,                -- volcengine / voicestick_cloud / tencent / local
  mode TEXT,                        -- original / translate(+目标语言入 mode_meta)
  raw_asr TEXT,                     -- ASR 原文
  refined_text TEXT,                -- 精修后注入的文本
  injected_text TEXT,               -- 实际注入内容(与 refined 可能不同,如翻译)
  edited_text TEXT,                 -- ★ 用户注入后修改的结果(编辑追踪产出)
  edited_text_status TEXT DEFAULT 'NOT_EXTRACTED',  -- NOT_EXTRACTED/PENDING/EXTRACTED/TIMEOUT/SKIPPED
  edited_text_attempts INTEGER DEFAULT 0,
  audio_local_path TEXT,            -- 复用 debug_audio_cache 目录约定
  duration REAL,
  focused_app_bundle_id TEXT,       -- 仅 bundle id/进程名,不读窗口内容
  hotwords_sent TEXT,               -- JSON 数组:本会话下发的热词集(命中归因用)
  mic_device TEXT,
  client_metadata TEXT              -- JSON:app 版本/音频链路等
);
```

迁移机制仿 Typeless:迁移 SQL 随包分发、启动时按序应用,保证 schema 演进不破坏存量资产。保留策略:默认保留(本地资产),`historyDurationSeconds` 类配置裁剪。

**P0-2 编辑追踪窗口**

- 时机:文本注入成功后启动,窗口 30~60s 或到下一次录音开始,取先到者。
- 采集:macOS 经 AX 读取焦点元素值(`AXFocusedUIElement` → `AXValue`/`AXSelectedText` 参数化读取,Typeless 同款);Windows 经 UIA。**边界:只 diff 自己注入的文本区间**,不保存整个字段内容(隐私红线,见 §六)。
- 产出:`edited_text` = 注入文本应用用户修改后的结果;diff 对(`注入 → 编辑后`)即为纠错素材。落库带状态机与 attempts(读取失败重试,超时置 TIMEOUT),实现参考 Typeless `edited_text_status/attempts/track_timeout` 三件套。

**P0-3 挖掘三通道化**(扩展 `hotword_candidate_miner`,macOS 同步补齐移植)

1. 精修 diff(现有,仅 Windows→移植 macOS);
2. **用户编辑 diff(新)**:`edited_text` vs `injected_text`,提取被用户改写的词对——同音/近音对(如 鱼器渍→语气词)是 correct_table 与热词的最佳素材;拉丁标识符对沿用现有 `LooksLikeIdentifier` 过滤;
3. `HotwordSelector.recordUsage` 升级:除计数外,把"下发热词是否在 final 文本命中"落库(命中归因,现有子串匹配逻辑复用)。

纠错对落地:火山 `correct_table_id` 候选(命中强替,第二遍生效)+ 腾讯词表;仍走"阈值 + 人工确认"防负反馈(miner 头注释已论证全自动入表的风险,维持该原则)。

**P0-4 macOS 补齐划词加词**

macOS 读选区照搬 Typeless 方案:AX 参数化读 `AXSelectedText`,失败回退"模拟 ⌘C → 读剪贴板 → 还原(带 TransientType 标记)"。`selection_hotword_manager` 的 LLM 提炼逻辑平台无关,可移植。

### P1 — 上下文分层与反馈

**P1-5 焦点应用 → 热词 profile 分层**:录音开始时取焦点应用(`NSWorkspace.frontmostApplication` / Windows 现有能力),维护 `app → 热词子集或权重加成` 的 profile(配置文件形态,如 `[hotword_profile."com.microsoft.VSCode"] weight_boost = 2.0`)。`HotwordSelector.score` 加 profile 加成项,同样 80-token 预算下提高有效命中率。配"不追踪应用"名单(反向借鉴 Typeless 黑白名单 schema:版本化 + 本地缓存;密码管理器/银行类默认列入)。

**P1-6 dialog_ctx 接线(零新数据,纯接线收益)**:最近 2~3 轮**精修后的正确文本**作为火山 `corpus.context.dialog_ctx`(800 tokens/20 轮额度)随 ASR 请求发送,让同音纠错在第一遍发生。数据源:扩展 `RefineHistory`(仍内存)或读 history 表最近 N 行 completed 记录。

**P1-7 热词命中率遥测与自动降权**:基于 P0-3 的命中归因,长期(如 30 天滑窗)零命中的热词自动降权,预算流向活跃词。`recordUsage` 的 count 语义升级为"命中次数"。

### P2 — 评测闭环(把"越用越准"变成数字)

**P2-8 个人评测语料自动生成**:history 表中 `(audio_local_path, edited_text 或 refined_text)` 对 → 生成个人评测集(优先收编历史错例,与 hotword RFC §2.1"从真实错误取材"原则一致)。注意:仅本地使用,默认不外传。

**P2-9 定期离线重放**:复用 `asr_bench` 三档对比——无热词 / 当前裁剪热词 / 全量热词——产出 WER/热词命中率趋势。用途:① 验证分层与降权策略有效;② 换 ASR 供应商决策依据;③ 产品侧"已为你优化 X%"素材。

### 远期(若做账号/多端同步)

Typeless 参考件:行级 `sync_status + sync_attempt_count + 部分索引圈定上传候选`;本地敏感字段信封加密(`_encrypted/_v/_type/_key/_data` 信封 + 服务端公钥);热词库导出/导入(JSON)先做,同步后做。

---

## 六、明确不照搬(红线)

1. **不读/不上传输入框字段全文**:Typeless 把 `full_field_content` 随音频上传服务端。本方案编辑追踪只 diff 自己注入的文本,上下文只取 app bundle id。
2. **不做全局键盘 tap**:硬件按钮触发,不碰 Fn/CGEventTap 权限面。
3. 不采用 `NSAllowsArbitraryLoads`、内嵌第三方 API key 等做法(Typeless 反例)。
4. 编辑追踪与上下文采集必须受"不追踪应用"名单约束,且默认最小化。

---

## 七、验收口径

- P0:一次真实使用后,history 表出现完整行;人为修改注入文本,`edited_text` 正确捕获且 diff 对进入候选通道;macOS 划词加词可用。
- P1:同一 80-token 预算下,分层热词在个人评测集命中率 ≥ 全局裁剪基线;dialog_ctx 生效轮次的第一遍同音错误率下降。
- P2:asr_bench 产出可复现的 WER/命中率趋势报表;"越用越准"有量化曲线。
- 全程:不追踪名单内应用零采集;网络出口不新增域名(除既有 ASR/LLM 配置)。

## 八、实施注意事项(来自现有代码约定)

- 候选文件写入必须走 `hotword_candidate_miner` 的磁盘事务收敛模式(B13:进程级互斥 + 每次从磁盘重读后改写),新通道入词不得旁路。
- macOS 与 Windows 的新模块保持 1:1 移植约定(HotwordSelector 先例),共享纯逻辑放 `VoiceStickCore` / Windows 对应层。
- 热词长度硬约束沿用 `isValidHotword`(≤10 汉字 / ≤30 英文字符,无空白)。
- 配置新增项进 `config.toml` + `desktop/macos/Config/config.example.toml` 同步示例;勿提交凭据。
