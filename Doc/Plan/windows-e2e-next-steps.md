# Windows 端到端测试 · 后续行动方案

> 配套文档：`windows-e2e-test-plan.md`（五层测试金字塔总方案）
>
> 编写日期：2026-07-15。本文档基于 L0/L1/L3/L4 已完成的现状，规划后续增强工作。

## 1. 背景与当前状态

基于五层测试金字塔（L0 纯单测 / L1 ASR 集成 / L2 微信渲染 / L3 固件真链路 / L4 微信真机），两个重点方向（① ASR 测试 ② 微信输入法测试）的闭环已建立：

| 层 | 状态 | 成果 |
|---|---|---|
| L0 | ✅ 完成 | `voicestick_windows_tests` 回归通过（exit=0） |
| L1 | ✅ 完成 | `voicestick_integration_tests`：真实火山 ASR + 语料 ogg 注入，12 条语料全通过（8 常规 CER<10%，4 数字/混合关键实体） |
| L2 | ⏭️ 跳过 | 增量有限（现有 30+ 微信测试覆盖状态机/热键/设备切换/UIPI/延迟/underrun） |
| L3 | ✅ 完成 | 固件 `test_playback` 回放钩子 + 2 Bug 修复（PSRAM 栈 fread 崩溃 + 回放不实时） |
| L4 | ✅ 完成 | 微信真机验证打通（微信识别正常 + CABLE peak=25202），仅验证 1 条短句 |

**已交付资产**：
- 测试目标：`voicestick_integration_tests`（CMake 目标，链接 voicestick_core + asr_client_win.cc）
- 固件改造：`audio_pipeline` test_playback 回放（SPIFFS 预读 PSRAM buffer + vTaskDelay 节流）
- 测试语料：`scripts/e2e_test/corpus/` 12 条 × pcm/ogg/txt + corpus.json
- 脚本：gen_corpus / verify_corpus / build_spiffs_image / run_l3_firmware / scan_ble / read_serial / loopback_capture / run_l4_wechat
- 方案文档：`windows-e2e-test-plan.md`
- Git：6 个提交已 push 到 origin/main

**过程中修复的 5 个 Bug**（记录于 `Expe/` 与记忆）：
1. 固件 PSRAM 栈 fread 触发 cache 禁用断言崩溃 → 预读 PSRAM buffer + memcpy
2. 固件回放全速循环产出加速音频致 ASR 无法识别 → vTaskDelay 节流
3. L1 断言点错误（final_countdowns vs pasted_text）→ focused_app 直接 Paste 不走倒计时
4. L1 帧间无节流致 ASR WebSocket 突发拥堵 → 40ms 节流模拟实时
5. L4 断言阈值过严（微信模式低电平）→ peak 主判据，nonzero_ratio 仅极端断言

## 2. 后续工作清单

按价值与优先级排序，每项含目标、实现方案、依赖、工作量、关联。

---

### 2.1 L4 多语料 + 边界用例（高优先级）

**目标**：补全方案 5.5 的人工抽检清单，验证微信输入法在各种场景下的鲁棒性，不止 1 条短句。

**现状缺口**：L4 仅验证「今天天气不错」1 条短句。方案 5.5 列出 6 项抽检均未系统执行：
- [ ] 长按热键（Ctrl+Win）→ 微信语音弹框出现
- [ ] 说出测试语料 → 微信候选框出现正确文字（多语料）
- [ ] 松开 → 文字上屏
- [ ] 短录音（<0.5s）过滤
- [ ] 连续多轮不串扰
- [ ] 首字延迟体感（<2s）

**实现方案**：
1. 扩展 `run_l4_wechat.py` 支持 `--phrase` 循环多语料（从 corpus.json 读 short/long/num/mix），每条语料跑一轮 loopback + 人工确认。
2. 加短录音用例（按住<0.5s 松开）验证过滤（记忆 `wechat-debug-audio-short-recording-filter`）。
3. 加连续多轮用例（3 条语料连发）验证不串扰（记忆 `wechat-output-mode-disconnect-cleanup`）。
4. 首字延迟：loopback 抓取同时记录按下到首帧 PCM 时间（TimedFakeSink 思路移植到 loopback，或用 VoiceStickApp 日志 `LogWechatLatency`）。

**依赖**：无新增基础设施（复用 loopback_capture + run_l4_wechat + 现有语料）。
**工作量**：1-2 天（脚本扩展 + 人工抽检）。
**关联**：方案 5.5、记忆 `wechat-input-method-hotkey`、`low-level-asr-masks-capture-gain-issue`。

---

