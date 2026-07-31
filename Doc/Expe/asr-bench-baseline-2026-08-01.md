# ASR 离线评测基准与基线报告（2026-08-01）

记录 VoiceStick 离线 ASR 评测工具链（`scripts/e2e_test/run_asr_bench.py` + `asr_bench/` 库）的构建与首次全量基线结果。动机：用户反馈腾讯/火山两家 ASR 都偶发「识别不稳定」，需要客观、可重复的对比评估手段来定位效率与稳定性问题。

## 工具链

- **运行器**：`scripts/e2e_test/run_asr_bench.py --provider all`（默认 3 轮全量回放构成压力测试；`--rounds 1 --clips id1,id2` 可做快速验证）。纯 stdlib 实现（raw socket WebSocket），凭据只从 `%APPDATA%\VoiceStick\config.toml` 读取。
- **协议保真度**：火山走 bigmodel_async 可复用连接协议（帧格式复刻 `replay_volcengine_asr.py` / `asr_protocol.cc`，`result_type=full` + `enable_nonstream` + `enable_ddc`，与桌面端一致）；腾讯走 asr/v2 WebSocket（opus 封装小端长度、`engine_model_type=16k_zh_en`、`needvad=1`，与桌面端一致）。音频按真实时长实时节奏发送。
- **语料**：`corpus/corpus.json` 从 12 条扩到 31 条，7 个新类别（fast +40% 语速 / slow -20% / noise 近似 SNR 20/15/10 白噪声 / english / tech 术语 / verylong 约 20s 长句 / voice 男声），edge-tts 合成 + ffmpeg 转 16kHz Ogg Opus 32kbps（对齐固件编码参数）。`gen_corpus.py` 支持逐条 `rate`/`voice`/`noise_snr_db`，已有语料自动跳过（`--force` 重生成）。
- **指标**：CER（归一化：去标点/空白、大小写与全半角归一、中文数字转阿拉伯）、首 partial 延迟（首帧音频→首个非空流式结果）、尾延迟（末帧音频→最终结果）、跨轮结果抖动（同语料不同轮次识别文本的最大编辑距离比）、失败/断连统计。
- **产出**：每次全量跑生成 `bench_results/asr_bench_<时间戳>.json`（机器可读，供版本间 diff 回归）+ 同名 `.md`（人读）。本次基线：`asr_bench_20260801-003002.json/.md`。

## 修复的工具链 bug（评测中发现）

ffmpeg 生成的 Ogg 一页封装多个 Opus 包，旧 `demux_ogg_packets`「一页一帧」逻辑把整页当一帧（首帧 3570B），腾讯直接报 4007 转码失败并断连。已改为按段表（lacing）正确拆包（`asr_bench/wsproto.py`）。注意 `replay_tencent_asr.py` 里仍是旧逻辑，回放 ffmpeg 语料会复现 4007——该脚本只适用于桌面端 OggOpusMuxer 产出的一页一帧调试音频。

## 基线结果（31 条 × 2 平台 × 3 轮 = 186 会话，真实凭据）

| 指标 | 腾讯 | 火山 |
|---|---|---|
| 成功率 | 93/93 | 93/93 |
| CER 均值 / 最差 | 0.0224 / 0.2222 | 0.0277 / 0.2609 |
| 首 partial 延迟 p50 / p95 | 968 / 1094 ms | 4172 / 18656 ms |
| 尾延迟 p50 / p95 | 141 / 188 ms | 984 / 1453 ms |
| 跨轮抖动 均值 / 最差 | 0.0015 / 0.0455 | 0 / 0 |

## 已定位的问题清单

1. **火山首 partial 延迟 ≈ 音频全长（流畅度主因候选）**。逐条数据：short_01（1.5s 音频）首 partial 1953ms；verylong_02（约 19s）首 partial 18953ms。首 partial 与音频长度强相关，即当前配置下火山几乎不在说话过程中吐流式结果，第一条结果贴着音频结束才来；腾讯首 partial 恒定约 1s 与音频长度无关。这直接解释了「按住说话半天没字幕、松手才出字」的卡顿感。怀疑与 `result_type=full` / `enable_nonstream`（二遍）/ `enable_ddc` 组合有关，下一步做配置消融实验（关 nonstream、result_type=single 等）找兼顾「流式快 + 最终准」的配置。
2. **火山尾延迟约为腾讯 7 倍**（p50 984ms vs 141ms）。松手到最终文本的等待，火山接近 1s 起步、长句 p95 到 1.6s；腾讯稳定在 200ms 内。
3. **火山术语识别稳定错误**：tech_02「Opus…Ogg」三轮一致识别为「Ogg…Auk」（CER 0.2609）；voice 类（男声）火山 CER 0.0294 而腾讯为 0。这类确定性错误适合用热词表兜底（对应现有 boosting_table_id 链路）。
4. **数字表示差异（两家共同）**：mix_02「百分之八十」两家都输出「80%」，归一化后仍算错（CER 0.2222）。不是识别错误而是表示形式差异，靠 LLM 精修/后处理统一，评测侧后续可考虑加入数字形式归一。
5. **腾讯结果跨轮轻微抖动**：jitter_max 0.0455（tech_03「BLE」间歇识别为「blip」）；火山三轮结果完全一致（抖动 0）。抖动本身不大，但说明腾讯对同一输入非完全确定。
6. **本次未复现断连类「不稳定」**：186 会话零超时、零断连、零服务端错误。结论：在本机直连、网络良好的条件下两家服务本身稳定；用户感知的「识别不稳定」更可能来自设备端音频链路（BLE 丢帧、AGC/电平问题）或弱网，建议下一步用真机 L3 回放链路或网络损伤（丢包/抖动）对比实验定位。

