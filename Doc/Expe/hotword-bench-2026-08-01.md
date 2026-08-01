# 热词专项评测：大热词库下腾讯/火山 ASR 热词效果对比（2026-08-01）

设计见 `Doc/Plan/hotword-eval-and-prioritization.md`。工具：
`scripts/e2e_test/run_hotword_bench.py`（配置矩阵驱动）+ `asr_bench/hotword_select.py`
（频率评分与预算裁剪）+ `asr_bench/tencent_vocab.py`（腾讯词表 TC3 API）。
真实凭据真实服务，不伪造结果。

## 评测设置

- **热词语料**：`corpus/hotword_corpus.json` 20 条（edge-tts 合成），每条标注目标热词，
  共 16 个目标热词：Opus×2、Ogg、ESP32、BLE×2、OTA、ESP32-S3、WebSocket×2、
  VoiceStick、CLAUDE.md、AGENTS.md、ESP-IDF、WinSparkle、VB-CABLE、覃海洋×2、
  蜜制、疯四×2、逐玉（选词覆盖：英文术语、中英混合、生僻姓、同音词、网络用语）。
- **对照语料**：`corpus/corpus.json` 31 条（无目标热词），度量误触发率。
- **热词库**：目标热词 + 填充词凑到 500 词（模拟用户真实增长后的库规模）；
  目标词构造为高频（count 20–100），填充词长尾（count 0–3），seed 固定。
- **轮次**：1 轮。

## 配置矩阵

| 组 | 火山 | 腾讯 |
|---|---|---|
| baseline | 无 corpus | 无热词参数 |
| direct_small | context 直传 16 个目标词（理想小库） | hotword_list 目标词 weight=10 |
| direct_full | 500 词按字典序塞满 80 tokens（无脑塞满对照） | 500 词字典序前 128 个 weight=5 |
| tiered | hotword_select 频率分层 → direct 层 | 频率分层 → 临时表 128 词 |
| table | SKIP（config.toml 无 boosting_table_id，控制台创建路径） | hotword_id=评测自动同步的 500 词表（--create-tables） |
| no_nonstream | 关二遍 + 目标词直传（验证热词只对第一遍生效） | N/A |

## 平台热词接口事实（调研结论）

- 腾讯：临时热词表 `hotword_list` 每请求 ≤128 词（`词|权重`，权重 1–11 或 100=同音强替）；
  词表 `hotword_id` 30 表 × 1000 词；两者同传仅临时表生效。
  **实测新发现：词表 API 与临时表均不接受含 `.` 等 ASCII 标点的词**
  （CreateAsrVocab 报 `InvalidParameterValue.InvalidWordWeight`，整请求被拒），
  CLAUDE.md / AGENTS.md 这类词在腾讯侧无法入表，只能走 LLM 精修兜底。
  桌面端 `SyncHotwords` 已补过滤（`IsValidHotwordChars`，含单测）。
- 火山：context 直传与热词表双向流式均 100 tokens（前向截断），二遍/非流式 5000 词；
  直传优先级高于表。官方称热词对二遍效果更优——与本仓库此前单用例实测
  （表对二遍无效）矛盾，本次 matrix 的 table/no_nonstream 组用于裁决。
  火山表通道需控制台创建（热词管理 API 文档需 JS 渲染，未程序化）。

## 结果

数据：`bench_results/hotword_hotword-full.json`（51 条/组 = 20 热词 + 31 对照，1 轮，全部成功无失败 run）。
命中率为 22 个目标热词实例的精确命中比例（大小写不敏感子串匹配）。

| 组 | 火山命中率 | 腾讯命中率 | 火山 CER | 腾讯 CER |
|---|---|---|---|---|
| baseline | 16/22 (72.7%) | 16/22 (72.7%) | 0.0226 | 0.0173 |
| direct_small | 19/22 (86.4%) | 21/22 (95.5%) | 0.0155 | 0.0097 |
| direct_full（字典序塞满） | 16/22 (72.7%) | 18/22 (81.8%) | 0.0226 | 0.0120 |
| tiered（频率分层） | 19/22 (86.4%) | 21/22 (95.5%) | 0.0169 | 0.0097 |
| table | SKIP | 21/22 (95.5%) | — | 0.0097 |
| no_nonstream | 15/22 (68.2%) | N/A | 0.0198 | — |

