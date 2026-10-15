# 耳语/轻声电平修复：AGC v2（自适应噪声门 + 更高增益上限）

> 状态：方案待评审（未实施）。证据 = 本机 `~/Library/Application Support/VoiceStick/DebugAudio` 29 段真实录音离线分析
> （2026-10-13 ~ 10-14，含截图中的 `20261014-222826-VS-53A8-session-11.ogg`）+ 固件 AGC 源码常量复算，
> 分析脚本口径见文末「附录 A」。

## 1. 现象

「调试会话」页里 StickS3（VS-53A8）的录音整体很轻：截图那一段 12.2 s 峰值仅 **-24.6 dBFS**，波形
几乎贴着中线。用户反馈「轻声/耳语识别率降低」——而耳语（嘴贴麦克风）正是本项目主打场景。

### 实测电平（16 kHz 解码，20 ms 帧 RMS，语音帧 = 高于 p90-12 dB 的帧）

| 分组（按时间） | 段数 | 语音帧 RMS p50 | 样本峰值 | AGC 后包络 p50 |
|---|---|---|---|---|
| 10-13 17:25–17:26（近场大声） | 2 | -24.5 / -26.9 dBFS | 0.0 / -1.0 dBFS | -13.2 / -17.2 dBFS |
| 10-14 07:17 | 1 | -25.3 dBFS | -0.3 dBFS | -15.1 dBFS |
| 10-14 10:07–11:03 | 14 | -33.8 … -48.9 dBFS | -1.1 … -30.9 dBFS | -22.2 … -41.8 dBFS |
| **10-14 22:22–22:28（轻声/耳语）** | **11** | **-37.1 … -58.6 dBFS** | **-20.0 … -45.4 dBFS** | **-29.7 … -52.4 dBFS** |

（包络 = 逐样本复算固件同系数 5 ms 攻 / 300 ms 释峰值跟随器，`audio_pipeline.c:112-146`。）

具体到耳语组：

| 录音 | 时长 | 峰值 | 语音 RMS p50 | 包络 p50 | ASR 结果 |
|---|---|---|---|---|---|
| 22:22 session-1 | 5.3 s | -45.4 dBFS | -58.6 dBFS | -52.4 dBFS | 有文本（33 字） |
| 22:22 session-2 | 3.0 s | -37.7 dBFS | -55.6 dBFS | -49.7 dBFS | **零输出**（app 日志无 `Enter pending confirmation`，同批其余 10 段均有文本） |
| 22:28 session-11（截图） | 12.2 s | -24.6 dBFS | -40.8 dBFS | -33.6 dBFS | 有文本（81 字） |

即：同一只设备、同一套固件，耳语段的电平比近场正常说话低 **20~30 dB**，且已经低到 ASR 会整段丢字的程度。

## 2. 根因：现行 AGC 在耳语区间**主动放弃增益**，在次轻区间又被上限截断

现行常量（`firmware/components/audio_pipeline/audio_pipeline.c:99-146`，三端镜像同值）：

```
AGC_TARGET 8192.0 (-12 dBFS)   AGC_MAX_GAIN 4.0 (+12 dB)   AGC_NOISE_FLOOR 184.0 (-45 dBFS)
env = |x| 峰值跟随(5ms/300ms)
gated = env < 184
desired = gated ? 1.0 : clamp(8192/env, 0.1, 4.0)      // ← gated 时 desired = 1.0，即 0 dB
```

### 模式 B（最严重）：噪声门把耳语**整段**挡在门外 —— 增益 ≈ 0 dB

耳语经 ES8311（PGA 18 dB）后的**前级包络约 -42 ~ -52 dBFS**，几乎全部低于 -45 dBFS 门限 ⇒
`desired = 1.0`，AGC 不做任何放大（甚至把已有增益按 τ=1 s 拉回 0 dB）。

