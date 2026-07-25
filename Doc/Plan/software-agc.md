# 软件 AGC：录音电平自动均衡

## 背景与症状

用户反馈：麦克风离嘴很近、说话声音轻，录到的音频电平很低且不稳定。

实测证据（2026-07-25 上午 6 段真实录音，debug 音频 volumedetect）：

| 会话 | 峰值 dBFS | 均值 dBFS |
|---|---|---|
| session-1 | -0.0（削波） | -31.6 |
| session-2 | -14.4 | -32.8 |
| session-3 | -2.6 | -33.7 |
| session-4 | -0.9（近削波） | -36.5 |
| session-5 | -23.5（过轻） | -39.6 |
| session-6 | -16.5 | -38.0 |

电平散布高达 23dB：大声近场时硬件 ALC 没压住（削波到 0dB），轻声时又没拉到
-11dBFS 目标（差 12dB）。ES8311 硬件 ALC（`Doc/Plan/es8311-alc-bitfield-fix.md` 修复后
已确认使能）两头都没守住，且行为不透明、无法打日志观测增益，继续调寄存器信心不足。

电平控制链路的唯一控制点在固件（ES8311 PGA + ALC → Opus → BLE → Windows；桌面端
ASR 直接吃 Ogg Opus 不解码，微信虚拟麦路径解码/重采样全线性，无软件增益）。
因此均衡必须落在固件。

## 方案：Opus 编码前的软件 AGC（用户已确认）

在 `audio_pipeline.c` 现有软件 HPF 之后、Opus 编码之前插入自研 AGC，对 mic 采集与
test_playback 回放两条来源统一生效。同时**关闭硬件 ALC**，避免两级 AGC 互相拉扯产生
pumping。PGA 保持 18dB（模拟防削波余量，软件 AGC 无法修复 ADC 硬削波）。

### 算法（单声道 int16，16 kHz，逐样本）

1. 包络跟随：对 |x| 一阶峰值包络，快攻 5ms / 慢释 300ms。
2. 期望增益：`desired = clamp(target / env, 0.1, max_gain)`，
   target = 0.5 满幅（-6dBFS），max_gain = 10（+20dB）。
3. 噪声门：env < -45dBFS 时不继续加增益，增益以约 1s 时间常数缓慢回落 0dB，
   避免静音段抬底噪，也避免句间停顿后增益骤降导致下一句起音偏轻。
4. 增益平滑：增益上升慢（约 500ms，防 pumping）、下降快（约 2ms）。
5. 瞬时峰值限幅：逐样本保证 `|x|*gain <= 0.8FS`（-1.9dB，无记忆、不回写增益状态），
   压住「增益挂在高位时突发起音」头几毫秒的过冲——首轮真机验证发现仅靠 2ms
   增益下降会在起音瞬间削波到 0dB，故加此兜底。上限取 0.8FS 而非贴近满幅：
   二轮真机验证（0.95FS 上限）发现 Opus 解码有 +1.2dB 过冲越界（astats Max level
   1.012、Flat factor 0，非削波平项），0.8FS 预留了过冲余量。
6. 输出逐样本 clamp 到 int16。

### 固件寄存器变更

- REG18（0x18）：`0x83` → `0x03`（ALC_EN=0，automute 关，winsize 无意义保持 3）。
- 删除 REG19（0x19）/ REG1A（0x1A）的写入（ALC 关闭后无意义）。
- PGA 18dB、REG17 默认（0xBF，≈0dB ADC 数字音量）不动。

### 可观测性

每秒节流 `ESP_LOGI` 输出当前包络电平（dBFS）与增益（dB），串口日志直接可见 AGC
工作状态；配合 debug 音频 volumedetect、频谱页（`scripts/e2e_test/spectrogram_server.py`）
和回放工具（`scripts/e2e_test/replay_tencent_asr.py`）验证。

### 不做（YAGNI）

- 不加 BLE 运行时调参、不加配置项：参数固化在代码里，真机调好再议。
- 不动 Opus 参数、BLE 协议、桌面端。
- 不改 PGA：18dB 是历史调出的近场防削波甜点。

## 涉及文件

- `firmware/components/audio_pipeline/audio_pipeline.c`：
  - HPF 之后新增 AGC 处理（主录音循环与 drain 循环两处调用点）；
  - 会话开始时（`s_hpf_z1/z2` 清零处）同步复位 AGC 状态；
  - `init_codec()` 中 ALC 寄存器写入改为关闭，注释更新。
- `Doc/Plan/es8311-alc-bitfield-fix.md`：顶部加注「已被本文取代（硬件 ALC 已关闭）」。

## 验证

固件无自动化单测，验证方式：`idf.py build` 编译通过 + 真机运行。

真机验证清单：

1. **轻声说话（当前主要场景）**：debug 音频峰值应收敛到约 -6dBFS ±3dB，无削波；
   串口日志可见增益拉到 +10~+20dB。
2. **正常/大声近场**：不削波，增益自动压到 0dB 以下。
3. **静音段**：底噪不被明显抬高（增益缓慢回落，不锁定在高档）。
4. **回归**：ASR 连续识别多次，确认轻声场景识别稳定性改善、正常场景不变差。

## 回退

改动集中在一个文件：恢复 REG18=0x83/REG19=0x80/REG1A=0x00 三处写入，删除 AGC
调用即可回到纯硬件 ALC 状态。
