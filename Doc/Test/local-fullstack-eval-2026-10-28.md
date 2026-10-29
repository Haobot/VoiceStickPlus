# 本地全栈小模型评测报告（ASR + 精修 + 语音改字可行性）

- 日期：2026-10-28
- 分支：`feat/local-model-fullstack`
- 环境：Apple M4 Pro 24GB / macOS 27 / ollama 0.40.3 / whisper.cpp 1.9.5（Metal on）/ sherpa-onnx 2.0.1
- 数据：`scripts/e2e_test/corpus/` 31 条（真实设备链路音频，16kHz PCM + 标注），CER 口径与
  云端基线（`bench_results/asr_bench_merged_20260801-025118.md`）完全一致
- 复现：`scripts/local_model_eval/README.md`；原始数据 `scripts/local_model_eval/results/`

## 0. 一句话结论

**可行，但不能照搬云端的「整句重写」形态。** 本地最优组合
SenseVoice int8 + 确定性清理 + 受限编辑指令（editform）× qwen2.5:3b：
管线 CER **1.67%**（本地 ASR 单层 2.22%，腾讯云基线 0.89%），改善 3 条 / 损伤 0 条，
按键释放后纯模型链路延迟 ≈ **110ms**（40+70），全程零网络。

## 1. 评估问题

把「云端模型处理文本」全面替换为本地小模型，涉及三层，逐层量化：

| 层 | 现状 | 本轮要回答的问题 |
|---|---|---|
| ASR | 腾讯/火山云端流式（macOS 主链路）；Windows 已有 SenseVoice 本地可选 | 本地 ASR 与云端的准确率/延迟差多少？哪个引擎/尺寸够用？ |
| 文本精修 | 云端 LLM（macOS）；Windows 本地 Qwen3-1.7B + 删除守卫 | 本地小模型精修能否修错而不伤对？什么形态可行？ |
| 语音改字 | 双端确定性解析 + macOS ollama 对齐兜底（已本地） | 缺口在哪儿（前置依赖）？ |

## 2. 方法

- **同口径**：CER 直接 import `scripts/e2e_test/asr_bench/metrics.py`（去标点/空白、
  NFKC、大小写折叠、中文数字转阿拉伯），与 2026-08-01 云端基线逐数字可比。
- **同语料 3 轮**（ASR 层，对齐云端基线的 3 轮口径）；精修层用 SenseVoice 第 1 轮
  确定性输出作输入（错误是真实 ASR 错误，非编造）。
- **prompt 双轨逐字取自生产代码**：云端指令式 = `llm_refinement_client.cc`
  BuildRefinePrompt；本地 few-shot = `local_refinement_client.cc` BuildSystemPrompt；
  热词后缀 = BuildRefinePrompt 热词段。请求体对齐生产（temperature 0、think:false）。
- **安全指标**：`damage`（ΔCER>+0.01 条数）、`clean_dmg`（原文全对被改错）——
  精修的红线不是增益而是不伤对。

## 3. 第一层：本地 ASR 横评（results/asr_local_20261028-111555 / 112933）

| 引擎 | 尺寸 | CER 均值 | CER 最差 | 热延迟 p50/p95 (ms) | 冷墙钟 p50 (ms) |
|---|---|---|---|---|---|
| （基线）腾讯云·流式 | — | 0.0089 | 0.1304 | 尾延迟 125 | — |
| （基线）火山云·流式 | — | 0.0134 | 0.2609 | 尾延迟 969 | — |
| **SenseVoice int8** | 239MB | **0.0222** | 0.2174 | **40 / 136** | ~1000（加载） |
| whisper large-v3-turbo q5_0 | 574MB | 0.0233 | **0.1579** | 552 / 762 | 763 |
| whisper small | 487MB | 0.1603 | 1.0000 | 207 / 551 | 397 |
| whisper tiny | 78MB | 0.3447 | 1.0000 | 59 / 323 | 149 |
| whisper base | 148MB | 0.3749 | 1.0000 | 86 / 280 | 193 |

