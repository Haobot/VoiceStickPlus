# 小米遥控器直连桌面链路补 HPF + AGC（音质/ASR 修复）

2026-10-13。状态：已实施（Python 镜像 + fixture 再生、macOS 全绿 816/816、Windows 代码就绪待 Windows 机 ctest 复验）。

## 背景与根因

「调试会话」页（`scripts/e2e_test/spectrogram_server.py`）播放的 `.ogg` 是送 ASR 的同一条流
（桌面端在 Ogg muxer 出口落盘，`debug_audio_cache` 开关）。用户反馈小米遥控器直连桌面时
音质差、近场爆破音多、ASR 准确率低。

根因：小米**直连桌面**链路（桌面端 ATVV 会话内 ADPCM→PCM→Opus 归一化）只有
「三点平滑 + 固定 +12dB 增益 + int16 硬限幅」，缺固件链路已验证的两道处理：

| 处理 | 固件链路（含网关模式中继） | 小米直连桌面链路 |
|---|---|---|
| 二阶 Butterworth HPF fc≈90Hz（去 20–100Hz 爆破音） | 有（`firmware/components/audio_pipeline/audio_pipeline.c:55-78`） | 无 |
| 软件 AGC（-6dBFS 目标、快下慢上、噪声门、0.8FS 瞬时限幅） | 有（`audio_pipeline.c:80-153`） | 无（固定增益硬削波） |

爆破音低频能量无 HPF 滤除，经 +12dB 固定增益后 int16 饱和削波 → 宽带谐波失真 → ASR 特征被毁；
轻声/远场又电平不足。网关模式音频过固件 HPF+AGC，可作 A/B 对照。

## 目标 / 非目标

- 目标：直连桌面链路音质与固件链路拉齐——去爆破音、防削波、电平归一，提升 ASR 准确率。
- 非目标：不改协议、不改固件、不动 StickS3 链路；不调 Opus 编码参数（32kbps CBR 与固件一致）；
  ADPCM 4-bit 固有量化噪声不在本次范围。

## 算法规格（Swift / C++ / Python 三端逐字一致）

处理链（`PcmPostprocessor.Process` 内部顺序）：

```
smooth3（整帧三点平滑，首尾样本不动，现状保留）
→ HPF（逐样本）→ 固定增益 gain_db（逐样本）→ AGC（逐样本，含瞬时限幅）
```

每一级输入输出都是 int16（lround + clamp 量化，与固件一致；级间量化保证三端 bit 级对齐）。
HPF 放在固定增益**之前**：先削爆破低频能量，避免 +12dB 把爆破音推进饱和区。

### HPF（transposed DF-II biquad，double）

系数逐字拷贝固件 `audio_pipeline.c:61-65`（fc≈90Hz @16kHz，Q=0.707）：

```text
b0 = 0.975318   b1 = -1.950637   b2 = 0.975318   a1 = -1.950028   a2 = 0.951246
y  = b0*x + z1
z1 = b1*x - a1*y + z2
z2 = b2*x - a2*y
out = clamp(lround(y))
```

参考频响（固件已实测验证）：90Hz -3.0dB / 50Hz -10.6dB / 30Hz -19.1dB / ≥300Hz ~0dB。
状态：`z1`、`z2`，会话开始清零。

### AGC（double；固件为 float，桌面端统一 double 保证 C++↔Python 对拍确定）

常量逐字拷贝固件 `audio_pipeline.c:92-100`：

```text
TARGET = 16384.0        # -6 dBFS
MAX_GAIN = 10.0         # +20 dB
MIN_GAIN = 0.1          # -20 dB
NOISE_FLOOR = 184.0     # -45 dBFS
ENV_ATTACK = 0.98758    ENV_RELEASE = 0.99979
GAIN_UP = 0.99988       GAIN_DOWN = 0.73162    GAIN_GATE = 0.9999961
```

逐样本逻辑（与固件 `agc_process` 相同）：

```text
ax = |x|
ec = ax > env ? ENV_ATTACK : ENV_RELEASE
env = ec*env + (1-ec)*ax
gated = env < NOISE_FLOOR
desired = gated ? 1.0 : clamp(TARGET/env, MIN_GAIN, MAX_GAIN)
gc = gated ? GAIN_GATE : (desired < gain ? GAIN_DOWN : GAIN_UP)
gain = gc*gain + (1-gc)*desired
g = gain
if ax > 1: g = min(g, 26214.0/ax)     # 0.8FS 瞬时限幅（无记忆，不回写 gain）
out = clamp(lround(x * g))
```

状态：`env`（初 0）、`gain`（初 1），会话开始复位。选 0.8FS 上限的理由（Opus 解码
+1.5~2dB 过冲余量）见固件注释 `audio_pipeline.c:126-129`。

### 开关与复位

- `hpf` / `agc` 两个 bool 开关（默认 `true`）：关闭时该级为直通（不动状态），
  用于逃生与测试隔离。
