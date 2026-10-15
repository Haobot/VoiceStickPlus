# 用户数据飞轮：借鉴微信输入法 (WeType) 的个性化架构
> 状态：RFC（待实施）。前置研究：WeType macOS 3.0.0 (build 670) 逆向分析 + 本仓库现状盘点（2026-10-15）。
> 目标读者：推进实施的 AI / 开发者。本文自包含，不依赖原对话上下文。

## 0. 一句话目标

让 Voice Stick 具备**用户数据的持续挖掘能力**：识别与精修结果随使用越来越准，沉淀的用户热词库 / 纠错对库成为可导出、可同步的核心资产。

## 1. 背景与前置研究结论

### 1.1 WeType 逆向结论（与本目标相关的部分）

对 `/Library/Input Methods/WeType.app` 主二进制（arm64 切片，25 万符号）的静态逆向发现，其「越用越准」由一个约 60 个 `user_*` 源文件的**用户数据飞轮**支撑（C++ 命名空间 `wxime`，全部本地运行）：

| WeType 机制 | 证据（符号/源文件名） | 作用 |
|---|---|---|
| 完整动作历史 | `user_action_history`、`stat_user_actions_wrapper`、`emit_and_drop_recorder`（上屏与回退都记录） | 正负样本都采集，不只记命中 |
| 纠错对资产 | `UserCorrectedTextDict`、`user_word_with_syllable_correction_dict`（存「错误形式→正确形式」映射） | 用户纠正过的配对是最强个性化信号 |
| 热词三段管道 | `user_hot_word_detector → classifier → handler`，`WXIME_EVENT_USER_HOT_WORD_RECOMMEND` 事件推荐用户确认 | 自动挖掘 + 用户确认入表，防脏词 |
| 热词质量管理 | `user_block_list_dict`（黑名单）、`potential_user_dict_clean_strategy`、`user_dict_clean_upper`（容量上限清洗） | 防错误热词带偏识别（负反馈）与库膨胀 |
| 场景分域 | `createSession(bundleID:)` per-app 会话、`user_scenes_session`、对微信/企微专属策略 | 不同 App 不同词域 |
| 上下文共现 | `user_bigram_dict`、`user_association_hash_dict/profile`、`AssociationPersonalModel` | 词不是孤立的，带搭配概率 |
| 资产化同步 | `user_dict_v2` + `user_dict_syncer`（全量/增量）+ `user_dict_v2_patch` + `wxime_kv`/`ttl_kv` | 用户词典 = 版本化可同步资产 |
| 双阶段精修 | Phase A = ASR 快速出稿，Phase B = 云端 LLM 润色带超时回退（日志串 `enter Phase B (polish), timeout=%dms` 实证） | 速度与质量兼顾，弱网可回退 |
| 本地 VAD | `WX_ASR_VAD_*` C API + `Resources/vad/` 模型 | 本地断句/端点检测 |

关键分工：**个性化/延迟敏感的放本地（热词、纠错、重排），生成式的放云端（ASR 解码、润色）**。WeType 的模型权重（`nlrgpt2_nnlm.bin` 等）为私有格式且权属腾讯，**不可复用**，可复用的是上表的机制（详见 §5）。

### 1.2 本仓库现状盘点（代码实证）

已有且方向正确（不要推倒重来）：