（whisper 均 `-l zh`、threads=8、Metal；turbo 另测 `-l auto`：CER 相同但延迟翻倍
1081ms——短中文片段 auto 检测多一遍解码，无收益，生产应固定 zh。）

结论：
1. **SenseVoice int8 是本地 ASR 唯一推荐**：CER 均值与 turbo 持平（2.22% vs 2.33%），
   延迟低 14 倍（40ms vs 552ms），模型小（239MB），且就是 Windows 已在分发、
   `local_asr_client_win.cc` 已接的同款（SHA-256 与 `pack_local_models.py` 清单核对一致）。
2. **whisper tiny/base/small 在本产品音频（低码率 Opus 转写链）上崩塌**（CER≥16%，
   worst=100% 出现整条幻觉/空输出），公开榜的干净音频排名不可迁移——再次验证
   「选型结论不可跨任务形态搬用」。
3. turbo 可作为「离线高精度档」备选（worst 更优 15.79%），但 574MB+552ms 只换
   最差值改善，不建议双引擎并存。
4. 与腾讯云的差距 1.33pp 集中在**术语类**（SenseVoice tech 类 10.79% vs 腾讯 4.92%：
   A2→ASR、dek hubb→GitHub、留峰装→封装），全部是确定性同音/近音漂移——
   正是精修层的目标错误类型。
5. 所有本地引擎跨轮抖动为 0（确定性推理），无云端的重试/超时/会话失败面。

## 4. 第二层：本地 LLM 精修（results/refine_20261028-115117 / 115818 / 120800 / 120903）

输入 = SenseVoice 对 31 条的第一轮输出（基线 CER 2.22%，其中 tech 类 10.79%）。

### 4.1 轮次 A：整句重写形态（cloud / cloud+热词 / few-shot）×9 模型 —— 全灭

27 个组合**没有一个跑赢「什么都不做」**。代表性结果：

| 模型 | 最优变体 | 管线 CER | 改善/损伤 | clean 损伤 | p50 |
|---|---|---|---|---|---|
| qwen2.5:7b（4.7GB） | cloud | 5.15% | 2 / 3 | 2 | 364ms |
| qwen3:8b | cloud+hw | 13.87% | 2 / 4 | 3 | 420ms |
| qwen2.5:3b | cloud+hw | 10.11% | 2 / 6 | 6 | 179ms |
| qwen2.5:1.5b | cloud | 19.42% | 0 / 14 | 11 | 101ms |
| gemma3:1b | fewshot | 436.7% | 0 / 22 | 17 | 206ms |
| qwen3:4b | cloud | 3935% | 0 / 31 | 24 | 8043ms |

损伤取证（非计分器假象，逐条人工复核）：
- `帮我打开浏览器。` → `请打开浏览器。`（礼貌性改写，CER 0.286）
- `我的手机号是…` → `**你的**手机号是…`（**代词翻转，语义反向**）
- `用 python 写一个 hello world 程序。` → ```` ```python\nprint("Hello, World!")\n``` ````（助手人格发作，输出代码块）

**结论：整句开放式重写对 ≤8B 本地模型是结构性不可用**——损伤率 20%~100%，
模型越大越轻但 7b 仍净负。云端精修不能平移成本地小模型任务。

（Windows 现役本地精修因此没有暴露此问题：`text_refiner.cc` 的
`RefineResultSafe` 守卫只放行「纯填充词删除」差异，改写一律回退规则层——
安全，但也**永远修不了 ASR 错字**。）

### 4.2 轮次 B→D：受限编辑指令形态（editform）——零损伤且有增益

形态：模型只输出指令（`错词→热词原形` / `删除：片段`），代码守卫执行
（替换目标必须在热词表、错词/片段必须真实存在于原文、≤5 条、大小写不敏感定位）。
两轮迭代：v1 零损伤但小模型答「无」无增益；v2 增加一条术语漂移教学例
（用 SDK/IDE，与语料术语零重叠）后：

