# 热词评测系统与高频热词优先策略设计

## 背景与目标

用户热词库快速增长（划词加词、候选挖掘两条通道持续入词），两个 ASR 平台的热词上传容量都有限。需要：

1. 摸清腾讯 / 火山两家热词接口的真实能力与限额。
2. 构建可重复的评测系统，量化「大热词库前提下如何提高识别精度」，横向对比两平台热词效果。
3. 热词数量超限时，用优先级裁剪策略尽可能保住高频热词的识别精度。

## 一、平台热词接口调研结论

### 腾讯云（实时语音识别 WebSocket `asr/v2`）

| 机制 | 传参 | 限额 | 说明 |
|---|---|---|---|
| 临时热词表 | 签名参数 `hotword_list` | **每次请求 128 词**，格式 `词\|权重`，权重 1–11，单词 ≤10 汉字 / 30 英文字符 | 随会话生效，动态调整的唯一手段 |
| 热词表 | 签名参数 `hotword_id` | **每账号 30 张表 × 每表 1000 词**，词长限制同上 | 控制台或 API（CreateAsrVocab）创建；桌面端已实现 `SyncHotwords` 同步 |
| 自学习模型 | `engine_model_type` 绑定 | 面向更大规模定制 | 超出热词表限额时的官方路径，训练成本高 |