| 能力 | 代码位置 | 现状 |
|---|---|---|
| 热词评分与预算裁剪 | macOS `desktop/macos/Sources/VoiceStickCore/HotwordSelector.swift`（Windows `desktop/windows/src/hotword_selector.{h,cc}` 1:1 同源）；评分 `1.0·log1p(count) + 0.5·exp(-age/30d) + 2.0·manual` | ✅ 双端一致 |
| 双出口①：ASR 词表同步 | macOS `TencentASRVocabClient.swift`（腾讯 hotword_id 表）、火山 corpus（见 `Doc/Plan/hotword-eval-and-prioritization.md`：腾讯 128 词/请求·火山 100 tokens 双向流式） | ✅ |
| 双出口②：LLM prompt 注入 | `VoiceStickCoordinator.hotwordsForLlmPrompts()` top-50（`kHotwordPromptMaxWords`） | ✅ |
| 使用反馈（仅正信号） | `VoiceStickCoordinator.recordHotwordUsage(fromText:)`（约 L2240-2265）——最终文本命中热词计数+时间戳，立即落盘 `hotword_usage.json` | ✅ 但维度单一 |
| 拉丁词挖掘（仅 Windows） | `desktop/windows/src/hotword_candidate_miner.{h,cc}`：精修 diff 中 identifier 样式新词，阈值 `kHotwordCandidateThreshold = 3` 次确认入表；B13 已修复单写者问题 | ✅ 仅 Windows |
| 划词 LLM 提炼（仅 Windows） | `desktop/windows/src/hotword_extractor.h`（复用 LLMChatClient） | ✅ 仅 Windows |
| LLM 精修流式 + 预算回退 | macOS `LLMRefinementClient.swift` / `RefinerServing.swift`；超时回退原文（等价 WeType Phase A/B） | ✅ |
| 存储 | `config.toml`（`asr_hotwords` 字符串）+ `hotword_usage.json` + Windows `hotword_candidates.json` | ⚠️ 三处分散，JSON 无版本化 |
| 前台 App 感知 | macOS `FrontmostAppProvider.swift`（目前仅用于输出目标） | ⚠️ 未用于热词分域 |

### 1.3 差距总表

| # | 差距 | 对应 WeType 机制 | 优先级 |
|---|---|---|---|
| G1 | 精修 diff 挖完热词即丢弃，纠错对未资产化 | UserCorrectedTextDict | **P0** |
| G2 | macOS 无任何挖掘管道（identifier 挖掘 + LLM 提炼均缺） | 热词三段管道 | **P0** |
| G3 | 无负信号（取消/重录/确认时长）采集 | UserActionHistory | P1 |
| G4 | 热词无 App 场景分域，混合稀释 ASR 直传预算 | per-app session | P1 |
| G5 | 中文热词无自动挖掘（仅划词手动 + 拉丁 identifier） | hot_word_detector + noun_tag | P1 |
| G6 | 存储分散 JSON，无导出/同步/版本化 | user_dict_v2 + syncer | P2 |
| G7 | 无上下文共现（bigram） | user_bigram_dict | P2 |
| G8 | 无热词黑名单/自动降权（负反馈防线缺 2/3 道） | user_block_list + clean_strategy | P1（随 G1 一起做） |

## 2. P0-1：纠错对资产化（体感提升最大）

**目标**：同一个错误第二次不犯。这是「越用越准」体感的第一来源。

**方案**：

1. 新增纠错对库（先 JSON 后随 G6 迁 SQLite， schema 预留）：
   `corrections(before: String, after: String, count: Int, last_seen: Int64, app: String?)`
   - 采集点：每次精修完成时计算 `original → refined` 的词级 diff（Windows 已有 `MineRefinementCandidates` 的 diff 逻辑可扩展；macOS 新写）。仅保留「词级替换/插入」形态的对，整句改写不存。
2. **注入精修 prompt**：`hotwordsForLlmPrompts()` 扩展为两段——热词段（现状 top-50）+ 纠错 few-shot 段（按 `count·recency` 排序 top-N 对，如 8 对），格式示例：
   ```
   用户历史纠正（输出时请遵循这些偏好）：
   - "克劳德" → "Claude"
   - "魏碑" → "WeBank"
   ```
   - 与热词段同样受 prompt 长度预算约束，超限先裁 few-shot 再裁热词。
3. **热词反向校准**：统计热词出现在纠错对 `before` 侧的频率；某词 `纠错次数 / 命中次数` 超阈值（建议 0.3 起调）时自动降评分权重并可在设置页提示移除。这是 G8 防线之一。

**验收**：
- 单测：diff 提取（纯函数）、prompt 拼装预算裁剪、反向校准统计。
- E2E 手测：构造同一错词两次输入，第二次精修 prompt 含该纠正对。
- 双端行为一致（prompt 段构造函数各自单测钉死）。