- 复位点 = 每次录音会话流开始，与既有 decoder/accumulator/slicer 复位同点：
  - macOS：`XiaomiAtvvSession.hardResetStream()`（`XiaomiAtvvSession.swift:325-331`）
    与 `stop()`（`:126-145` 复位块内）各加 `postprocessor.reset()`。
  - Windows：`XiaomiAtvvSession::HardResetStream()`（`xiaomi_atvv_session.cc:288-294`）
    与 `Stop()`（`:35-` 复位块内）各加 `postprocessor_.Reset()`。
- AUDIO_SYNC 途中重同步（macOS `:194-201` / Windows `:119-131`）**不**复位 HPF/AGC
  （只复位解码器链路，滤波/增益状态保持连续）。

## 改动清单

### 1. macOS

- `Sources/VoiceStickCore/PcmPostprocessor.swift`
  - 类增加私有状态 `hpfZ1/hpfZ2/agcEnv/agcGain`；常量按上文规格（`private static let`）。
  - 构造器改为 `init(gainDb: Double = 0.0, hpf: Bool = true, agc: Bool = true)`；
    增加只读属性 `hpfEnabled`/`agcEnabled`。
  - `process(_:)`：先整帧三点平滑（现状逻辑保留），再逐样本 `hpf → gain → agc`
    （各级 int16 量化；开关关闭则跳过该级）。
  - 新增 `reset()`：清零 HPF/AGC 状态。
  - 更新类注释（三点平滑+增益 → 完整链）。
- `Sources/VoiceStickCore/XiaomiAtvvSession.swift`
  - `Options`（:41-53）加 `hpf = true`、`agc = true` 两字段（含 init 参数）。
  - `init`（:96-100）构造 `PcmPostprocessor(gainDb:hpf:agc:)` 透传。
  - `hardResetStream()`（:325）与 `stop()`（:120）加 `postprocessor.reset()`。
- 配置解析/序列化
  - `Sources/VoiceStickCore/ConfigFileModels.swift`：`XiaomiConfigFile`（:170-196）
    加 `hpf: Bool?`、`agc: Bool?`（普通 Bool 解码即可，无需 LenientDouble）；
    `xiaomiSettings(from:fallback:)`（:208-215）应用两字段。
  - `Sources/VoiceStickCore/ConfigParsing.swift` `XiaomiSettings`（:37-49）加
    `hpf = true`、`agc = true`（Equatable 自动覆盖）。
  - `Sources/VoiceStickCore/AppConfig.swift`：`xiaomiSettings(for:)`（:725-731）
    返回元组加 `hpf/agc`；序列化模板（:832-840）加 `hpf = …` / `agc = …` 两行。
  - 透传：`AppDelegate.swift:372`、`:719`、`BleCentral.swift:1286` 构造
    `XiaomiAtvvSession.Options` 处带入新字段。
- `desktop/macos/Config/config.example.toml:97-102`：注释块补 `hpf`/`agc` 说明与示例行。

### 2. Windows（镜像）

- `src/pcm_postprocessor.h/.cc`
  - `PcmPostprocessor` 加 `hpf_z1_/hpf_z2_/agc_env_/agc_gain_` 与常量（`static constexpr double`）。
  - 构造器 `PcmPostprocessor(double gain_db = 0.0, bool hpf = true, bool agc = true)`；
    `Process()` 去掉 `const`（有状态），链式处理同 macOS。
  - 新增 `void Reset()`。
- `src/xiaomi_atvv_session.h/.cc`
  - `Options`（`xiaomi_atvv_session.h:77-91`）加 `bool hpf = true; bool agc = true;`。
  - 构造函数（`xiaomi_atvv_session.cc:24`）透传给 `postprocessor_`。
  - `HardResetStream()`（:288-294）与 `Stop()`（:35-）加 `postprocessor_.Reset()`。
- 配置解析/序列化
  - `src/app_config.h:139` 附近：`XiaomiSettings` 加 `bool hpf = true; bool agc = true;`；
    若 `operator==` 为手写（`app_config.cc:1137` 相等跳过落盘依赖它），同步加两字段比较。
  - `src/app_config.cc:554-563` `ParseXiaomiSettings`：加两行 `TomlBool(table,"hpf"/"agc")`。
  - `src/app_config.cc:1132-1154` 序列化：注释「全量写出 3 个字段」改 5 个，补
    `hpf = true/false`、`agc = true/false` 两行（保证「遥控器设置」对话框保存后新键不丢）。
  - `src/win32_app.cc:580` 附近：构造 `Options` 带入 `xiaomi.hpf/agc`。
- 「遥控器设置…」对话框（`remote_settings_dialog.cc`）UI 不改（只编辑 gain_db/
  double_click_ms；新键经序列化全量写回保留）。

### 3. Python 镜像 + golden fixtures（强制，否则三端对拍红）