可复核判据：若 AGC 真的给了 +12 dB，输出包络必然 ≥ `4 × 184 / 32768` = 736 ⇒ **-33.0 dBFS**；
而 22:22 session-1 / session-2 实测包络 p50 为 **-52.4 / -49.7 dBFS**，低于该下界 16~19 dB
⇒ 这两段的实际增益只能是 ≈ 0 dB。离线把现行算法**再跑一遍**这两段录音，输出与输入逐帧一致，同样证明生效增益为 0。

推论：**只调大 `AGC_MAX_GAIN` 完全无效**（离线复算：+24 dB、+36 dB 两档在不改门限时输出一字未变）。

### 模式 A：门限之上，被 +12 dB 上限截断

包络落在 -45 ~ -24 dBFS 之间时 `desired = 8192/env` 会被 `MAX_GAIN=4` 截断 ⇒ 输出包络 = 输入 + 12 dB，
即 **-33 ~ -12 dBFS**，距 -12 dBFS 目标最多差 21 dB。22:2x 里较亮的 7 段（包络 -29.7 ~ -38.1 dBFS）
正落在这一区间。

### 需要多少增益

把前级包络 -42 … -52 dBFS 拉到 -12 dBFS 目标，需要 **+30 … +40 dB** —— 现行 +12 dB 差 18~28 dB，
就是 2026-10-14 收紧前的 +20 dB 也仍差 10~20 dB。

> 结论：这不是 10-14 收紧「改坏」的单一回归，而是 **AGC 设计从未按真实耳语电平标定**（2026-07-25 的
> `software-agc.md` 就是为「声音轻」而做，但目标/上限是照正常说话定的）。10-14 收紧只是把天花板再压 8 dB、
> 目标再降 6 dB，让模式 A 又恶化 14 dB。

## 3. 方案：AGC v2

### 常量变更

| 参数 | 现行 | AGC v2 | 理由 |
|---|---|---|---|
| `AGC_TARGET` | 8192 (-12 dBFS) | **不变** | 保留 10-14 的爆破音余量决策 |
| `AGC_MAX_GAIN` | 4.0 (+12 dB) | **63.0 (+36 dB)** | 耳语前级包络 -42…-52 dBFS，实测需 +30…+40 dB |
| 噪声门 | 固定 `184` (-45 dBFS) | **自适应 `max(16, 3 × nf)`**（nf = 包络最小值跟踪：瞬时下降、τ≈6 s 缓升） | 固定门限在安静房间也把耳语挡住；自适应门安静时开、嘈杂时自动回到旧行为 |
| 门内行为 | `desired = 1.0` + τ=1 s 回落到 0 dB | **冻结当前增益，仅以 τ=3 s 缓退** | 句间 0.3~0.8 s 停顿不再把 +30 dB 打回 0 dB（否则下一句首字又低 20 dB） |
| 增益上升 | 一律 500 ms | `desired > 4×gain` 时 **100 ms**，否则 500 ms | 会话起始 gain=1 → +36 dB，500 ms 太慢；实测 t+100 ms 处快攻多 5.4 dB |
| `AGC_MIN_GAIN` 0.1 / 0.8FS 瞬时限幅 / HPF 120 Hz / 每会话复位 | **保留** | | 限幅负责防削波，快攻不会引入削波 |

### 逐样本逻辑（相对现行的 diff）

```
ax = |x|
env = 峰值跟随(ax)                         // 5 ms / 300 ms，不变
// 新增：噪声底跟踪
if (env < nf) nf = env; else nf += (env - nf) * 1/(16000*6);
gate_thr = fmaxf(16.0f, nf * 3.0f);        // 下限 -66 dBFS，防纯数字静音乱开门

gated = env < gate_thr;
desired = gated ? 1.0f : clamp(AGC_TARGET/env, AGC_MIN_GAIN, AGC_MAX_GAIN);
gc = gated ? GAIN_GATE_SLOW(τ=3s)
           : (desired < gain ? GAIN_DOWN(2ms)
                             : ((desired > 4*gain) ? GAIN_UP_FAST(100ms) : GAIN_UP_SLOW(500ms)));
gain = gc*gain + (1-gc)*desired;
// 0.8FS 瞬时限幅 + clamp 到 int16 —— 不变
```