## 3. P0-2：macOS 补齐挖掘管道（双端能力对齐）

**方案**：

1. 移植 Windows `MineRefinementCandidates` + `ExtractIdentifierTokens`（纯函数，Swift 化，参考 `HotwordSelector.swift` 1:1 移植先例与 `scripts/e2e_test/asr_bench/hotword_select.py` 对齐惯例）。
2. 候选存储对齐 Windows `hotword_candidates.json` 语义（counts/dismissed/notified），**注意 B13 教训**：macOS 从第一行起就走单一写入口（进程内串行队列 + 临时文件改名落盘），不做 load-once 缓存整存。
3. 达阈值弹通知、设置页确认入表，与 Windows UX 对齐。

**验收**：macOS 单测覆盖提取/阈值/忽略不回弹（对应 Windows `TestHotwordCandidatesSingleWriter` 语义）；i18n 键双端对齐（`check_i18n`）。

## 4. P1 项（按 ROI 排序）

### P1-1 负信号采集（G3）
- 状态机埋点（`VoiceStickCoordinator` / Windows coordinator）：
  - 识别后**取消粘贴 / 侧键取消 / 重录** → 对本次结果文本记负事件；
  - **倒计时自动确认 vs 手动确认**、确认前停留时长 → 疑惑度信号。
- 落入 events 流水（G6 的载体；G6 未做前先 JSONL 追加）。
- 用途：热词评分扩展负样本维度（`score -= w·negative_count`）；纠错对挖掘只取「用户实际放行」的样本。
- 验收：状态机各取消路径均有事件；评分函数含负项单测。

### P1-2 App 场景分域（G4）
- `usage` 与 `corrections` 加 `app` 字段（bundle ID，`FrontmostAppProvider` 已有；Windows 取前台窗口进程）。
- 热词选桶策略：会话开始时按当前 App 取「App 桶 ∪ 全局桶」，App 桶优先填 ASR 直传预算，剩余给全局桶。
- 评分升级为场景条件频率（该 App 内 count 优先）。
- 注意：腾讯/火山词表是会话粒度生效，切 App 需重传——只在会话开始时选桶一次，避免会话中抖动。
- 验收：同库不同 App 下 ASR `hotwords` 参数不同（单测钉死选桶函数）；跨 App 不丢全局词。

### P1-3 中文热词自动挖掘（G5）
- 复用 Windows `HotwordExtractor`（LLMChatClient）思路：**用户最终放行文本**本地攒批（如每 20 条或每日），批量走同一提炼 prompt，只产候选，仍走阈值+确认入表。
- 隐私红线：文本仅在本机攒批，走用户已配置的 LLM 端点前须在设置里默认关闭、显式开启（与现有 LLM 精修开关同级）。
- 验收：提炼结果进候选池而非直接入表；关闭开关时零网络请求。

### P1-4 热词负反馈防线（G8，随 G1 落地）
- 三道防线对齐 WeType：①候选确认入表（已有）；②黑名单 `dismissed/blacklist`（已有 dismissed，补"因纠错统计自动建议移除"）；③容量清洗（上限阈值触发按评分淘汰，日志留痕）。

## 5. 模型/技术选型结论（避免无效探索）

| 项 | 结论 | 理由 |
|---|---|---|
| WeType `.bin` 模型（nlrgpt2/decgnr/ckvd 等） | **不可复用** | 私有二进制格式未公开、权属腾讯；逆向提取不可行也不合规 |
| 本地 NNLM 候选重排（WeType 核心套路） | **不适用本产品** | 前提是 ASR 返回 n-best 候选流；火山/腾讯流式 ASR 只返回单条最优路径。本产品的"重排"= LLM 精修，个性化注入点在 prompt（§2） |
| 本地 VAD | **可平替**（silero-vad ~2MB MIT 或 webrtc-vad） | 用途：`click_to_talk` 自动断句、`hold_to_talk` 尾静音提前收尾（减少拖尾错字，参考 `audio-trailing-syllable-drain.md` 关联问题）；桌面端跑，不动固件 |
| 学习排序（LightGBM 类） | 暂不需要 | 现有手工评分公式即等价思路；信号维度扩充后再评估 |
| RNNoise | 已有 | 无需动 |

