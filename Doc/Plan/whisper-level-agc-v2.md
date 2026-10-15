# 耳语/轻声电平修复：AGC v2（噪声预算 + 自适应本底 + 增益上限 +12→+36dB）

> 状态：**已实施 + 真机验收通过**（2026-10-15，含当日 v2.1 修复）。四端同步（固件 C / macOS Swift / Windows C++ / Python 镜像）+ golden fixture 再生；
> Python self-test 17/17、macOS VoiceStickTests 1033/1033、C++↔Python **全链逐字节一致**（113680 样本 `cmp` 通过）、
> 固件 idf.py build 通过 + 已串口烧录到 VS-53A8。
> **真机结果（20:16）**：设备串口 `agc env=-38…-57 dBFS nf=-73.6 dBFS gain=+26.6→+35.0 dB`（本底已是真实环境底噪、
> 增益挂到 +35dB）；录音语音帧 RMS **-22.1 dBFS**（判据 -22±3 ✓）、峰值 **-5.6 dBFS**、无削波 ——
> 修复前同场景为 -38…-42 dBFS。
> 证据 = 本机 `~/Library/Application Support/VoiceStick/DebugAudio` 真机录音离线分析（2026-10-13 ~ 10-15，
> 含截图中的 `20261014-222826-VS-53A8-session-11.ogg` 与 10-15 19:24~20:01 的 15 段复现）。

## 0. v2.1 修复（2026-10-15 晚，同日第二次）：click guard 必须在 AGC 之前

v2 落地后用户复测「**还是太轻**」，10-15 19:24~20:01 的 15 段新录音峰值 -10~-40 dBFS、语音帧 RMS -28~-58 dBFS，
与「AGC 生效」的预期（-12dBFS 包络 / 约 -22dBFS RMS）不符。逐一排除后定位到**冷启动本底估计被按键咔哒污染**：

- 管线顺序是 `HPF → AGC → click_guard`，即 **AGC 看得到按键咔哒**；
- 包络是 300ms 慢释，咔哒在 0~50ms 落下后把包络顶到很高，**整个冷启动窗（120ms）量到的「本底」其实是咔哒尾部**；
- 本底被钳到上限 -45dBFS ⇒ 门限 = 3×184 = **-33dBFS** ⇒ 耳语包络（-46dBFS）永远低于门限 ⇒ 增益被噪声预算压在
  `184/env ≈ 1.1` ⇒ **整场不放大**（输出 ≈ 原始拾音电平，正是用户听到的「还是太轻」）。

为什么离线测试没抓到：线下测试用的 `.ogg` 已经是 **click guard 之后**的输出（头部 120ms 被静音），
本底估计自然干净；真机 AGC 看到的却是 guard 之前的信号。**这是一次「测试向量与真机输入形状不一致」的教训。**

v2.1 两处改动：

1. **固件把 click guard 移到 HPF/AGC 之前**（主循环 + drain 两处）——咔哒不再进入 AGC 包络；
2. **本底估计只统计「有效信号」**：跳过 1920 样本（120ms，= guard 斜坡长度）+ 数字静音（`env < 1.0`），
   累计 960 样本（60ms）有效信号定罪，300ms（index 4800）封顶；门限比 4.0→**3.0**（+9.5dB）。
   桌面端无 guard，但同样跳 120ms 前置区（保持四端逐字一致）。

## 1. 现象与实测数据

「调试会话」页里 StickS3（VS-53A8）的录音整体很轻：截图那段 12.2 s 峰值仅 **-24.6 dBFS**，波形几乎贴着
中线；用户反馈「轻声/耳语识别率降低」，而耳语（嘴贴麦克风）正是本项目主打场景。

（16 kHz 解码，20 ms 帧 RMS；「语音帧」= 高于 p90-12 dB 的帧；「包络」= 与固件同系数的 5 ms/300 ms 峰值跟随器。）