### 2.2 compare_ogg.py L3 链路保真比对（高优先级）

**目标**：证明 L3 固件回放路径与 L1 桌面端注入路径产出等价的 Ogg Opus 流，确认两条测试路径可互相印证。

**现状缺口**：L1 用 FakeBleCentral 注入语料 ogg 解封的 Opus 帧，L3 用固件回放语料 PCM 经 Opus 编码后 BLE 上报。两条路径的 Opus 包序列是否一致未验证。

**实现方案**：
1. L1 侧：integration_tests 注入语料时同时落盘 coordinator 封装的 Ogg（复用 `debug_audio_recorder` 或 OggOpusMuxer 输出）。
2. L3 侧：`run_l3_firmware.py` 已采集裸 Opus 包（l3_captured_opus.bin），用 OggOpusMuxer 封装为 ogg。
3. `compare_ogg.py`：比对两个 ogg 的采样率/通道/preskip/帧数/Opus 包序列（字节级或哈希）。
4. 容差：固件 Opus 编码器与桌面 OggOpusDemuxer 解出的包可能字节不同（编码器版本/complexity 差异），改比对帧数 + 每帧 payload 长度 + 整体时长一致性。

**依赖**：L1 落盘 ogg（需 integration_tests 加落盘逻辑）。
**工作量**：1 天。
**关联**：方案 5.4.2a、`ogg_opus_demuxer.h`。

---

### 2.3 run_all.py 编排器（中优先级）

**目标**：一键串联 L0-L4，输出 HTML/JSON 报告，便于回归与交付。

**实现方案**：
1. `run_all.py` 按顺序执行：
   - L0：`voicestick_windows_tests.exe`
   - L1：`voicestick_integration_tests.exe`（需 config.toml 火山 key，无则 skip）
   - L3：`run_l3_firmware.py`（需设备 + 串口，无则 skip）
   - L4：`run_l4_wechat.py`（需用户交互，提示是否跑）
2. 收集每层 exit code + 输出，生成 JSON（机读）+ HTML（人读）报告。
3. 失败层高亮，附诊断信息（如 L1 的 CER 明细、L3 的帧统计、L4 的 peak）。
4. 支持 `--layer L0,L1` 选择性跑。

**依赖**：无（整合现有）。
**工作量**：1 天。
**关联**：方案第 7 节。

---

### 2.4 cer.py 独立工具（中优先级）

**目标**：L1 的 CER 现内联 C++（integration_tests.cc），抽 Python 版供 L4 微信识别结果比对（人工输入或 OCR 抓取的微信候选文字 vs 预期）。

**实现方案**：
1. `cer.py`：UTF-8 按字拆分 + 编辑距离，命令行 `python cer.py --hyp "今天天气不错" --ref "今天天气不错。"` 输出 CER。
2. `--keywords` 模式：关键实体包含判定（case-insensitive），用于数字/混合语料。
3. L4 流程：人工看完微信识别后输入候选文字，cer.py 比对 corpus.json 预期，输出 CER。

**依赖**：无。
**工作量**：0.5 天。
**关联**：L1 CER 实现（integration_tests.cc 的 SplitUtf8/EditDistance/Cer）。

---

### 2.5 L2 微信渲染解码正确性（低优先级）

**目标**：验证 wechat 路径 Opus→PCM 解码正确性（现有测试用 FakeWasapiRenderSink 不保存 PCM 内容，只测状态机/延迟）。

**现状缺口**：TimedFakeSink.GetBuffer 返回零填充 scratch_，不保存写入的 PCM，无法验证解码内容。L1 已间接验证 Opus 内容正确（ASR 识别），但 wechat 路径用同 AudioOpusDecoder，增量有限。

**实现方案**：
1. 新建 `CapturingFakeSink`（继承 WasapiRenderSink，ReleaseBuffer 时保存 PCM 到 buffer）。
2. 测试：FakeBleCentral 注入语料 Opus → coordinator wechat 模式 → CapturingFakeSink 收 PCM。
3. 参考 PCM：源 PCM 经 Opus 编解码（AudioOpusDecoder）。
4. 比对：频域相关性/SNR（Opus 有损，不精确匹配），或简化为能量/峰值/长度匹配。

**依赖**：无。
**工作量**：1 天。
**关联**：方案 5.3、core_tests.cc:377 TimedFakeSink、`audio_opus_decoder.cc`。
**优先级说明**：L1 已验证 Opus 解码正确（ASR 识别对），L2 增量有限，建议仅在 wechat 路径出现解码嫌疑时做。

---

### 2.6 capture_helper 抓取程序（低优先级）