会话开始复位 `env / gain / nf`（与现有 `s_agc_env/s_agc_gain` 同点）。

### 离线复算（拿真实录音当输入，Python 逐样本复算）

耳语组 session-1/-2 的录音本身就是**未被放大的前级信号**（见模式 B 判据），因此可直接当耳语测试向量：

| 向量 | 现状（RMS p50 / 峰值） | AGC v2（RMS p50 / 峰值） |
|---|---|---|
| 耳语 22:22 s1 | -58.6 / -45.4 dBFS | **-24.2 / -10.0 dBFS** |
| 耳语 22:22 s2 | -55.6 / -37.7 dBFS | **-21.0 / -7.3 dBFS** |
| 耳语 22:28 s10 | -44.3 / -30.1 dBFS | **-20.1 / -3.3 dBFS** |
| 耳语 22:28 s11 | -40.8 / -24.6 dBFS | **-20.6 / -2.2 dBFS** |
| 网关 10-14 s30（大声） | -25.3 / -0.3 dBFS | -24.0 / -1.9 dBFS（限幅生效，无削波） |
| 近场大声 10-13 s27 | -24.5 / -0.0 dBFS | -22.1 / -1.9 dBFS |
| 中等 10-14 s2 | -37.9 / -15.1 dBFS | -25.8 / -1.9 dBFS |

噪声底（帧 RMS p5，越低越好）：耳语 s1 -87.7 → -55.0 dBFS；s11 -93.6 → -76.8 dBFS；
网关大声 s30 **-62.8 → -57.9 dBFS（仅 +4.9 dB）** —— 自适应门确实没把嘈杂场景的底噪抬起来。

## 4. 改动清单（四端必须逐字同步，否则 golden 对拍红）

1. 固件 `firmware/components/audio_pipeline/audio_pipeline.c`：常量 + `agc_process` 门/攻速逻辑 + `nf` 状态与复位
   （主循环 `:668-671` 与 drain `:778-781` 共用 `agc_process`，无需改调用点）。
2. macOS `desktop/macos/Sources/VoiceStickCore/PcmPostprocessor.swift`（小米直连链路同算法镜像）。
3. Windows `desktop/windows/src/pcm_postprocessor.{h,cc}`。
4. Python 镜像 `scripts/e2e_test/atvv_capture.py`（`Agc` 类 + `self_test()` 新断言）→
   `scripts/e2e_test/regen_fixture_wav.py` 再生 `fixtures/xiaomi/demo_synthetic/session_1.wav`
   （**先断言解码与已提交 `session_1.raw.wav` 逐样本一致**，见 `Doc/Expe/xiaomi-direct-desktop-hpf-agc-lessons-2026-10-13.md` 经验四）。
5. 测试：`Tests/VoiceStickTests/PcmPostprocessorTests.swift`、`desktop/windows/tests/core_tests_codec_serial.cc`
   `TestPcmPostprocessor()`。
6. 文档：`Doc/Plan/software-agc.md` 追加「AGC v2」附录（或由本文取代其参数表）；`Doc/Expe/` 沉淀一条真机结论。
7. 不加配置开关（沿用 `software-agc.md` 的 YAGNI 决定）；小米链路已有 `hpf/agc` 逃生开关。

## 5. 验证计划

1. **离线（本地可做）**：Python 镜像 self-test 新增断言——
   - 耳语拉起：-58 dBFS RMS 的真实耳语向量 → 输出语音 RMS 落在 -28…-18 dBFS；
   - 静音不抬：-70 dBFS 宽带噪声单独输入 → 输出包络 ≤ -65 dBFS 且 `gain ≤ 1`（自适应门不放行）；
   - 大声不削波：-1 dBFS 峰值向量 → 输出 |x| ≤ 26215（0.8FS 限幅），无 `|x| == 32767` 平顶；
   - `reset()` 幂等 + 「流式 == 全新实例流式」（**不是**「分帧 == 整段」，见经验三）。
   用本机 29 段真实录音做回归：耳语组语音 RMS 收敛到 -20 ± 4 dBFS。