| 分组（按时间） | 段数 | 语音帧 RMS p50 | 峰值 | AGC 后包络 p50 |
|---|---|---|---|---|
| 10-13 17:25–17:26（近场正常说话） | 2 | -24.5 / -26.9 dBFS | 0.0 / -1.0 dBFS | -13.2 / -17.2 dBFS |
| 10-14 07:17 | 1 | -25.3 dBFS | -0.3 dBFS | -15.1 dBFS |
| 10-14 10:07–11:03 | 14 | -33.8 … -48.9 dBFS | -1.1 … -30.9 dBFS | -22.2 … -41.8 dBFS |
| **10-14 22:22–22:28（轻声/耳语）** | **11** | **-37.1 … -58.6 dBFS** | **-20.0 … -45.4 dBFS** | **-29.7 … -52.4 dBFS** |

耳语段比同一设备的近场正常说话低 **20~30 dB**。其中 22:22 session-2（3.0 s，峰值 -37.7 dBFS）**ASR 零输出**
（app 日志同批其余 10 段都有 `Enter pending confirmation`，只有它没有）。

> 注意：调试页频谱按每段自身最大值归一化（`scripts/e2e_test/spectrogram_server.py:229` `spec[i] -= maxDb`），
> 再轻的录音看着也一样亮——只有下方波形是绝对幅度。这也是「看着正常、实际很轻」错觉的来源。

## 2. 根因

现行 AGC（旧 `audio_pipeline.c`）：

```
AGC_TARGET 8192(-12dBFS)  AGC_MAX_GAIN 4.0(+12dB)  AGC_NOISE_FLOOR 184(-45dBFS)
gated = env < 184;  desired = gated ? 1.0 : clamp(8192/env, 0.1, 4.0)
```

- **模式 B（主因）：噪声门把耳语整段挡在门外 ⇒ 增益 = 0 dB。** 耳语经 PGA 18 dB 后的前级包络约
  **-42 ~ -52 dBFS**，几乎全在 -45 dBFS 门限之下，`desired = 1.0`，AGC 一点不放大。
  可复核判据：若真给了 +12 dB，输出包络必然 ≥ `4×184/32768` = **-33.0 dBFS**；而 22:22 session-1/-2
  实测包络 p50 为 **-52.4 / -49.7 dBFS**，低于该下界 16~19 dB ⇒ 实际增益只能 ≈0 dB。
- **模式 A：门限之上被 +12 dB 上限截断。** 包络 -45~-24 dBFS 时输出 = 输入+12 dB，即 -33 ~ -12 dBFS，
  仍差 0~21 dB；22:2x 中较亮的 7 段正落在这一区间。
- 把 -42 ~ -52 dBFS 拉到 -12 dBFS 目标需要 **+30 ~ +40 dB**。
- 更正一处历史注释错误：`AGC_GAIN_GATE = 0.9999961` 注释写「~1s」，按 `exp(-1/(fs·τ))` 实际 **τ≈16 s**；
  `AGC_GAIN_DOWN = 0.73162` 注释写「2ms」实际 **τ≈0.2 ms**。模式 B 的失效与这两个系数无关——它由
  `gated ⇒ desired = 1.0` 直接造成。

这不是 10-14 收紧单点改坏：2026-07-25 的 `software-agc.md` 就是为「声音轻」而做，但目标/上限照正常说话定；
10-14 把上限压到 +12 dB 只是让模式 A 又恶化 14 dB。

## 3. 方案：噪声预算型 AGC v2

把 -45 dBFS 从「门限（不给增益）」改为「**噪声预算（抬到这儿为止）**」：噪声段只允许把本底抬到
-45 dBFS，语音段放开到 +36 dB。本底用冷启动窗估计，门限 = 本底 +12 dB。

### 常量（四端逐字一致）

