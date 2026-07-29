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

## diff 挖掘的盲区（同日追加，"Stack Chain" 实测）

用户测试说「Stack Chain」多次不见托盘通知。回放调试录音（session-39）实证：火山二遍
final 已正确输出「Stack Chain」（流式 partial 是 "Stack chain"，二遍自动改大写）。
即 ASR 本来就识别对 → 精修无 diff → `MineRefinementCandidates` 返回空 → 不计数。
**纠错对挖掘只覆盖三种场景之一**：

| 场景 | diff 挖掘 |
|---|---|
| ASR 识别错 + LLM 认识该词能纠回（deepseek→DeepSeek） | 能挖到 |
| ASR 识别错 + LLM 不认识的全新词 | 挖不到 |
| ASR 本来就识别对（Stack Chain） | 挖不到 |

同期 `cloud.md:1` 那条计数恰好是假阳性通道实例：LLM 某次把 CLAUDE.md 错修成
cloud.md 被挖了一次——再次印证必须人工确认入表。

**修复**：新增 `hotword_mining_enabled`（默认关闭）+ `ExtractHotwordCandidates`
异步提炼通道——会话粘贴完成后后台多调一次 LLM，从最终文本提炼「可能是专有名词且
不在热词表」的候选（JSON 数组输出），解析侧防臆造（候选必须在原文实际出现，忽略
大小写）、限长 2..40 字符、至多 3 词、热词去重。两通道共用
`RecordAndNotifyHotwordCandidates`（已加互斥锁，两个后台回调都会碰存储）。
教训：**「从纠错 diff 中学习」预设了纠错一定发生；对识别本来正确的文本，diff 恒空，
统计信号必须另开通道**。

## 修复：LLM 提炼链路 candidates=0（2026-07-28 第二轮，已修）

**根因（离线探测实证，`scripts/probe_hotword_extraction.py`）**：两个独立问题叠加——

1. **防臆造过滤误杀（系统性）**：ASR 英文空格形态不稳定（`Stack Chain` /
   `Stack  Chain` 双空格 / `StackChain` 连写），LLM 输出时把拼写规范化成
   `Stack Chain`，而 `ContainsCaseInsensitive` 要求精确子串 → 候选被
   `not_in_text` 拒掉。探测复现：双空格源文本 3/3 全部误杀。
2. **模型 temp 0 不稳定（偶发）**：同一输入 `stack chain 是什么` 两次调用分别
   返回 `["stack chain"]` 和 `[]`——DeepSeek temp 0 并非完全确定。

**修复内容**（desktop/windows）：

- `AppearsInSourceText`：精确子串之外，对全去空白形式再比一次（忽略大小写），
  容忍 ASR 空格噪声。ASR 文本不记日志的隐私设计不变，统计只计数。
- prompt 加固：加「非句首首字母大写词组即使像普通词组合也应输出」规则 +
  正反两个 few-shot 示例。实测让大写词组提取稳定 3/3、纯中文/普通对话稳定
  `[]`；小写常见词形态（`stack chain`）变为稳定不提取——可接受，火山二遍
  final 会自动改正大小写（`Stack chain`→`Stack Chain` 实证过）。
- `ParseHotwordExtractionResponse` 增加 `HotwordExtractionStats` 回填
  （bracket/json/items/各拒绝原因计数），`ExtractHotwordCandidates` 增加 log
  回调，协调器落 `hotword extraction detail:` 日志行——今后 candidates=0
  可直接从日志区分模型判断 vs 解析 vs 过滤。
- `ChatSync` 非流式 receive 超时 10s→30s（llm_chat_client.cc）：DeepSeek TTFT
  偶发超 10s 会被误杀为 ok=0。

**三个旧谜团的最终结论**：

- 「7 次 paste_complete 无提炼日志」= 侧键双击「恢复上一次输入」
  （`RestoreLastInputConfirmation`）与 finalizing watchdog 回退直达粘贴、
  不过 TransformText。预期行为（文本首次粘贴时已挖过，重复挖会虚增计数）。
- 「started 后无 finished」= ChatSync 超时当时已存在，实为应用退出致
  alive=false，回调静默丢弃。非 bug。
- diff 挖掘通道在 `refine_enabled=false` 时天然不触发——`MineRefinementCandidates`
  只在精修完成回调里调用。用户当前 refine 关闭，LLM 提炼是唯一活跃通道。

