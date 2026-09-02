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
- **小米遥控器 ATVV 工具组**（`Doc/Plan/xiaomi-remote-2-pro-support.md` §7.2，设备需先在系统蓝牙设置 Bond 配对；均只依赖 stdlib+bleak）：
  - `atvv_capture.py`：真机 golden 采集。bleak 直连 ATVV GATT 服务，应答 MIC_OPEN，落盘 `fixtures/xiaomi/<ts>/`：`events.jsonl`（Control 事件流）+ 每会话四件套 `session_<N>.adpcm`（原始 ADPCM 裸流）/ `session_<N>.json`（sidecar：帧长/增益/逐段 reset 字节区间，供 C++ 单测精确复现解码路径）/ `session_<N>.raw.wav`（纯解码）/ `session_<N>.wav`（解码+三点平滑+增益）。常驻自服务：连接成功与每段录制完成弹 Win32 提示框（无控制台反馈场景），遥控器睡眠断链后 2s 自动重连直至 `--duration` 上限（配 `--max-sessions 0` 可不限段数长时间值守）。内置 F5 抑制：遥控器语音键会经 OS 级 HID 通道连带发 F5，采集期间用 WH_KEYBOARD_LL 吞掉（注意 ctypes 必须显式声明 64 位 restype/argtypes，否则 HMODULE/LPARAM 截断导致钩子静默装不上或回调 OverflowError）。**前置约束：采集前必须退出 VoiceStick.exe——遥控器 ATVV 语音人格只授给第一个连上的主机，后连者只能看到残血 GATT 服务表（无 ATVV/HID）**；Windows 侧连接须禁用 GATT 服务缓存（bleak `winrt=dict(use_cached_services=False)`）防陈旧缓存假性 "service not found"。采集手法：贴嘴按住语音键后立刻开口，说完再松手（按键释放即 STOP；首段静音会丢内容）。`--self-test` 离线自测（11 项，含编码器往返与 sidecar 段落记账），无需设备无需 bleak。
  - `atvv_bench.py`：golden 会话的 ASR 离线评测。按 sidecar 复现解码 → 桌面端同参数后处理（三点平滑 + `--gain-db`，默认 12dB 对齐 `XiaomiAtvvSession::Options`；粒度差异：本脚本按整段会话平滑，桌面端按 640 样本帧切片平滑，每 640 样本 2 个边界样本取值不同，约 0.3%，对 ASR 无实质影响）→ 裸 PCM 直送真实 ASR（火山 `format=pcm` / 腾讯 `voice_format=1`，Python 侧无 Opus 编码器，PCM 是协议允许的等价路径），参考答案取自 fixtures 的 `refs.json`（`{"session_1": "文本"}`）或 `--refs`，报告写 `bench_results/atvv_bench_<stamp>.json|.md`（口径同 run_asr_bench，synthetic fixtures 会在报告顶层与 Markdown 头部显式标记）。调参闭环：`--gain-db`/`--no-smooth` 改参数重跑对比。`--self-test` 离线自测；`--emit-demo-fixture DIR [--pcm-source corpus/xxx.pcm] [--text-file xxx.txt]` 合成带 `synthetic: true` 标记的假采集目录（仅验证链路，不作识别率结论）。无凭据/无 fixtures/无 refs 均退出码 2。
  - `atvv_probe.py`：L3 真机探针。量测 GET_CAPS→CAPS 延迟、MIC_OPEN→首音频帧/ACK/STREAM_START 延迟、会话帧数/字节数、STOP→尾包时延分布、`--idle` 长连接静置稳定性，输出 JSON 报告（stdout + `--out` 文件）。检测 VoiceStick.exe 在运行则提示先退出（BLE 单连接互斥）。
  - L4 微信输入法复跑：`run_l4_wechat.py` 输入源无关（由桌面端 config 决定输入设备），待真机到位后执行。
  - fixtures 入库策略：`fixtures/xiaomi/demo_synthetic/`（约 78KB 冒烟资产，含 refs.json）入库 git 跟踪，供 golden 对拍/集成测试冒烟开箱即用；真机采集目录 `fixtures/xiaomi/<ts>/` 与评测产物 `bench_results/` 由 .gitignore 忽略，不入库。

## 依赖与设计文档

- 依赖 `bleak` / `numpy` / `sounddevice`（另 `battery_voltage_monitor.py` 需 `matplotlib`），**未列入根目录 `requirements.txt`**（该文件只含 `pyyaml` / `pyserial` / `Pillow`），运行前需另行 `pip install`。
- 设计文档见 `Doc/Plan/windows-e2e-test-plan.md` 与 `Doc/Plan/windows-e2e-next-steps.md`。