| 常量 | 旧值 | 新值 | 说明 |
|---|---|---|---|
| `AGC_TARGET` | 8192.0 | 不变 | -12 dBFS |
| `AGC_MAX_GAIN` | 4.0 | **63.0** | +36 dB |
| `AGC_MIN_GAIN` | 0.1 | 不变 | -20 dB |
| `AGC_NOISE_FLOOR` → `AGC_NOISE_BUDGET` | 184.0（门限） | **184.0（预算）** | -45 dBFS：噪声段被放大后的上限 |
| `AGC_ENV_ATTACK/RELEASE` | 0.98758 / 0.99979 | 不变 | 5 ms / 300 ms |
| `AGC_GAIN_UP` | 0.99988 | 不变 | 小误差慢上 500 ms |
| `AGC_GAIN_UP_FAST` | — | **0.9993752** | 大误差（desired > 4×gain）快上 100 ms |
| `AGC_GAIN_DOWN` | 0.73162 | 不变 | 快下（τ≈0.2 ms，注释已更正） |
| `AGC_GAIN_GATE` | 0.9999961 | **删除** | 门内不再强制 desired=1.0 |
| `AGC_NF_ABS` | — | **16.0** | -66 dBFS：门限绝对下限 |
| `AGC_NF_RATIO` | — | **4.0** | 门限 = 本底 +12 dB |
| `AGC_NF_NEAR / NF_RISE` | — | **2.0 / 6.25e-5** | 仅 env < 2×nf 时以 τ=1 s 缓升本底 |
| `AGC_NF_TRUST_MIN / MAX_TRUST` | — | **1.0 / 184.0** | 本底可信区间 [-45dBFS, -90dBFS] |
| `AGC_NF_WARM/SKIP/WARM_MAX` | — | **1920 / 80 / 7680** | 冷启动窗 120 ms（跳过 5 ms 包络爬升，最多读到 480 ms） |

### 逐样本逻辑

```
env = 峰值跟随(|x|)                       // 首样本直接取 |x|，避免包络从 0 爬升污染本底
if (!nf_ready) {                          // 冷启动窗：直通（增益 1.0，仅保留 0.8FS 限幅）
    if (index >= 80) { nf = min(nf, env); if (env >= 1.0) nf_seen = true; }
    if ((index >= 1920 && nf_seen && nf <= 184) || index >= 7680) {
        nf = nf_seen ? clamp(nf, 1.0, 184.0) : 16.0; nf_ready = true;
    }
    index++; return limiter(x);
}
if (env < nf) nf = env;                                   // 瞬时下降
else if (env < nf * 2) nf += (env - nf) * 6.25e-5;        // 仅噪声段缓升
if (nf > 184.0) nf = 184.0;                               // 本底估计上限（防语音污染后压死语音）
gated   = env < max(16.0, nf * 4.0);                      // 门限 = 本底 +12 dB
desired = clamp(8192 / max(env,1), 0.1, 63.0);
if (gated) desired = min(desired, 184.0 / max(env,1));    // 噪声预算：本底最多抬到 -45dBFS
gain = (desired < gain) ? fast_down : (desired > 4*gain ? fast_up_100ms : up_500ms);
y = x * min(gain, 26214/|x|)；clamp 到 int16      // 0.8FS 瞬时限幅（无记忆）
```

设计要点：
- **语音段上限放开到 +36 dB**，耳语前级 -50 dBFS 也能进目标区；
- **噪声段仍有硬上限**：判为噪声时输出被压在 -45 dBFS 以内，嘈杂房间自动退回近 0 dB（不会噪声泵升）；
- **本底靠冷启动窗标定**，因此安静房间门限低（耳语能开）、嘈杂房间门限高（噪声被挡）；
- **不用「门内把增益拉回 0 dB」**：那会让句间停顿后的下一句起音塌下去；冷启动窗直通也让首字不再被
  未标定的本底误判。

## 4. 离线复算（真实录音当输入，Python 逐样本）

耳语组 session-1/-2 的录音本身就是**未被放大的前级信号**（见 §2 模式 B 判据），可直接当耳语测试向量：

