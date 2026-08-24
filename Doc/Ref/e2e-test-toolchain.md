# Python E2E 真机验证工具链（scripts/e2e_test/）

本文承载 `scripts/e2e_test/` 工具链的完整说明，2026-08-02 由 `AGENTS.md`/`CLAUDE.md` 的「测试策略」章节迁入；根指南只保留摘要与指向本文的指针。

`scripts/e2e_test/` 是跨固件+Windows 端到端的半自动验证工具链（L0–L4），用真实 BLE 连接与真实 ASR/音频链路，不伪造结果。

## 工具清单

- **L0 语料**：`gen_corpus.py` / `verify_corpus.py` / `build_spiffs_image.py` 生成测试 PCM 语料并打包成 SPIFFS 镜像刷入固件。
- **L3 固件回放**：`run_l3_firmware.py` 用独立 bleak BLE 连接（VoiceStickApp 必须先断开，StickS3 BLE 独占单连接），下发 `test_playback` 回放 PCM 驱动录音，订阅 `audio_tx` 收 Opus 帧统计首帧延迟与帧数，配合串口日志 `playback set` 确认回放生效。
- **L4 微信输入法**：`run_l4_wechat.py` + `loopback_capture.py` 用 WASAPI 抓取 CABLE Output PCM，验证 Opus 解码->渲染->CABLE->微信识别链路（半自动，需人工按设备键说话并确认结果）。
- **辅助**：`scan_ble.py`（BLE 扫描）、`read_serial.py`（串口日志读取）、`replay_tencent_asr.py`（腾讯 ASR 回放，仅适用桌面端一页一帧调试 ogg，ffmpeg 语料会报 4007）、`spectrogram_server.py`（调试音频频谱分析页，v2.1.2 新增）。
- **ASR 离线评测基准**：`run_asr_bench.py --provider all` 对 corpus 全部语料（31 条 7 类别）实时节奏回放腾讯+火山 ASR（默认 3 轮压力测试），采集 CER/首 partial 延迟/尾延迟/跨轮抖动，产出 `bench_results/*.json|.md` 对比报告；协议实现库在 `asr_bench/`（纯 stdlib，凭据只读 config.toml）。另有 `run_volc_ablation.py`（火山 result_type/nonstream/ddc 配置消融）。关键结论：火山首 partial 延迟≈音频全长且与请求配置无关；nonstream 二遍会把第一遍正确的术语改错（如 Opus→Auk），内联热词与 boosting_table_id 对二遍均无效，只能靠 LLM 精修兜底。详见 `Doc/Expe/asr-bench-baseline-2026-08-01.md`、`volc-config-ablation-2026-08-01.md`、`volc-hotword-ablation-2026-08-01.md`。
- **热词专项评测**：`run_hotword_bench.py` 对热词语料（`corpus/hotword_corpus.json`，每条标注目标热词，`gen_corpus.py --manifest` 生成音频）按配置矩阵（baseline/直传小库/塞满对照/频率分层/表通道/火山关二遍）回放，量化热词命中率、CER、误触发率；`asr_bench/hotword_select.py` 是高频热词评分（频率×新近度×手动加权）与平台预算裁剪模块（火山 80 tokens / 腾讯 128 词），`asr_bench/tencent_vocab.py` 是腾讯热词表管理 API（TC3 签名纯 stdlib，`--create-tables` 自动同步评测词表）。注意腾讯热词拒绝含 `.` 的词（如 CLAUDE.md 只能走 LLM 精修兜底），桌面端 `SyncHotwords` 已加 `IsValidHotwordChars` 过滤（含单测）。设计见 `Doc/Plan/hotword-eval-and-prioritization.md`，首轮全矩阵结果见 `Doc/Expe/hotword-bench-2026-08-01.md`（精选小库：腾讯 +22.8pt、火山 +13.6pt；字典序塞满增益基本消失，频率分层在相同预算内追平理想小库；腾讯 500 词表通道与精选直传持平；火山关二遍反而全场最低，中文热词二遍下可生效、英文混排词仍被二遍覆盖）。
- **热词验收测试**：`run_hotword_acceptance.py` 做「桌面端同款发送」的回归——按桌面端完全一致的逻辑把配置里的热词发给真实 ASR（腾讯=字符过滤后 `sync_vocab` 同步词表走 `hotword_id`，火山=`hotword_select` 评分+80 tokens 装入 `corpus.context`），回放音频并报告每个热词的命中率与识别原文；音频可自动生成（edge-tts 为每个热词造句，产物在 `corpus/acceptance_<stamp>/`，不入库）或用 `--manifest` 标注清单指向已有录音（含真实调试 ogg）。输出 `bench_results/acceptance_<stamp>.json|.md`，含 baseline vs desktop 逐词增益与「不可入表词（需 LLM 精修）」诊断。与 hotword-bench 的区别：bench 是策略矩阵对比，acceptance 是「当前配置/热词到底能不能识别到」的验收与回归。
- **功耗记账导出与报告**：`power_log_dump.py` 用独立 bleak BLE 连接下发 `power_log dump` 命令、收 `state_tx` 分片重组二进制流，落盘 `power_log.bin` 并解析为 `power_log.csv`；`power_report.py` 读 CSV 与 `power_model.json`（每模式标定电流常数，初值为经验估计，bench 标定后更新）产出分模式时长/能耗占比、VBAT 衰减回归与续航估算。设计见 `Doc/Plan/power-mode-energy-profiling.md`。
- **电池电压监测与可视化**：`battery_voltage_monitor.py` 以 60s 间隔×60 分钟采集电池电压（数据源为设备端 power_log 的周期 VBAT 采样，经 `time_anchor` 锚定墙钟时间戳），结束后绘制时间-电压曲线并保存 PNG/SVG/CSV 到 `battery_logs/`。默认 live 模式保持 BLE 连接每周期增量导出；`--offline` 模式只在首尾连接、中间设备自动采样（免保持连接）。注意电池供电下设备空闲约 11 分钟自动关机断连，长时监测建议 USB 供电或 `--offline`。`--demo`（合成数据验证绘图链路）与 `--self-test`（离线自测数据管道）可不连设备运行。依赖 matplotlib。

## 依赖与设计文档

- 依赖 `bleak` / `numpy` / `sounddevice`（另 `battery_voltage_monitor.py` 需 `matplotlib`），**未列入根目录 `requirements.txt`**（该文件只含 `pyyaml` / `pyserial` / `Pillow`），运行前需另行 `pip install`。
- 设计文档见 `Doc/Plan/windows-e2e-test-plan.md` 与 `Doc/Plan/windows-e2e-next-steps.md`。
