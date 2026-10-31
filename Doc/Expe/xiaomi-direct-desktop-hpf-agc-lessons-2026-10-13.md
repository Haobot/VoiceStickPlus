# 小米直连桌面链路 HPF+AGC 实施：跨端 bit 对拍手法与方案期望值自洽校验

日期：2026-10-13（当日完成实施、全量验证并提交 `fd81ec3`）
相关文件：`desktop/macos/Sources/VoiceStickCore/PcmPostprocessor.swift`、`desktop/windows/src/pcm_postprocessor.{h,cc}`、`scripts/e2e_test/atvv_capture.py`、`scripts/e2e_test/atvv_bench.py`、`scripts/e2e_test/regen_fixture_wav.py`、`scripts/e2e_test/fixtures/xiaomi/demo_synthetic/session_1.wav`
方案：`Doc/Plan/xiaomi-direct-desktop-hpf-agc.md`（同 commit）
相关 commit：`fd81ec3`

## 背景与成果

小米遥控器**直连桌面**链路（ATVV 会话内 ADPCM→PCM→Opus 归一化）原先只有「三点平滑 + 固定 +12dB 增益 + int16 硬限幅」，缺固件链路已验证的 HPF(90Hz) 与软件 AGC。爆破音 20–100Hz 低频经 +12dB 推进饱和区削波 → 宽带谐波失真毁 ASR 特征；轻声电平不足。

实施（算法逐字移植 `firmware/components/audio_pipeline/audio_pipeline.c:55-153`，级间 lround+clamp 量化回 int16）：

```
ADPCM 解码 → 三点平滑 → HPF(90Hz) → 固定增益 gain_db → AGC(含 0.8FS 限幅) → Opus
```

- HPF 放在固定增益**之前**（先削爆破低频再放大，防饱和）。
- `hpf`/`agc` bool 开关默认开，沿 `gain_db` 既有配置链路透传（`[device.<id>.xiaomi]`，Windows 序列化全量写出 5 字段防「遥控器设置」对话框保存丢键）；关闭即回滚旧行为。
- 复位点 = 会话 `hardResetStream()`/`Stop()`（与 decoder 同点）；AUDIO_SYNC 途中复同步**不**复位滤波状态。
- 五类新测试三端对齐：直流衰减、50Hz vs 1kHz 频响比、AGC 拉起、满幅瞬时限幅 ≤26215、reset 幂等。

验证状态（**不伪造**）：Python self-test 16/16 + bench 8/8（python3.9 与 3.11 双跑）；macOS `swift build` + `VoiceStickTests` 816/816 全绿（golden fixture 与再生 wav bit 级一致）；Windows 端本机**无法构建**，用 clang 交叉验证替代（见下节），正式 `build_win.bat` + ctest **待 Windows 机**；真机 A/B 待用户配合。

## 经验一：Windows 机缺席时的交叉验证手法（本类任务可复用）

本机 macOS 不能跑 `build_win.bat`，但 Windows core 里的**纯逻辑 .cc 无 Win32 依赖**，可直接用 macOS clang++ 编译做实质预验证：

```bash
# 语法检查
clang++ -std=c++20 -fsyntax-only -Idesktop/windows/src desktop/windows/src/pcm_postprocessor.cc
# golden 对拍：编译 pcm_postprocessor.cc + ima_adpcm_decoder.cc + 小 main，
# 读 session_1.adpcm 按 sidecar 解码 → 默认构造 PcmPostprocessor(gain_db) 处理
# → 与 Python 再生的 session_1.wav 逐样本比（本例 31104 样本 bit 级一致）。
```

本机实测有效，检出力等同 ctest 对拍 2。三个 clang/MSVC 差异边界（**不是本次改出来的，预存**）：

- `xiaomi_atvv_session.h` 的 `explicit XiaomiAtvvSession(Options options = {})`：clang 严格报「default member initializer needed within definition of enclosing class」（嵌套 struct 默认成员初始化用于外层类默认实参），MSVC 接受——该文件**不能**用本机 clang 做语法检查，勿误判为自己改坏。
- `debug_audio_recorder.cc` 的 `localtime_s` 是 MSVC 专属（POSIX 是 `localtime_r`），本机编译必败，正常。
- `Process({1000})` 这种 initializer_list 喂 `std::span<const int16_t>` clang 拒绝（`{1000}` 推导为 `int` 列表）；Windows 测试文件用显式 `std::vector<std::int16_t>{1000}` 无此问题——临时验证脚本里注意。

## 经验二：方案/测试期望值必须先与算法常量对算一遍