| 向量 | 现状语音帧 RMS p50 | AGC v2 | 峰值 | 噪声底(帧 RMS p10) |
|---|---|---|---|---|
| 耳语 22:22 s1 | -58.6 dBFS | **-30.4 dBFS** | -13.4 dBFS | -75.4（更干净） |
| 耳语 22:22 s2 | -55.6 dBFS | **-23.4 dBFS** | -7.9 dBFS | -79.1 |
| 耳语 22:28 s10 | -39.4 dBFS | **-21.9 dBFS** | -8.6 dBFS | -88.2 |
| 耳语 22:28 s11 | -29.1 dBFS | **-20.2 dBFS** | -1.9 dBFS | -68.6 |
| 近场大声 s27 | -24.5 dBFS | -20.1 dBFS | -1.9 dBFS | -56.5 |
| 网关大声 s30 | -23.7 dBFS | -23.3 dBFS | -1.9 dBFS | -41.0 |
| 白噪声 -50dBFS RMS | -38.2 dBFS | **-50.9 dBFS（不放大）** | -38.6 dBFS | -51.4 |
| 耳语 + 噪声 -50dBFS | -37.9 dBFS | **-51.3 dBFS（不放大）** | -38.5 dBFS | -51.9 |

单元向量（三端同值，测试即断言）：轻声型（120 ms 底噪 + 8 段 200 ms 幅度 100 正弦）稳态峰值
**5956**（+36 dB）；白噪声（LCG，RMS 172.7）峰值 299→338、RMS 172.7→98.7（**不放大**）；满幅方波峰值
**26214**（0.8FS 限幅）；`reset()` 幂等、640 样本流式 == 全新实例逐样本一致。

## 5. 改动清单（已落地）

1. 固件 `firmware/components/audio_pipeline/audio_pipeline.c`：常量 + `agc_process` 重写 + 会话复位 +
   日志加 `nf`（每秒 `agc env=… nf=… gain=… dB`）。
2. macOS `desktop/macos/Sources/VoiceStickCore/PcmPostprocessor.swift`（+ 状态/复位）。
3. Windows `desktop/windows/src/pcm_postprocessor.{h,cc}`（+ 状态/Reset）。
4. Python 镜像 `scripts/e2e_test/atvv_capture.py`（`Agc` 重写 + 常量 + 两用例）→
   `scripts/e2e_test/regen_fixture_wav.py` 再生 `fixtures/xiaomi/demo_synthetic/session_1.wav`
   （先断言解码 == 已提交 `session_1.raw.wav` 逐样本一致，通过：31104 样本）。
5. 测试：`Tests/VoiceStickTests/PcmPostprocessorTests.swift`、`desktop/windows/tests/core_tests_codec_serial.cc`
   `TestPcmPostprocessor()`（旧「-40dBFS 拉到 +12dB 上限」用例换为 v2 轻声拉起 + 噪声预算两条）。
6. 文档：`Doc/Plan/software-agc.md` 加 v2 附录；`Doc/Expe/` 记录方法论。

## 6. 验证与真机验收

已验证（本轮）：

- Python `atvv_capture.py --self-test` **17/17**；
- macOS `swift run VoiceStickTests` **928/928**（含 golden fixture 与两条新 AGC 用例）；
- **C++↔Python 逐样本 bit 级对拍一致**（clang++ 编译 `pcm_postprocessor.cc` + 小 main，与 Python 全链
  4 组向量输出哈希相同：`4217994342213201755`）——本机不能跑 MSVC，故 Windows 正式 `ctest` 待 Windows 机；
- 固件 `idf.py build`（ESP-IDF v5.5.1）通过。

待真机（用户配合，唯一权威判据）——`debug_audio_cache = true`，同文本耳语 5 段 + 近场大声 3 段：

1. 耳语：调试页波形明显抬起，语音帧 RMS **-22 ± 3 dBFS**、峰值 -15 ~ -3 dBFS、无 0 dB 平顶；
2. 近场大声：不削波（峰值 ≤ -1.9 dBFS 平顶即限幅生效）、无爆点「砰」声；
3. 语音间隙底噪：安静房间允许抬到 -45 dBFS 量级；嘈杂环境不应比改前更吵；
4. 串口 `agc env=… nf=… gain=…` 应见轻声时 gain 挂到 +25 dB 以上、nf 落在环境底噪；
5. ASR：原来零输出的那种电平稳声会话（-37 dBFS 峰值级）能出文本，字错率主观对比改善。