| 组合 | 管线 CER | 改善 | 损伤 | clean 损伤 | tech 类 | p50 |
|---|---|---|---|---|---|---|
| 基线（不精修） | 2.22% | — | — | — | 10.79% | — |
| **det v5 + editform × qwen2.5:3b** | **1.67%** | **3** | **0** | **0** | **6.59%** | **70ms** |
| det v5 + editform × qwen2.5:1.5b | 2.08% | 1 | 0 | 0 | 9.7% | 44ms |
| editform × qwen3:8b | 4.87% | 1 | 3 | 1 | 26.6% | 122ms |

- det 层（纯确定性：句首填充词剥离＋3 连字口吃收敛＋CJK 间空格清理）在 31 条上
  零损伤；其 v1 曾误删「这个 API」的「这个」（指示代词≠填充词），v5 修正为
  「指示词仅在后跟逗号时剥」后归零——确定性层的规则也要过真实数据。
- qwen3:8b 在 editform 下仍不稳（把「留峰装」错拆成删除指令），qwen3 家族
  在对齐、整句重写、编辑指令三个任务形态上两次崩塌，全线不推荐。
- **推荐链：qwen2.5:3b-instruct（Q4，1.9GB）为主，qwen2.5:1.5b（986MB）为低配档**；
  延迟 70/44ms，与 targeted 对齐层同模型家族，可共享常驻引擎。

### 4.3 填充词清理子任务（12 例合成集）

| 方案 | CER | 全对 | 风险 |
|---|---|---|---|
| few-shot 整句重写 × 1.5b | 2.08% | 11/12 | 在 31 条真实输入上损伤 14 条（不可用） |
| det v5 确定性 | 4.59% | 8/12 | 零 LLM、零损伤 |
| det v5 + editform | 4.59% | 8/12 | editform 未增加填充词召回 |

口语清理交确定性层（规则可逐步扩），LLM 不为它冒险——「能在确定性层解决的
不给模型」。

## 5. 端到端组合与延迟预算

| 管线 | CER | 出文本延迟（按键释放后，模型链路） |
|---|---|---|
| 腾讯云 ASR + 云端 LLM 精修（现状） | 0.89% + 精修未测 | 尾延迟 125ms + 云精修总耗时 2.5~12.8s（distilled §L265，流式显示缓解体感） |
| **全本地：SenseVoice + det + editform×3b** | **1.67%** | **40 + 0 + 70 ≈ 110ms**，无网络、无流式 partial |

代价：本地无流式 partial（说话中无字幕渐进），按键释放后出全文比云端更快，
但说话过程中的「实时感」缺失——交互上以录音态 UI 补偿（现有 UI 已有录音中状态）。

## 6. 语音改字环节衔接

- 解析/匹配/规划已是确定性双端同构；macOS 对齐兜底已本地（qwen2.5:1.5b 94%/82ms）。
- **缺口 1（安全前置，未落地）**：C 层修复（`isCommandFallback` 标记 + 门控跳过，
  防「分析→升旗」漂移时回退粘贴污染 LastInjection）——代码未实现，仅文档待办。
- **缺口 2**：Windows 对齐同构（复用 llama.cpp 常驻引擎 + 换 Qwen2.5 GGUF）。
- 本地 ASR 下指令轮的触发词错误率会高于腾讯（漂移更多），对齐层的价值上升；
  §17.7 的热词偏置思路（指令轮热词表）仍成立。

## 7. 局限

1. 31 条语料偏小，错误集中于术语/噪声类；真实口音/长会话分布未覆盖
   （DebugAudio 回放扩充是下一步）。
2. editform 增益依赖热词表质量：无热词用户只拿到 det 层（零增益零损伤）。
3. 本轮为 Apple Silicon 数字；Windows x86 上 SenseVoice 已有真实用户
   （`[local_asr] enabled`），延迟量级不同但结论（引擎选型、形态选型）不依赖平台。
4. 云端 LLM 精修在同输入下的修复上限未测（本机无 [llm] 凭据）；
   qwen2.5:7b 充当「更大模型仍救不了重写形态」的锚点。