来源：[热词说明](https://cloud.tencent.com/document/product/1093/40996)、[创建热词表](https://cloud.tencent.com/document/product/1093/41111)、[实时语音识别 WebSocket](https://cloud.tencent.com/document/product/1093/48982)。注：腾讯的临时热词带**显式权重**，这是火山没有的调节手段。

### 火山引擎（豆包大模型流式 `bigmodel_async`）

| 机制 | 传参 | 双向流式容量 | 非流式/二遍容量 | 优先级 |
|---|---|---|---|---|
| Context 直传 | `corpus.context` 内 `hotwords` | **100 tokens**（从前往后截断） | 5000 词 | 高（与表并用时先拼接，超 5000 截断） |
| 热词表 | `corpus.boosting_table_id` | **100 tokens** | 5000 词（单词 ≤10 汉字 / 30 字母）；每应用 500 张表 | 低 |
| 替换词表 | `corpus.correct_table_id` | — | 命中即强替 | — |
| 对话上下文 | `corpus.context` 内 `dialog_ctx` | N/A | 800 tokens / 20 轮 | — |

来源：[热词与上下文](https://www.volcengine.com/docs/6561/2604976)、[热词](https://www.volcengine.com/docs/6561/155739)。关键提示：

- 官方建议双向流式场景控制在 **30–50 个短词**以内（与桌面端 `kHotwordCorpusTokenBudget = 80` 一致）。
- 官方称「热词+上下文在二遍链路效果更优」，但本仓库 2026-08-01 实测（`volc-hotword-ablation-2026-08-01.md`）发现 tech_02 用例二遍最终文本对直传热词和真实 `boosting_table_id` 均免疫。**矛盾点必须用评测系统在大样本上裁决**，不能只看单个用例。
- 官方推荐技巧可纳入策略层：生僻词附解释（「逐玉，追逐的逐，玉石的玉」）、场景描述、对话历史。

## 二、评测系统设计

复用现有 `scripts/e2e_test/asr_bench/` 基建（纯 stdlib 协议实现、真实凭据、不伪造结果），新增热词专项评测。

### 2.1 热词语料库

新增 `corpus/hotword_corpus.json`（与现有 `corpus.json` 分离，热词语料会持续膨胀）：

```json
{
  "id": "hw_01",
  "text": "把 Opus 音频流封装成 Ogg 格式再发送。",
  "hotwords": ["Opus", "Ogg"],
  "category": "tech",
  "duration_s": 3.2
}
```

- 每条语料标注 `hotwords`（该句中期望正确识别的目标热词），这是命中率指标的 ground truth。
- 选词原则：优先收录用户真实热词库中历史上被识别错的词（从 `hotword_candidates.json` 和精修 diff 挖掘记录取材），而非人造生僻词。
- 混入**对照语料**：不含任何热词的句子（复用现有 corpus 即可），用于度量热词库的**误触发率**（热词污染：不该出现热词的地方被错误匹配）。
- 音频生成复用 `gen_corpus.py` 的 edge-tts + ffmpeg 链路，输出 ogg/pcm。

### 2.2 指标

- **热词命中率**（核心）：识别文本归一化后包含目标热词的比例，按词 × 轮次统计（`hit / (clips × rounds)`）。归一化复用 `metrics.py` 的 CER 归一化管线，另加大小写折叠（Opus/opus、OG/Ogg）。
- **CER**：整句字错误率，观察热词对整体精度的副作用。
- **误触发率**：对照语料中输出包含任一库内热词的比例。
- 沿用首 partial / 尾延迟统计，确认热词配置对延迟无回归。

### 2.3 配置矩阵（`run_hotword_bench.py`）

| 组别 | 火山 | 腾讯 |
|---|---|---|
| A 基线 | 无 corpus | 无热词参数 |
| B 小库直传 | context 直传 top-30（80 tokens 内） | `hotword_list` top-30 带权重 |
| C 限额直传 | context 直传塞满 100 tokens | `hotword_list` 塞满 128 词 |
| D 超库裁剪 | 词库 500 词按频率裁到预算（策略组） | 同左 |
| E 热词表 | `boosting_table_id`（5000 词容量） | `hotword_id`（1000 词表） |
| F 二遍消融 | 关 `enable_nonstream` 对照 | N/A |
| G 替换词 | `correct_table_id` 对照（可选） | N/A |

D 组即「高频优先策略」的量化验证：同一词库、同一预算，只改变选词策略（频率优先 vs 随机/字母序），命中率差值就是策略收益。

### 2.4 产出

`bench_results/hotword_*.json|.md`：每平台每配置的热词命中率、CER、误触发率、逐词明细表。结论写入 `Doc/Expe/`。

## 三、高频热词优先策略（受限时的处理方式）

### 3.1 排序模型

每个热词维护三元组：`(count, last_used_ts, source)`（现有候选挖掘已有计数存储 `hotword_candidates.json`，用户手动加词记 `source=manual`）。

```text
score = w_count × log(1 + count) + w_recency × exp(-Δt / τ) + w_manual × is_manual
```

- 默认 `w_count=1.0, w_recency=0.5, τ=30 天, w_manual=2.0`：手动加的词基本必胜，高频常用词次之，长尾旧词淘汰。
- log 压缩计数防止单个超高频词永远霸榜；recency 衰减让新词有机会。

### 3.2 预算裁剪（`asr_bench/hotword_select.py`，纯 stdlib）

按 score 降序装入，直到触及平台预算：

- 火山流式：token 估算（复用桌面端 `FitHotwordsToCorpusBudget` 的规则：中文 1 字 ≈ 1.5 token，ASCII 词 ≈ len/4 + 1），预算 80 tokens（留 20 安全余量）。
- 腾讯临时表：128 词硬上限 + 每词长度校验（≤10 汉字/30 字母），超限词直接降级到表通道。
- 腾讯/火山表通道：1000/5000 词，词长超标的过滤并记录。

### 3.3 分层通道

词库按频率分三层，各走最优通道：

```text
高频层（top-N，装得进会话预算）→ 会话级直传（火山 context / 腾讯 hotword_list）
中频层（1000/5000 词内）       → 平台热词表（hotword_id / boosting_table_id）
长尾层                          → LLM 精修 prompt（唯一不限量的兜底，已上线）
```

火山侧注意：官方明示直传+表拼接超 5000 词从前往后截断，两层并用时总词数要受控。

### 3.4 评测验证点

- D 组（频率裁剪）命中率应 ≥ C 组（无脑塞满）——验证「宁缺毋滥」官方建议与「超库必裁」需求。
- E 组验证表通道对二遍最终文本是否有效（裁决调研矛盾点）。
- 误触发率随库大小的变化曲线，给出实用词库规模建议。

## 四、开发项分解

1. `asr_bench/tencent.py`：`build_signed_url` / `run_clip` 增加 `hotword_id`、`hotword_list` 参数（参与签名，签名字典按键排序已有）。
2. `asr_bench/volcengine.py`：`session_payload` / `run_clip` 增加 `hotwords`、`boosting_table_id`、`correct_table_id` 参数（`request.corpus` 字段）。
3. `asr_bench/hotword_select.py`：评分 + 预算裁剪（§3.1/3.2），单元自测可离线跑。
4. `asr_bench/metrics.py`：加热词命中率/误触发率计算（不动现有 CER 逻辑）。
5. `gen_hotword_corpus.py`：从热词库造句 → edge-tts 合成音频（复用 `gen_corpus.py` 函数）。
6. `run_hotword_bench.py`：配置矩阵驱动、结果落盘、报告生成（复用 `report.py` 模式）。
7. 跑实验 → `Doc/Expe/hotword-bench-<date>.md` 报告，更新 `Doc/Ref/volcengine-asr.md` 与 AGENTS.md 结论。
8. （桌面端接入）C++ `hotword_select` 移植 + `asr_hotwords` 自动裁剪：**已实现**（2026-08-11，`desktop/windows/src/hotword_selector.cc`，评分模型与 Python 侧一致；使用统计存 `hotword_usage.json`，ASR 直传与精修/翻译 prompt 热词段均按评分裁剪）；腾讯 `hotword_list` 权重按频率映射：未做（表通道现状已覆盖，见 `Doc/Expe/hotword-bench-2026-08-01.md` 结论 3/5）。

## 五、约束与风险

- 坚持「不伪造结果」：无凭据/无音频时 SKIP 或报错；腾讯表 API 需要真实凭据创建表，实验前先确认配额。
- 评测成本：矩阵 7 组 × 2 平台 × 语料条数 × 轮次，用 `--only` 小跑冒烟再全量。
- edge-tts 合成语音与真人分布有差距，命中率绝对值仅供横向对比（沿用基线报告口径）。
- 腾讯 `hotword_list` 权重 1–11 的实际区分度未知，作为矩阵的附加观察项。