注：腾讯 direct_small 实际发送 15 词——CLAUDE.md / AGENTS.md 含 `.` 被词表/临时表接口
拒绝，客户端侧过滤后不上传（这两个词腾讯各配置均靠基线能力命中 1/1，未受影响）。
火山 direct_small 17 词、direct_full 31 词（80 token 预算字典序截断）、tiered 30 词。

### 逐词要点

- 两平台基线共同漏识：Ogg（0/1，识别为 AUX/awg/OG）、覃海洋（火山 0/2）、蜜制、逐玉。
- 热词纠正效果：覃海洋（火山 0/2→2/2，腾讯 1/2→2/2）、逐玉（0/1→1/1）、蜜制（腾讯 0/1→1/1）、
  BLE / VoiceStick（腾讯各 +1）。
- **CLAUDE.md（火山）**：所有二遍开启的配置恒 0/1，仅 no_nonstream（关二遍）命中 1/1——
  精确复现 2026-07-28 单用例结论「第一遍热词提升被二遍覆盖」（英文混排词）。
- **中文热词（火山）**：覃海洋/逐玉在二遍开启下经直传获得纠正（0→2/2、0→1/1）——
  07-28「二遍完全无视热词」的结论需修正为「二遍对中文热词可生效，对英文混排词仍会覆盖」。
- **Ogg**：两平台全部配置 0/1，热词通道无法纠正。疑似 edge-tts 合成音频对该词发音异常
  （语料局限），需人工录音复核后才能判定是否真·声学盲区。
- **误触发率**：朴素子串度量各组恒为 3/31（9.7%），但逐条人工核对，3 条全部是对照语料
  参考文本本身合法包含库词（tech_01 WebSocket、tech_02 Ogg、tech_03 BLE/ESP32），
  真实热词诱发的误触发 ≈ 0。该度量应修正为「参考文本不含该词才计误触发」。
  附带在 tech_02 复现已知火山二遍 bug：Opus→Auk（见 `asr-bench-lessons-2026-08-01.md`）。

### 结论

1. **热词通道两平台均真实有效**：精选小库下腾讯命中率 +22.8pt（72.7%→95.5%），
   火山 +13.6pt（72.7%→86.4%）；CER 同向改善（腾讯 0.0173→0.0097，火山 0.0226→0.0155），
   热词不伤整体准确率。
2. **预算受限时「选哪些词」比「塞多少词」关键**：direct_full 按字典序塞满（腾讯 128 词、
   火山 31 词）增益大幅缩水（腾讯 +4.5pt、火山 0pt），差距全部来自低频/排序靠后的
   中文目标词被裁掉；tiered 按频率分层精选后，在相同预算上限内恢复到与理想小库持平
   （86.4% / 95.5%）。本轮未观察到「同词被稀释」效应——500 词规模下瓶颈在裁剪策略而非
   塞满本身，但库继续增长时分层精选是唯一的可持续路径。
3. **腾讯表通道（hotword_id）是大库正解**：500 词表命中 95.5%，与精选直传持平、
   高于字典序直传，且无每请求 payload 开销（30 表 × 1000 词容量）。
   注意含 `.` 的词进不了腾讯任何热词通道，只能 LLM 精修兜底。
4. **火山不应为热词关二遍**：no_nonstream（关二遍+直传）命中率 68.2% 全场最低，
   二遍对整体准确率是净收益；中文热词在二遍下也能经直传生效，
   英文混排词（CLAUDE.md 类）的二遍覆盖问题只能靠 LLM 精修兜底。
5. **对桌面端的策略建议**：腾讯侧维持词表通道 + 频率精选入表（`SyncHotwords` 现状正确，
   已补 `.` 过滤）；火山侧维持 80 tokens 预算精选直传（现状正确）；Python 侧
   `hotword_select` 评分排序与 C++ `EffectiveHotwords` 策略同向，500 词规模下暂无
   移植紧迫性，库规模再上一个量级时再把 EWMA/新近度评分下沉到 C++。

## 复现命令

```sh
cd scripts/e2e_test
python gen_corpus.py --manifest corpus/hotword_corpus.json   # 一次性合成音频
python run_hotword_bench.py --provider all --configs all --rounds 1 \
    --library-size 500 --create-tables --stamp hotword-full
```