2. **三端一致**：macOS `swift run VoiceStickTests` 全绿（含 golden fixture）；Windows 端本机不能构建时用
   clang++ 语法检查 + 小 main 与 Python 镜像逐样本对拍（`Doc/Expe/xiaomi-direct-desktop-hpf-agc-lessons-2026-10-13.md` 经验一），
   正式 ctest 待 Windows 机。
3. **固件**：`source /Users/hwp/esp/v5.5.1/esp-idf/export.sh && idf.py build`（**不是** `~/esp/esp-idf`，那是 v6.1）。
4. **真机 A/B（用户配合，唯一权威判据）**：`debug_audio_cache = true`，同文本耳语 5 段 + 近场大声 3 段，判据：
   - 耳语：调试页波形明显抬起，语音帧 RMS **-22 ± 3 dBFS**、峰值 -15 ~ -3 dBFS、**无 0 dB 平顶**；
   - 近场大声：仍不削波，增益被压到 0 dB 以下；
   - 噪声：语音间隙底噪相比改前升幅 ≤ 10 dB；
   - ASR：原来零输出的耳语会话（如 22:22 session-2 那种电平）能出文本，同文本字错率主观对比改善。
   - 串口 `agc env=… dBFS gain=… dB` 每秒一行，可直接看增益是否真的挂到 +30 dB 以上。

## 6. 风险与回滚

- 风险 1：高增益会同时抬起环境噪声（键盘、风扇）。缓解 = 自适应门（实测嘈杂向量 s30 底噪只 +4.9 dB）
  + 已实现的桌面端 RNNoise（`audio_noise_suppression`，默认关，双端镜像）。
- 风险 2：快攻（100 ms）在「增益挂高位时突然大声」的瞬间可能过冲——由 0.8FS 无记忆限幅兜底（离线复算 s27/s30 峰值恰好 -1.9 dBFS，无削波）。
- 风险 3：三端不同步 → golden 对拍红。按第 4 节顺序改并跑测试。
- 回滚：只改回 4 个常量（`MAX_GAIN 4.0`、门限换回固定 `184.0`、`GAIN_GATE` 换回 1 s、去掉快攻），无需降级版本。

## 7. 备选方案（不作为主方案）

- **A. 桌面端做电平归一**（`VoiceStickCoordinator.handleAudioFrame` 已有 Opus 解码→RNNoise→重编码通路）：
  可行但多一道编解码、改变送 ASR 的流、对固件/其它消费端无效，只适合做 P2 兜底或 A/B 对照。
- **B. 提高 PGA 18 → 24 dB**：对 SNR 无本质帮助（声学噪声同比放大），且牺牲近场大声的 ADC headroom
  （`Doc/Plan/audio-gain-tuning.md` 的原始教训），不采用。
- **C. 改 ASR 服务端参数**：需先核对腾讯/火山是否提供增益类参数，且不解决本地留档与虚拟麦链路，不作为主手段。

## 8. 附带建议：调试页给出绝对电平

频谱页把每个会话的频谱按自身最大值归一化（`scripts/e2e_test/spectrogram_server.py:229-230` `spec[i] -= maxDb`），
所以**再轻的录音看起来也一样亮**——这正是「波形看着小、频谱看着正常」的错觉来源。建议：
会话列表每行显示 `峰值/RMS dBFS`，悬停状态栏显示当前视口的绝对 dBFS。改动纯前端、零风险，
却是第 5.4 节验收判据的直接读表工具。

## 附录 A：本文数据的复现口径

```bash
# 全量电平表（峰值 / 均值）
cd "~/Library/Application Support/VoiceStick/DebugAudio"
for f in *.ogg; do ffmpeg -hide_banner -nostats -i "$f" -af volumedetect -f null - 2>&1 | grep volume; done
```

逐样本包络/AGC 复算：16 kHz 单声道解码 → 20 ms 帧 RMS；包络用固件同系数
（`ATK=0.98758 / REL=0.99979`），语音帧取 RMS > p90-12 dB 的帧；AGC v2 复算用同系数 + 第 3 节逻辑。