- `scripts/e2e_test/atvv_capture.py`
  - `smooth3`（:347）/`apply_gain`（:359）保留不动；新增 `hpf_90hz(samples)` 与
    `agc_process(samples)`（常量、lround half-away-from-zero 与 `apply_gain` 同一 helper）。
  - 新增组合入口 `postprocess(samples, gain_db, hpf=True, agc=True)` =
    `agc_process(apply_gain(hpf_90hz(smooth3(samples))))`（按开关跳过）。
  - 落盘点 `:500` 改为 `postprocess(self.pcm_samples, self.args.gain_db, …)`；
    CLI 加 `--no-hpf` / `--no-agc`（A/B 采集用）。
  - `self_test()`（:853 起）：新增 HPF（直流衰减、50Hz vs 1kHz 幅度比）与 AGC
    （轻声拉起、0.8FS 上限）断言；`:1011-1013` 的管线复现断言改用 `postprocess`。
- `scripts/e2e_test/atvv_bench.py:204-205`、`:314`：改用 `postprocess`，保证 bench 反映新链。
- 再生 fixture：`scripts/e2e_test/fixtures/xiaomi/demo_synthetic/session_1.wav`。
  用一段临时脚本 `from atvv_capture import …`：解码 `session_1.adpcm` →
  **先断言与已提交 `session_1.raw.wav` 逐样本一致**（验证解码分帧正确）→
  按 sidecar `session_1.json` 的 `gain_db` 跑新 `postprocess` → 覆写 `session_1.wav`。
  `.adpcm` / `.raw.wav` / sidecar 均不动。

### 4. 测试更新

- macOS `Tests/VoiceStickTests/PcmPostprocessorTests.swift`
  - 旧用例（三点平滑/增益/钳位/限幅）构造改 `PcmPostprocessor(gainDb:hpf:false,agc:false)`，
    断言语义不变，继续守旧路径。
  - 新增：
    - HPF 直流衰减：1000 个恒定 5000 输入，末 100 样本 |out| < 50。
    - HPF 频响：10000 幅度 50Hz 与 1kHz 正弦各 1s，稳态 RMS 比 ≥ 10dB 衰减。
    - AGC 拉起：-40dBFS（幅度 ≈328）1kHz 正弦 2s，稳态输出幅度 ≈16384 ±20%。
    - 瞬时限幅：满幅（±32000）突发起音，|out| ≤ 26215。
    - `reset()`：同一输入序列跑两遍（中间 reset）输出逐样本一致。
- Windows `tests/core_tests_codec_serial.cc` `TestPcmPostprocessor()`（:146-172）：
  同样改造（旧用例加 `/*hpf=*/false, /*agc=*/false`）+ 对等新增用例；
  golden 对拍（:96-105）保持默认构造（hpf/agc 开），依赖第 3 步再生 fixture 转绿。
- macOS `ImaAdpcmDecoderTests.swift:185-200` golden 对拍同为默认构造，依赖同一再生 fixture。

### 5. 文档

- `Doc/Ref/desktop-config.md:84-101`：小节标题「仅 Windows 消费」已过时（macOS 同样消费），
  改为双端；字段表加 `hpf`/`agc`（bool，默认 `true`，直连链路高通去爆破音 / 自动增益控制），
  示例补两行。
- `Doc/Agent/desktop-architecture.md`：pcm_postprocessor「三点平滑+增益限幅」描述更新为新链。
- `Doc/Ref/protocol.md` ATVV 章节（:681-685 附近）若描述桌面端后处理链，同步一句。

### 6.（可选顺手）Windows 调试录音文件名设备前缀

`src/debug_audio_recorder.cc:58-67` 硬编码 `-VS-` 前缀，小米会话落盘无法从文件名区分
（macOS 已按设备类用 `RC-`）。对齐 macOS 参数化前缀，便于调试会话页辨认。

## 验证

1. Python：`python3 scripts/e2e_test/atvv_capture.py --self-test` 全绿。
2. macOS：`cd desktop/macos && VOICESTICK_ARCHS=arm64 swift build && swift run VoiceStickTests`
   全绿（含 golden fixtures）。
3. Windows（本机 macOS 无法构建，须在 Windows 机执行）：
   `build_win.bat` + `ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿，
   重点看 ATVV golden fixtures 与 TestPcmPostprocessor。
4. 真机 A/B（用户配合）：`debug_audio_cache = true`，同环境近场录音改前/改后各一段，
   调试会话页对比：频谱 0–100Hz 爆破能量显著消失、波形不再平顶削波；ASR 准确率主观对比。
   可用 `--no-hpf --no-agc` 采集做同源对照。

## 风险与回滚

- 行为变更仅限小米直连模式三条消费链（ASR / 微信输入法 / 字幕），共用 PcmPostprocessor 出口。
- `gain_db` 保留兼容，语义变为 HPF 后、AGC 前的固定预增益。
- 漏复位会导致跨会话滤波/增益状态泄漏（第二次按键音质劣化）——复位点已在两个会话硬重置
  函数内，并有 `reset()` 幂等测试守护。
- 浮点确定性：常量逐字拷贝、全程 double、lround half-away-from-zero，C++↔Python fixture
  对拍 bit 级一致；Swift 无 fixture 对拍，算法等价即可。
- 回滚：`hpf = false` + `agc = false` 即恢复旧行为（平滑+固定增益），无需降级版本。