方案原文写「AGC 拉起：-40dBFS 输入，稳态输出幅度 ≈16384（-6dBFS）±20%」。但 `MAX_GAIN = 10.0`（+20dB）决定 -40dBFS（幅度≈328）输入**数学上只能到 ≈3280（-20dBFS）**——目标 -6dBFS 需 +34dB，超上限。若照抄方案写三端断言，全部按错误基线实现，且「修正算法去满足期望值」是错的方向（常量必须逐字拷固件）。

判据/做法：实施任何带数值阈值的方案前，先用常量手工推算一遍稳态值（本次 AGC：`gain(n) = desired + (1-desired)·exp(-n/8000)`，2s 后 ≈9.84，输出 ≈3226），再定断言区间。方案文档有少量笔误是常态，常量与机制才是权威。

## 经验三：DSP 链测试注意「process() 调用粒度」语义

第一版 Swift/C++ 测试写「640 样本帧流式输出 == 整段一次处理输出」，必红：三点平滑作用在**单次 process 调用内部**，整段处理时第 640k 边界的邻居是真实样本，分帧处理时是帧首/尾样本（不动），且 HPF/AGC 递归状态会把边界差异向后传播，**边界后不存在严格相等的样本区间**。

正确断言：「reset() 后流式 == 全新实例流式」（验证状态清零语义）。另注意生产路径本来就是按 640 帧切片喂 postprocessor（OpusFrameSlicer 出口），所以**流式喂法才是生产语义**，整段处理只存在于 golden 对拍里（Python 镜像同样按整段，两端一致即可对拍）。

## 经验四：fixture 再生纪律（本次实操确认有效）

`scripts/e2e_test/regen_fixture_wav.py`（已入库）：再生 `session_N.wav` 前**先断言按 sidecar 段落解码与已提交 `session_N.raw.wav` 逐样本一致**——这一步验证了「解码分帧路径没写错」，再跑新链覆写 wav。少了这步，解码侧回归会静默混进 fixture。`.adpcm`/`.raw.wav`/sidecar 一律不动。

## 教训（顺带的存量坑）

- `scripts/e2e_test/asr_bench/report.py:121` 的 f-string 表达式内反斜杠（`replace('|', '\\|')`）是 **Python ≥3.12 专属语法**，本机 3.9/3.11 下 `atvv_bench.py --self-test` 连 import 都过不了（已修为提取临时变量，行为不变）。教训：仓库脚本的实际运行 Python 版本不可假设，改 bench 链路前先跑一次 self-test 确认基线可跑。
- 已跟踪但处于 gitignore 路径下的文件（`desktop/windows/` 全目录），`git add <文件>` 也会被拒（"paths are ignored"），必须 `git add -f` + `git ls-files` 逐个验证——存量 M 文件同理，不是只有新文件需要。

## 长期技术记忆

- 三端 bit 级对齐的硬约束是「每一级输出量化回 int16 + lround half-away-from-zero」；Swift 用 `.rounded()`（默认 .toNearestOrAwayFromZero）、C++ 用 `std::lround`、Python 用 `floor(v+0.5)/-floor(-v+0.5)` helper（`atvv_capture.lround`，与 `apply_gain` 共用）。任一端改成 float 级联输出，golden 对拍即红。
- AGC 0.8FS 瞬时限幅是无记忆的（不回写增益状态），上限 26214 留 Opus 解码 +1.5~2dB 过冲余量——桌面端复用同一结论。
- 桌面端 AGC 统一 double（固件是 float）：C++↔Python 对拍要求确定舍入，float 中间态可能引入 ULP 级差异。

## 遗留 / 观察项

- Windows 正式构建 + ctest（重点 `TestPcmPostprocessor` 与 ATVV golden fixtures）**待 Windows 机**；本机 clang 交叉验证已过不能替代 MSVC/Win32 全量。
- 真机 A/B：同环境近场录音改前/改后各一段（`debug_audio_cache = true`），判据 = 调试会话页频谱 0–100Hz 能量显著消失、波形不再平顶；`--no-hpf --no-agc` 可做同源对照。
- `gain_db` 语义已变为「HPF 后、AGC 前的固定预增益」；旧配置值不变，但调参手感与旧版不同（AGC 会接管电平）。

## 2026-10-14 追加：参数收紧（喷麦/底噪治理）

本文 §链路规格 与正文中的 HPF 90Hz、AGC -6dBFS/+20dB 为 2026-10-13 实施时点值，**已于 2026-10-14 被取代**：HPF 上调 120Hz、AGC 目标 -12dBFS、上限 +12dB（网关模式真机录音爆点/底噪分析驱动，现行常量以 `Doc/Plan/software-agc.md` 附录与源码为准）。Python 镜像类名随之改为 `Hpf120Hz`；golden fixture 已按新常量重生成，三端对拍纪律与本文经验不受影响。