## 后续实验与修复（2026-08-01 同日更新）

基于上述发现完成的消融实验与工具链修复，结论有重要修正：

1. **「首 partial ≈ 音频全长」与请求配置无关**。对 result_type / enable_nonstream / enable_ddc 五组组合消融（80 会话全成功），首 partial 延迟五组完全相同（逐语料两两差 ≤15ms），问题不在请求配置层；桌面端维持现状配置。详见 `Doc/Expe/volc-config-ablation-2026-08-01.md`。
2. **tech_02「Ogg…Auk」错误是 nonstream 二遍重识别引入的**（第一遍本来识别为 Opus/OG，基本正确）。内联热词直传对二遍最终文本无效（带/不带输出逐字节相同），实测 `boosting_table_id`（自学习平台热词表）对二遍同样无效——该错误的兜底路径只有桌面端 LLM 精修。详见 `Doc/Expe/volc-hotword-ablation-2026-08-01.md`。上节第 3 条「适合用热词表兜底」的判断据此修正。
3. **腾讯发送节奏测量开销已修复**：`asr_bench/tencent.py` 改为 select 零超时探测后再收帧，verylong 总延迟从 29.1s 降到 16.6s（≈音频 16.1s + 尾延迟），总延迟指标恢复可信。下节「总延迟被高估约 1–2s」的局限描述已过时。
4. **CER 归一化加入中文数字短语解析**（`asr_bench/metrics.py`）：「百分之八十→80」「十五→15」等，离线重算基线 186 条 24 条 CER 下降、0 条上升；mix_02 从 0.2222 降到 0。上节第 4 条的假错误已消除。
5. `replay_tencent_asr.py` 的 Ogg 抽帧已改为复用 `asr_bench/wsproto.py` 的段表拆包，ffmpeg 语料不再报 4007。

### 修复后更新基线（2026-08-01 重跑）

修复 pacing 与数字归一化后全量重跑。火山数据取自 `asr_bench_20260801-012813.json`（92/93，1 例会话超时属真实瞬态），腾讯数据取自 `asr_bench_tencent-final.json`（93/93；首次重跑腾讯段遇本机 DNS 故障 35 例，网络恢复后补跑，用 `merge_bench_results.py` 合并），合并产物 `bench_results/asr_bench_merged_20260801-025118.json/.md`：

| 指标 | 腾讯（旧→新） | 火山（旧→新） |
|---|---|---|
| 成功率 | 93/93 → 93/93 | 93/93 → 92/93 |
| CER 均值 | 0.0224 → **0.0089** | 0.0277 → **0.0134** |
| 首 partial p50/p95 | 968/1094 → **672/750** ms | 4172/18656 → 4157/18656 ms（不变，与配置无关） |
| 尾延迟 p50/p95 | 141/188 → 125/172 ms | 984/1453 → 969/1516 ms |
| 总延迟 p50/p95 | 6625/29125 → **4375/19218** ms（修复后 ≈ 音频时长+尾延迟） | 4609/19735 ms（火山 runner 未变，沿用原值） |
| 跨轮抖动最差 | 0.0455 → 0.0435 | 0 → 0 |

两家 CER 下降主要归因数字归一化消除假错误；腾讯首 partial 改善约 300ms 归因发送节奏修复（不再每帧阻塞轮询）。火山首 partial 形态未变，再次印证其与客户端无关。

## 测量口径与已知局限

- 语料为 edge-tts 合成，分布与真人发音有差距；CER 绝对值仅供两家横向对比与版本间回归，不代表真实场景字准。
- 「总延迟」受发送节奏影响大：本基线生成时腾讯侧发送循环有每帧非阻塞收帧开销，Windows 定时器粒度导致长音频总延迟被高估（verylong p95 达 29s）；已在 `asr_bench/tencent.py` 修复（select 零超时探测），修复后 verylong 总延迟 ≈ 音频时长 + 尾延迟。以首 partial 延迟与尾延迟为核心指标。
- 噪声语料的 SNR 为近似值（语音 rms 按 0.5 估算），只用于压力对比趋势，不是精确声学测量。
- 评测默认不带热词/自学习表（客观基线）；火山 `corpus` 热词直传只影响流式第一遍（见 `Doc/Ref/volcengine-asr.md`），本基线测的是无热词裸能力。

## 复现与回归

```sh
cd scripts/e2e_test
python run_asr_bench.py --provider all            # 全量基线（约 25 分钟）
python run_asr_bench.py --provider all --rounds 1 --clips short_01,mix_01   # 冒烟
```

改动 ASR 链路（桌面端协议、配置默认值、热词策略）后重跑全量，用新旧 JSON 的 `providers.*` 指标 diff 判断回归。