### 真机验收记录（2026-10-15 20:16，VS-53A8）

| 判据 | 要求 | 实测 | 结论 |
|---|---|---|---|
| 串口 `nf` | 落在真实环境底噪（不是咔哒尾部） | **-73.6 dBFS**（安静房间） | ✅（v2 失效时该值会被钳到 -45dBFS） |
| 串口 `gain` | 轻声时挂到 +25dB 以上 | **+26.6 → +35.0 dB** | ✅ |
| 录音语音帧 RMS | -22 ± 3 dBFS | **-22.1 dBFS** | ✅（修复前 19:24~20:07 同场景 -38…-42 dBFS） |
| 录音峰值 | -15 ~ -3 dBFS，无 0dB 平顶 | **-5.6 dBFS**，无平顶 | ✅ |
| 噪声底（间隙 p10） | 不被明显抬起 | -57.6 dBFS（原 -76 ~ -83） | ✅ 抬到 -58dBFS 量级，远低于语音 |

### 近场大声回归（2026-10-15 20:18，session-2）

| 判据 | 实测 | 结论 |
|---|---|---|
| 峰值 ≤ -1.9 dBFS、无 0 dB 平顶 | 峰值 **-5.8 dBFS**；≥32767 样本 **0** 个、连续平顶段 **0** 段 | ✅ 无削波 |
| 语音帧 RMS 仍收敛到目标 | **-23.7 dBFS** | ✅ 与耳语段 -22.1 dBFS 同档（不同音量同一目标 = AGC 在工作） |
| 间隙底噪未被抬高 | 帧 RMS p10 **-72.3 dBFS**（耳语段 -57.6） | ✅ 大声时增益明显更低，符合预期 |
| ASR | 两段均出文本（76 / 87 字） | ✅ |

> 机制级判据（大声时串口 `gain` 应压到 0 dB 以下）本轮**未采到串口日志**（采集脚本当时已停），
> 上表为录音侧的间接证据：同一房间同一底噪下，大声段的间隙电平比耳语段低 15 dB ⇒ 两段增益不同、
> 大声段未被抬升。如需坐实可再录一段 + 采串口。

仍待补：ASR 字错率主观对比（同文本改前/改后）。

## 7. 风险与回滚

- **噪声泵升**：由预算上限（-45 dBFS）与自适应门限共同兜底；离线合成「耳语+噪声 -40~-50 dBFS」全部
  未被放大（-51 dBFS 输出）。
- **本底被语音污染**：冷启动窗若整段是语音，本底被钳到 -45 dBFS ⇒ 门限 -33 dBFS，正常语音仍能开门，
  只有低于 -33 dBFS 的轻声会被压到「不放大」（安全失败方向：宁可不变响，不误判成噪声放大）。
- **首字**：冷启动窗 120 ms 直通（本机 click guard 本就静音前 120 ms），之后大误差 100 ms 快攻，
  0.8FS 限幅兜底过冲。
- **回滚**：四个常量（`AGC_MAX_GAIN=4.0`、`NOISE_BUDGET`→旧门限语义、删 `AGC_FAST_RATIO/NF_*`）
  即恢复旧行为；小米链路另有 `hpf/agc` 配置开关可整级旁路。

## 附录 A：数据复现口径

```bash
cd "~/Library/Application Support/VoiceStick/DebugAudio"
for f in *.ogg; do ffmpeg -hide_banner -nostats -i "$f" -af volumedetect -f null - 2>&1 | grep volume; done
```

逐样本包络/AGC 复算：16 kHz 单声道解码 → 20 ms 帧 RMS；包络用固件同系数（`ATK=0.98758 / REL=0.99979`）；
「语音帧」取 RMS > p90-12 dB 的帧；AGC v2 复算 = `scripts/e2e_test/atvv_capture.py` 的 `Agc` 类。