5. whisper 只测了 GGML q5/官方档；mlx-whisper 运行时未测（同模型家族，
   不改变选型结论，只可能改延迟数字）。

## 8. 轮次迭代史（教训沉淀）

| 轮 | 假设 | 结果 |
|---|---|---|
| A1 | 小 whisper 也能用 | tiny/base/small 崩塌（CER≥16%） |
| A2 | turbo 需要 auto 语言 | auto 与 zh 精度相同、延迟 ×2 |
| B1 | 生产云端 prompt 直接本地跑 | 27 组合全灭（损伤 20%~100%） |
| B2 | few-shot 能救 | 填充词 11/12 但真实输入损伤更重 |
| C1 | editform v1 零损伤 | 安全但小模型全答「无」，零增益 |
| C2 | 加一条术语漂移教学例 | 3b 增益 +3/-0，tech CER 10.79%→6.59% |
| D | det 指示词剥离 | 「这个API」误删 → v5 逗号门槛归零 |

任务形态定律（distilled §7.6）第三次验证，且新增一条：**本地精修的可行形态
= 编辑指令 + 源校验守卫执行**（与 targeted 对齐、跨轮纠错同构——「模型出指令、
守卫代码层执行」house pattern 现在有三个任务的实证）。

## 9. 内嵌 llama.cpp 复测（2026-10-29，D1 验收）

M2 内嵌化（`feat/macos-local-model-settings`，llama.cpp b10868 dlopen 引擎）后按
task-shape 纪律复跑评测口径，与 Ollama 原始结论对拍：

### 精修（det v5 + editform，31 例，与 §5 同输入集）

| 运行时×模型文件 | CER | damage | p50/p95 | 判定 |
|---|---|---|---|---|
| Ollama（评测原始，Metal） | 1.67% | 0 | 70ms | 基线 |
| **内嵌引擎 + Ollama registry blob（1.93GB）** | **1.78%** | **0** | 706ms/1199ms | **✅ 达标**（CPU） |
| 内嵌引擎 + Qwen 官方 ModelScope Q4_K_M（2.10GB） | 2.86% | **1** | 179ms | ❌ damage 越线 |

### 对齐（29 例全集，GBNF 枚举约束）

| 运行时×模型文件 | hit | dangerous | 备注 |
|---|---|---|---|
| Ollama 今日全量复跑（格式约束） | 23/29 | 3 | 与当时 94%/主集口径一致级别 |
| **内嵌引擎 + Ollama blob** | **22/29** | 5 | 与 Ollama 同 blob 仅差 1 例（数值噪声） |
| 内嵌引擎 + Qwen 官方量化 | 20/29 | 5~7 | 明显劣化 |

### 结论（写入清单与发布轨道的依据）

1. **同名量化不同量化器产物行为有实质差异**：Qwen 官方 ModelScope Q4_K_M 与
   Ollama 自家量化在对齐上差 6 个点、在 editform 上 damage 0→1。**LLM 档位
   pin Ollama registry blob（评测原件，SHA=blob 名）**，不使用官方 ModelScope 件。
2. **内嵌引擎忠实**：同 blob 下与 Ollama 差 ≤1 例（Metal vs CPU 的浮点数值在
   边界 logit 上的翻面），KV 前缀复用/每例新建引擎结果逐例全同（复用路径无 bug）。
3. **GBNF 枚举语法**（`llama_sampler_init_grammar`，约束先于贪心入链）是
   Ollama `format` 约束解码的等价物——无约束纯 few-shot 会掉点（对齐 69%）。
4. CPU 延迟：精修 3B p50 706ms（总预算 5s 内 ✓）；对齐 1.5B p50 ~700ms
   （local_align_timeout 3000ms 内 ✓）。Metal 加速留后续里程碑。

结果存档：`results/refine_embedded_20261029.json`、
`refine_embedded_officialquant_20261029.json`、
`scripts/local_align_bench/results/embedded_*.json`。
