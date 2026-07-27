# 火山二遍不吃热词直传的根因排查与热词体系加固（2026-07-28）

排查 AGENTS.md/CLAUDE.md 识别不对的完整过程与结论。涉及：ASR 热词链路、精修 LLM 兜底、
热词候选自动挖掘。相关代码以当前源码为准。

## 根因（回放实证，非猜测）

火山 `enable_nonstream` 二遍模式下，`corpus.context` 热词直传**只在流式第一遍生效**；
最终文本由二遍（nostream）重识别产出，不吃直传热词并覆盖第一遍结果。1.0
（`volc.bigasr.sauc.duration`）与 2.0（`volc.seedasr.sauc.duration`）行为一致，
`dialog_ctx` 上下文同样进不了二遍。

实证方法（关键）：用同一段 DebugAudio 录音（`%APPDATA%\VoiceStick\*.ogg`）跨配置回放，
工具 `scripts/e2e_test/replay_volcengine_asr.py`（可复用协议 + `--legacy` 传统协议，
支持 `--hotwords/--dialog-context/--boosting-table-id/--correct-table-id/--no-nonstream/
--no-ddc/--resource-id/--url`）。实验矩阵：

| 配置 | 最终识别 |
|---|---|
| 无热词 / 3 词 / 26 词 / dialog_ctx（二遍开） | 全部 `agentsdmd 和 clouddmd` ❌ |
| 3 词 + `--no-nonstream`（只第一遍） | `Claude DMD`——直传生效 ✅ |
| nostream 端点（`--legacy --url .../bigmodel_nostream`）+ 3 词 | `claude dmd`——端点上真生效但弱 |
| LLM 精修 prompt 附热词表（DeepSeek） | `AGENTS.md 和 CLAUDE.md` ✅ 完美 |

## 教训：对照实验混淆变量导致假阳性

最初让用户体验式测试：热词表精简到 3 词后「识别正确」，据此误判根因是「双向流式
100 tokens 上限」（还提交了 80-token 预算裁剪 `AsrProtocol::FitHotwordsToCorpusBudget`）。
后来用同一段录音回放才发现：3 词 + 二遍开依然错——**当时的成功是因为用户重新录了一遍、
发音更清晰，不是热词生效**。教训：ASR 对照实验必须固定音频变量（回放同一段），
真人重录会引入发音差异，足以淹没热词效果。裁剪修复本身无害（仍防第一遍超限），保留。

## 落地方案

- **A：精修 LLM 兜底**。`LLMRefinementClient::BuildRefinePrompt` 追加完整热词表
  （默认/自定义 prompt 都追加），LLM 把近音错词纠回热词原形。需 `refine_enabled=true`。
- **A 加固（短期）**：prompt 加 few-shot 纠错示例 +「原文已正确的热词原样保留」约束；
  `RefineResultKeepsHotwords` 本地守卫——原文已出现的热词被精修改丢时整体回退原文
  （`voice_stick_coordinator.cc` TransformText 完成回调）。
- **B：官方词表通道**。配置 `volcengine_boosting_table_id` / `volcengine_correct_table_id`
  → `corpus.boosting_table_id` / `correct_table_id`（`asr_protocol.cc CorpusJson`）。
  需火山控制台自学习平台建表；**对二遍是否生效未验证**，用回放脚本 `--boosting-table-id` 测。
- **热词候选自动挖掘（中期，替代全自动入表）**：`hotword_candidate_miner.cc` 做纠错对
  挖掘——refined 中新出现的「标识符样式」词（字母开头、含大写/数字/._-，排除普通英文
  单词与版本号）计入 `%APPDATA%\VoiceStick\hotword_candidates.json`，满 3 次
  （`kHotwordCandidateThreshold`）弹托盘通知，设置-热词区候选列表由用户「加入/忽略」。

## 为什么不全自动积累热词（需求决策记录）

用户曾提议「精修时 LLM 提炼热词自动入表」，评估后否决，改候选确认制，原因：

1. **负反馈污染**：LLM 可能把错误变体（clouddmd）当热词入表，ASR 第一遍反被错误热词
   带偏，越用越差；无人确认 = 模型自己批改自己作业。
2. **额度矛盾**：直传第一遍 ~100 tokens 预算，表无限膨胀加剧裁剪 churn；精修 prompt
   热词段也越来越长，稀释小模型注意力，精修更不稳。
3. **成本延迟**：每次输入多一次 LLM 调用；合并进精修让输出格式变脆。
4. **不可审查**：热词表影响识别正确性，全自动写入出问题难排查。

## 精修不稳定的三来源（备忘）

1. 二遍 ASR final 每次不同（agentsdmd/agents dmd/Agents dmd），错法难度不同；
2. temp 0 ≠ 完全确定（服务端 batching/浮点非确定）；
3. 25 个混杂热词稀释小模型（deepseek-v4-flash）注意力。few-shot 示例是最有效的加固。

## 其他实证事实

- `bigmodel_nostream` 端点不接受可复用连接事件协议（报 45000151），需传统
  full client request 协议；热词直传额度 nostream 5000 词 vs 双向流式 100 tokens。
- 不建议切纯 nostream：丢实时 partial 上屏（核心交互），且热词仍救不全（agents dmd
  未被纠回），工程改动大。二遍 = 流式快 + nostream 准的组合已是最优。
- macOS Swift 端三项 parity 未做：热词裁剪、精修热词注入+守卫、词表 ID 与候选挖掘。
