# 火山 ASR 配置消融实验（2026-08-01）

承接 `Doc/Expe/asr-bench-baseline-2026-08-01.md` 问题清单第 1 条：基线发现火山在当前桌面端配置（`result_type=full` + `enable_nonstream=true` + `enable_ddc=true`）下**首 partial 延迟 ≈ 音频全长**（说话过程中不吐流式结果，第一条结果贴着音频结束才来；腾讯恒定约 1s）。本实验对这三个请求参数做消融，定位配置因素。

## 方法

- 运行器：`scripts/e2e_test/run_volc_ablation.py`（新增）。`asr_bench/volcengine.py` 的 `session_payload` / `connection_payload` / `run_clip` 新增可选参数 `result_type` / `enable_nonstream` / `enable_ddc`，默认值保持现状不变（桌面端口径），消融脚本通过覆盖参数对比。
- 语料：8 条代表性语料（short_01 / long_01 / mix_01 / tech_01 / fast_01 / noise_01 / english_01 / verylong_01，覆盖 1.9s–16.1s 时长与 7 个类别），每配置组合 2 轮，共 8×5×2=80 次真实火山会话。
- 指标口径与 run_asr_bench 完全一致（归一化 CER、首 partial 延迟、尾延迟、跨轮抖动）。
- 产出：`bench_results/volc_ablation_20260801-011351.json` / `.md`。

## 配置组合与结果（80/80 全部成功，零超时零断连）

| 配置 | result_type | nonstream | ddc | CER 均值 | 首 partial p50 (ms) | 尾延迟 p50 (ms) | 跨轮抖动 |
|---|---|---|---|---|---|---|---|
| base_full_nonstream_ddc（基线=桌面端现状） | full | ✓ | ✓ | 0.0000 | 4875 | 985 | 0 |
| single_nonstream_ddc | single | ✓ | ✓ | 0.0000 | 4875 | 1032 | 0 |
| full_ddc | full | ✗ | ✓ | 0.0116 | 4875 | 625 | 0 |
| single_ddc | single | ✗ | ✓ | 0.0116 | 4875 | 625 | 0 |
| single_plain | single | ✗ | ✗ | 0.0116 | 4875 | 625 | 0 |

逐语料首 partial 延迟五组配置两两相差 ≤15ms，全部贴音频全长（short_01 1.9s→1945ms；verylong_01 16.1s→16101ms）。

## 结论

1. **「首 partial 延迟 ≈ 音频全长」与这三个配置参数无关。** `result_type`、`enable_nonstream`、`enable_ddc` 五组组合的首 partial 延迟完全相同（p50 4875ms，逐语料差 ≤15ms，可视为测量噪声）。问题不在请求配置层，下一步应排查端点/输入格式/服务端出词策略（见「后续方向」）。
2. **没有配置能降低首 partial 延迟**，因此「CER 不明显劣化（差 <0.02）前提下首 partial 最低」的答案是：五组并列，差异只在 CER 与尾延迟。
3. **关 `enable_nonstream`（二遍）的代价与收益**：CER 均值从 0.0000 劣化到 0.0116（差 0.0116 < 0.02，主要是 tech_01 0.040 与 noise_01 0.053 两处，二遍重识别确实能纠回这两类错误）；收益是尾延迟 p50 从 ~985ms 降到 ~625ms（省约 360ms）。
4. **`result_type=full/single` 与 `enable_ddc` 在本语料上无任何可测差异**（CER、延迟、抖动完全一致）。
5. 五组配置跨轮抖动均为 0，火山在本机直连条件下结果完全确定。

## 桌面端配置建议

- **维持现状（`result_type=full` + `enable_nonstream=true` + `enable_ddc=true`），不要为「首 partial 慢」去改这三个参数**——改了也没用，反而关 nonstream 会损失二遍纠错的 CER 收益（术语/噪声场景）。
- 若未来更看重松手后的尾延迟（-360ms）且能接受术语/噪声场景约 0.01 的 CER 代价，可考虑关 `enable_nonstream`；但本实验语料规模小（8 条），改动默认值前建议先用 `run_volc_ablation.py --clips` 扩语料复测。
- 基线报告第 3 条（tech 术语确定性错误）在本实验中复现：关 nonstream 后 tech_01 出现 0.040 的 CER，说明二遍重识别对术语类错误有实际纠正作用，与「热词直传只吃流式第一遍、最终文本靠二遍」的已知行为一致。

## 后续方向（定位首 partial 慢的真正原因）

按嫌疑排序：

1. **换端点验证**：`bigmodel_async`（优化双向流式，"只在结果变化时返回"）与旧版 `bigmodel`（双向流式）对比，确认是否为 async 端点的出词策略所致。
2. **换输入格式验证**：Ogg Opus → 裸 PCM（`format=pcm`），排除 Ogg 容器整页缓冲导致服务端延迟出词的可能。
3. **真机对照**：抓桌面端真实会话的首 partial 时间，确认「按住说话半天没字幕」在用户真实链路上与离线回放口径一致（排除评测发送节奏因素）。
4. 咨询火山侧：`bigmodel_async` + Ogg Opus 下首结果是否本来就只在语句结束（VAD 断句）后才返回。