## 6. P2 项

- **P2-1 存储资产化（G6）**：三处存储收敛为单库 SQLite（macOS 系统 SQLite / Windows 现有依赖），表：`hotwords/usage/corrections/events`；WAL 解决多写者；提供导出/导入（JSON 快照）作为跨设备资产格式。若将来做云同步，参考 WeType 的增量思路（`user_dict_syncer` full/inc + patch）并上加密（WeType 用 SQLCipher）。
- **P2-2 bigram 共现（G7）**：热词间搭配概率（A 出现时 B 的条件频率），用于 ASR 词表同传提升整句连贯与 LLM prompt 上下文。依赖 P2-1 的 events 表。
- **P2-3 VAD endpointer**：见 §5。

## 7. 实施顺序与依赖

```
P0-1 纠错对资产化 ──┬─→ P1-1 负信号（共用 events 载体）
P0-2 macOS 挖掘对齐 ┘   P1-4 负反馈防线（依赖 P0-1 的纠错统计）
                         P1-2 App 分域（依赖 usage.app 字段，可先行 JSON 实现）
                         P1-3 中文提炼（依赖 P0-2 的 macOS 候选池）
P2-1 SQLite ──→ P2-2 bigram
```

建议按 P0 → P1 → P2 分批提交，每项独立可回退；跨端改动遵守仓库红线（Windows 源码 `git add -f` 并逐文件验证跟踪；协议/配置字段变更同步 `Doc/Ref/` 与双端）。

## 8. 风险与红线

1. **热词负反馈**：错误热词入表会带偏 ASR 第一遍（Windows 注释已识别该场景）。所有自动挖掘只产候选，入表必须用户确认；纠错统计降权是第二道防线（P1-4）。
2. **隐私**：所有用户数据（热词/纠错对/事件）默认仅本地；任何上传（LLM 提炼、云同步）必须显式开关 + 文档声明。本产品当前卖点之一是本地资产，不可破坏。
3. **prompt 膨胀**：few-shot 对与热词段共用预算，超限先裁 few-shot；小模型注意力稀释问题 Windows 端已实证过（top-50 上限的由来）。
4. **双端漂移**：评分权重、挖掘阈值、prompt 段构造必须双端同源（对齐 `hotword_select.py` 惯例），任何一侧改动同步另一侧 + 更新本文档状态。
5. **ASR 词表容量**：火山双向流式 ~100 tokens / 腾讯 128 词每请求是硬约束，场景分域（P1-2）本质是缓解该约束的手段，不要绕过预算直传。

## 9. 附录：WeType 机制 ↔ 本项目落点速查

| WeType 机制（逆向证据） | 本项目落点 | 状态 |
|---|---|---|
| UserCorrectedTextDict 纠错对 | §2 P0-1 corrections 库 + few-shot | 待做 |
| UserActionHistory 全动作记录 | §4 P1-1 events 埋点 | 待做 |
| hot_word detector/classifier/handler | §3 P0-2 + §4 P1-3 双端候选管道 | macOS 缺 |
| user_block_list + clean_strategy | §4 P1-4 三道防线 | 部分（dismissed 已有） |
| per-app session（bundleID） | §4 P1-2 App 分桶 | 待做 |
| user_bigram_dict | §6 P2-2 | 待做 |
| user_dict_v2 + syncer/patch | §6 P2-1 SQLite + 导出 | 待做 |
| Phase A/B 双阶段精修 + 超时回退 | 已有（精修预算回退） | ✅ 无需动 |
| 本地 VAD | §5 silero-vad 平替 | 待做 |
| NNLM 重排 | 不适用（无 n-best），等价物 = 精修 prompt 个性化 | — |