**教训**：防臆造校验的「原文出现」判据必须按 ASR 输出特性设计——ASR 的空白
本身是不可靠信号，严格子串匹配会把正确的候选当臆造杀掉。

## 真机验证与第三轮修复：模型空响应非确定性（2026-07-29 上午）

带 `raw=` 日志的版本真机实测 8 次会话（`hotword extraction detail` 行）：

- `raw=["Stack Chain"]` → kept=1；`raw=["DeepSeek", "Stack Chain"]` → Stack Chain
  kept（DeepSeek 原文未出现，模型从热词表上下文臆带，防臆造正确拦截）——
  **链路修通，计数到 2/3**。
- 两次 `raw=["Stack Chain", "DeepSeek"]` 全被拒 not_in_text：当次 ASR 把词识别成
  了别的字形，模型按「纠正」脑补出原文不存在的词——过滤按设计工作，非 bug。
- **新暴露的问题**：用户确认两次粘贴文本就是大写「Stack Chain」（与离线探测
  6/6 提取成功的输入等价），线上却连续 2 次 `raw=[]`。离线同 prompt 同输入
  复现不出来——DeepSeek temp 0 的非确定性按时间段成群出现（重放同 ogg 3/3
  稳定大写，排除 ASR 侧变量）。

**第三轮修复**：`ExtractHotwordCandidates` 拆出 `...Attempt`，模型成功但 0 候选
且文本含大写字母（目标候选典型形态）时**自动重试一次**；网络错误、解析出候选、
纯中文文本均不重试（避免每次会话白调 LLM）。detail 日志加 `attempt=N`。

**观测设计回顾**（本轮方法论，可复用）：candidates=0 这类「链路末端无输出」问题，
按「模型返回了什么 → 解析过了吗 → 被哪条过滤拒了」三段埋点（`raw=` + stats
计数），一轮真机日志即可定位到段；`raw=` 只含模型候选词（与托盘通知同级，不含
用户原文），与「ASR 文本不记日志」的隐私设计兼容。

## 收尾：计数闭环、通知被系统吞、断连丢文字（2026-07-29 中午）

**Stack Chain 计数到 3/3，链路全通**。指纹日志（`started: text_len=… model=…
hotwords=…`）证实线上请求与离线探测输入完全一致（text_len 与回放文本字节数
精确吻合），4 连 `[]` 最终判定为 **DeepSeek 服务端分时段非确定**——同一时段内
attempt 1/2 结果一致，跨时段才翻转；靠重试+计数累加兜底，无需再追。

**托盘气球通知不可靠（实测）**：计数满 3 后 `ShowNotification` 确实触发
（`notified` 有记录），但用户 Win+N 通知中心无任何记录——Win32 托盘气球
（`Shell_NotifyIconW NIF_INFO`）会被专注助手/通知设置静默拦截且无返回值可查。
修复：`VoiceStickUi` 新增 `ShowTimedMessage(message, duration_ms)`，候选建议
同时显示到悬浮窗 3 秒（必现）；实现侧（Win32App）负责 UI 线程封送 +
`HasActiveSession()` 守卫（会话活跃时回退托盘气泡，不踩状态机浮窗——与
热词处理 `ProcessHotwordWithLlm` 同一模式）。

**断连丢弃在途 ASR final（"流式有字但没粘贴"）**：录音中 BLE 音频丢包 → 停滞
超时送 final 音频块 → nostream final 在途仅 450ms 时检测到设备重新广播（僵尸
链路），`CancelActiveCycleIfDeviceDisconnected` 直接 `asr_->Cancel()` 杀掉了
与 BLE 无关的网络侧 final。修复：final 音频块发出后（
`sent_final_audio_chunk_ && kFinalizing`）断连不再取消 ASR，final 到达照常粘贴；
final 不到则由既有 finalizing watchdog 回退粘贴原文或报错。测试
`TestCoordinatorDisconnectAwaitingAsrFinalKeepsSession`（断连后 on_final 照常
粘贴）+ `TestCoordinatorDisconnectDuringRecordingCancelsSession`（录音中断连
仍取消的对照）。

**教训**：「延迟越来越大、一度没反应」这类主诉，先用日志把单次会话的时间轴
拉出来（本次是 10s 音频停滞 + 2s END 超时 + 僵尸链路拆除 ≈ 33s），往往能
定位到一两个具体的等待点，而不是「整体变慢」。
