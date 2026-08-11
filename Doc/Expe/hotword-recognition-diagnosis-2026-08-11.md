# 热词识别率诊断与自动化验收工具（2026-08-11）

记录「热词识别率低」从分析到修复到建立自动化验收的全过程：根因链、A+B 修复、
`run_hotword_acceptance.py` 工具与首轮真实验收结论、以及过程中的经验教训。
相关代码以当前源码为准。

## 成果

1. **根因链（按影响排序，均有评测/代码实证）**
   - 火山 `enable_nonstream` 二遍重识别覆盖流式第一遍热词直传：英文/混排词
     （CLAUDE.md 类）恒被覆盖，中文词（覃海洋/逐玉）可生效；`boosting_table_id`
     对二遍同样无效。唯一兜底 = LLM 精修（`Doc/Expe/hotword-bench-2026-08-01.md`
     + `volc-hotword-ablation-2026-08-01.md`）。
   - `FitHotwordsToCorpusBudget` 按插入顺序贪心截断，新加词 append 尾部最先被裁。
   - 精修/翻译 prompt 附加热词表无上限，大库稀释小模型注意力。
   - 腾讯桌面端只用词表通道（`hotword_id`）且每运行只同步一次；含空格/`.` 的词
     （CLAUDE.md、Claude Code）进不了腾讯任何热词通道。
   - macOS 热词零防护（不裁剪、无精修注入、无词表）。
   - `refine_enabled` / `hotword_process_enabled` / `hotword_mining_enabled`
     默认全关——唯一对英文词有效的兜底默认不启用。

2. **A+B 修复（提交 `2565634`，Windows）**
   - 新增 `desktop/windows/src/hotword_selector.cc`：评分模型与
     `asr_bench/hotword_select.py` 完全一致（`1.0*log1p(count) + 0.5*exp(-age/30d)
     + 2.0*manual`），库外词按 manual（修复新词最先被裁）；使用统计存
     `%APPDATA%\VoiceStick\hotword_usage.json`（最终文本大小写不敏感匹配，
     只计数不记录文本）。
   - 协调器 ASR 直传前评分排序装入 80 tokens；超裁每次运行提示一次（浮窗/托盘）。
   - 精修/翻译 prompt 热词段评分 top-50（`kHotwordPromptMaxWords`）。

3. **自动化验收工具 `scripts/e2e_test/run_hotword_acceptance.py`**
   - 桌面端同款发送回归：腾讯 = 字符过滤后 `sync_vocab` 词表通道；火山 =
     评分+80 tokens 装入 `corpus.context`。
   - 音频：自动生成（edge-tts 逐词造句，`corpus/acceptance_<stamp>/` 不入库）
     或 `--manifest` 标注清单指向已有录音（含真实调试 ogg）。
   - 输出 `bench_results/acceptance_<stamp>.json|.md`：逐组命中率、逐词
     hit/total、未命中「参考 vs 识别原文」示例、baseline vs desktop 增益、
     不可入表词诊断；`--report-only` 断点续报。

## 首轮真实验收结论（用户热词 DonkeyCar / Claude Code，双平台 × 2 轮）

| 词 | 腾讯 baseline | 腾讯 desktop | 火山 baseline | 火山 desktop |
|---|---|---|---|---|
| DonkeyCar | 6/6 | 6/6 | 2/6 | 4/6 |
| Claude Code | 6/6 | 6/6 | 0/6 | 0/6 |

- **用户当前用腾讯 + 这两个词：基线就 100% 识别对**，热词通道无增益空间。
  「热词没识别到」若是腾讯场景，问题不在热词通道，更可能来自真实语音链路
  （BLE 丢帧/口音/语速）或测试的是别的词——用验收工具 + 真实词重跑验证。
- `Claude Code` 含空格进不了腾讯词表（工具自动诊断「不可入表，需 LLM 精修」），
  火山侧同样被评分模型过滤且被二遍覆盖（Cloud Code）——唯一兜底是开启
  `refine_enabled=true`（精修 prompt 附加热词表，07-28 已实证 AGENTS.md 场景）。
- 火山 DonkeyCar 热词真实有效（33%→67%），佐证热词通道本身可用。

## 经验教训

1. **「热词没识别到」先查运行时事实，再谈修复**：provider、热词构成、
   refine 开关是三个必查项。本次用户热词仅 2 个且腾讯基线满分——此前 A+B 的
   评分裁剪对用户当前库完全无效果（<80 tokens 不裁剪），「用户感知没改善」
   不代表改动无效，只代表没打中痛点。
2. **评测报告里的实现状态断言必须回代码核对**：`hotword-bench-2026-08-01.md`
   结论 5 称「C++ EffectiveHotwords 策略同向/暂无移植紧迫性」，实际代码只有
   贪心截断、无评分——报告写「同向」不等于代码真有。教训：bench 验证的是
   **策略**，实现状态要另查源码，别引用报告断言代替代码事实。
3. **验收 vs 评测要分开**：bench（`run_hotword_bench.py`）是策略矩阵对比（每组
   换发送策略），acceptance 是「当前配置/热词到底能不能识别到」的桌面同款回归
   （基线 + desktop 两组即可）。热词改动后跑 acceptance，不要每次烧全矩阵。
4. **工具构建教训（report-only 断点续报的价值）**：
   - 部分结果 JSON 里的字段可能是**循环中临时值**（`unsendable` 初始为空、
     最终值在循环后计算），续报逻辑必须重算而非信任旧字段。
   - `detail` 是 `to_dict()` 字典不是 ClipResult 对象，复用指标函数要兼容
     dict/对象两种形态。
   - 48 会话真实 ASR 跑一次约 6 分钟，把「采集」与「报告生成」分离 +
     `--report-only`，修报告 bug 不用重跑采集。
5. **edge-tts 合成语音与真人分布有差距**（沿用基线口径）：验收的命中率绝对值
   仅供横向对比与回归；真实场景用 `--manifest` 指向真实调试录音最可靠。

## 复现命令

```sh
cd scripts/e2e_test
# 自动生成音频 + 双平台验收（默认读 config.toml 热词与 provider）
python run_hotword_acceptance.py --provider all --rounds 2
# 用已有录音（标注清单含 text/hotwords/audio 路径）
python run_hotword_acceptance.py --manifest my_clips.json
# 断点续报（不重复烧 ASR）
python run_hotword_acceptance.py --report-only bench_results/acceptance_<stamp>_partial.json \
    --manifest corpus/acceptance_<stamp>/manifest.json
```
