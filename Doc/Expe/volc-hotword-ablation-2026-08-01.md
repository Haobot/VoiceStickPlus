# 火山 corpus 热词直传兜底效果对照实验（2026-08-01）

验证基线报告（`Doc/Expe/asr-bench-baseline-2026-08-01.md`）问题清单第 3/5 条中两类术语识别错误能否被 `corpus.context` 热词直传兜底：

- 火山把 tech_02 的「Opus…Ogg」三轮稳定识别为「Ogg…Auk」（CER 0.2609）；
- 腾讯把 tech_03 的「BLE」间歇识别为「blip」。

工具：`scripts/e2e_test/replay_volcengine_asr.py`（纯 stdlib，bigmodel_async 可复用连接协议，`result_type=full` + `enable_nonstream` + `enable_ddc`，与桌面端一致），凭据只从 `%APPDATA%\VoiceStick\config.toml` 读取，真实服务回放，不伪造结果。对照变量仅为热词：先 `--no-config-hotwords` 裸跑，再 `--hotwords "Opus,Ogg,ASR,ESP32,BLE,WebSocket"`（6 词，估算 10 tokens，在客户端 80 预算内）。

参考文本（`corpus/corpus.json`）：

- tech_02：「把 ASR 的 Opus 音频流封装成 Ogg 格式再发送。」
- tech_03：「ESP32 通过 BLE 把音频数据发送到桌面端。」

## tech_02（火山稳定错误用例）

| 配置 | 流式第一遍最终 partial | 二遍最终文本（RESULT） |
|---|---|---|
| 无热词 | 把 ASR 的 Opus 音频流封装成 OG 格式再发送 | 把 ASR 的 Ogg 音频流封装成 Auk 格式再发送。 |
| 带热词 | 把 ASR 的 Opus 音频流封装成 OG 格式再发送 | 把 ASR 的 Ogg 音频流封装成 Auk 格式再发送。 |

- 裸跑复现了基线错误：最终文本「Ogg…Auk」与基线三轮结果一致。
- **带热词后所有输出逐字节不变**——流式 partial 序列相同，二遍最终文本仍是同样的错误。热词直传对这条用例完全无效。
- 值得注意：本用例的流式第一遍**本来就对**（「Opus…OG」，仅 OG/Ogg 大小写差异），错误是二遍重识别引入的。

## tech_03（腾讯间歇错误用例，对火山回放）

| 配置 | 流式第一遍中间 partial（关键帧） | 二遍最终文本（RESULT） |
|---|---|---|
| 无热词 | `E S P 32通过 Bleed 把音频数据发送到…` | ESP32通过 BLE 把音频数据发送到桌面端。 |
| 带热词 | `ESP32通过 BLE 把音频数据发送到…` | ESP32通过 BLE 把音频数据发送到桌面端。 |

- 火山的二遍最终文本不带热词时已经正确，「blip」错误是腾讯侧现象，火山回放未复现（符合基线：该条火山 CER 为 0，抖动在腾讯）。
- 但热词对**流式第一遍**的质量提升清晰可见：无热词时第一遍把 BLE 识别为「Bleed」、ESP32 拆成「E S P 32」；带热词后第一遍直接输出规范的「ESP32」「BLE」。这是本次实验中热词直传唯一观测到的实际效果。

## 附加消融：tech_02 关掉二遍（`--no-nonstream`，无热词）

```
RESULT: 把 ASR 的 Opus 音频流封装成 OG 格式再发送。
```

最终文本等于流式第一遍结果，识别正确（仅 OG/Ogg 大小写差异）。进一步坐实：tech_02 的错误 100% 由 `enable_nonstream` 二遍重识别引入，而热词够不到二遍。

## 结论

1. **与 `Doc/Ref/volcengine-asr.md` 的记载完全一致**：`corpus.context` 热词直传只在流式第一遍生效，二遍（nonstream 重识别）最终文本忽略内联热词。tech_02 带/不带热词输出逐字节相同，是最直接的证据。
2. 热词直传对第一遍确有提升（tech_03 的 partial 从「Bleed / E S P 32」变为「BLE / ESP32」），但 Voice Stick 默认开启二遍，用户看到的最终文本吃不到这个提升。
3. tech_02 这类「第一遍对、二遍改错」的用例，热词直传无法兜底；自学习平台 `boosting_table_id` 已于 2026-08-01 用本机 config.toml 中已配置的真实表 ID 实测（`replay_volcengine_asr.py corpus/tech_02.ogg --no-config-hotwords --boosting-table-id <id>`），最终文本仍为「Ogg…Auk」——**热词表对二遍同样无效**。因此该类错误的唯一兜底路径是桌面端 LLM 精修（prompt 附加热词表，已上线）。
4. 腾讯的「blip」间歇错误与火山热词链路无关，本实验不适用；腾讯侧无热词直传能力，只能靠 LLM 精修兜底。

## 复现命令

```sh
cd scripts/e2e_test
# tech_02 对照
python replay_volcengine_asr.py corpus/tech_02.ogg --no-config-hotwords
python replay_volcengine_asr.py corpus/tech_02.ogg --hotwords "Opus,Ogg,ASR,ESP32,BLE,WebSocket"
# tech_02 附加消融：关二遍
python replay_volcengine_asr.py corpus/tech_02.ogg --no-config-hotwords --no-nonstream
# tech_03 对照
python replay_volcengine_asr.py corpus/tech_03.ogg --no-config-hotwords
python replay_volcengine_asr.py corpus/tech_03.ogg --hotwords "Opus,Ogg,ASR,ESP32,BLE,WebSocket"
```

（Windows 控制台为 GBK 代码页时输出中文会乱码，加 `PYTHONIOENCODING=utf-8` 环境变量即可。）