**目标**：Win32 TOPMOST 窗口 + Edit 控件，拦截 Return 提交的文本到文件，供 L3 真实注入路径断言。

**现状缺口**：L1 用 FakeInputInjector 进程内捕获识别文本，绕过真实注入。L3 真实注入（VoiceStickApp SendInput 到前台）需 capture_helper 抓取。

**实现方案**：
1. 新建 `scripts/e2e_test/capture_helper/`（Win32 exe）：TOPMOST 窗口 + 多行 Edit， subclass 拦截 Return，把 Edit 内容写到文件 + 清空。
2. L3 真机流程：capture_helper 置顶 → L3 固件回放 → VoiceStickApp 注入文本到 capture_helper → 脚本读文件断言。
3. UIPI 处理：capture_helper 需与 VoiceStickApp 同完整性（记忆 `windows-uipi-weixin-injection`）。

**依赖**：无。
**工作量**：1-2 天（Win32 GUI）。
**关联**：方案 4.1/9 节、记忆 `windows-uipi-weixin-injection`。
**优先级说明**：L1 FakeInputInjector 已覆盖核心 ASR 断言，capture_helper 仅 L3 真实注入需要，推迟。

---

### 2.7 微信模式自动化（低优先级 / 大工程）

**目标**：解决 L4 的 BLE 单连接约束，实现 L4 全自动回放（不用人工按键说话）。

**现状缺口**：L4 需 VoiceStickApp 连设备接收音频，但 L3 bleak 不能同时连（BLE 单连接）。故 L4 用人工按键说话替代 L3 自动回放，半自动。

**实现方案**：
1. 给 VoiceStickApp 加 `--test-playback <file>` 命令行：app 连设备后下发 test_playback + remote_button 驱动固件回放，app 接收音频渲染 CABLE。
2. run_l4_wechat.py 改为：启动 VoiceStickApp --test-playback + loopback 抓取，全自动（不需人工按键）。
3. 注意：固件回放 PCM 需是语料（SPIFFS 已烧录 short_01.pcm 等）。

**依赖**：VoiceStickApp 命令行扩展（改 app 代码）。
**工作量**：2-3 天（app 改造 + 测试）。
**关联**：L3 固件回放钩子、记忆 `firmware-playback-hook-psram-stack`。
**优先级说明**：L4 半自动已验证链路通，全自动为提升效率，非必需。

---

### 2.8 CI 集成（低优先级）

**目标**：将 L0/L1 纳入 CI 自动回归。

**实现方案**：
1. L0（windows_tests）已可 CTest，纳入 CI。
2. L1（integration_tests）需联网 + 火山 key：用 GitHub Actions secrets 注入 `volcengine_api_key` 到 config.toml，跑 integration_tests。无 key 时 return 77 skip（已实现）。
3. L3/L4 需真机 + 串口 + CABLE + 微信，不适合 CI，保留本地跑。
4. 注意：Windows runner + MSVC + CMake + Ninja + opus 依赖配置。

**依赖**：CI 环境（GitHub Actions Windows runner）。
**工作量**：1-2 天（CI 配置 + 依赖装）。
**关联**：`.github/workflows/`、记忆 `windows-gitignore-and-signing`。
**优先级说明**：CI 对回归有价值，但 L1 联网+凭据需谨慎，且 Windows runner 配置复杂。

## 3. 建议执行顺序

1. **2.1 L4 多语料 + 边界用例**（高价值，补全微信验证广度，复用现有工具）
2. **2.3 run_all.py 编排器**（整合现有 L0-L4 出报告，提升回归效率）
3. **2.2 compare_ogg.py**（L3 链路保真，证明两路径等价）
4. **2.4 cer.py**（小工作量，L4 比对基础）
5. 其余（2.5-2.8）按需推进，优先级低或工程量大

## 4. 风险与注意事项

- **L4 人工抽检依赖用户配合**：多语料多轮需用户在真机旁操作，无法全自动（除非 2.7 落地）。
- **L3 设备状态**：固件回放需 SPIFFS 已烧录语料（build_spiffs_image.py + esptool）；烧录后需手动 Reset（记忆 `stick-s3-button-boot-control`）。
- **L1 联网+凭据**：火山 key 不可硬编码/提交，从 config.toml 读；CI 用 secrets。
- **微信模式低电平**：CABLE 电平低致 loopback nonzero_ratio 低，断言用 peak 主判据（记忆 `low-level-asr-masks-capture-gain-issue`）。
- **config 切换**：L4 需 `target=wechat_input_method`，验证后改回 `focused_app`，VoiceStickApp 需重启读新 config。
