# 音频增益调优：修复近场 ASR 效果差

## 背景

用户反馈：Stick 靠近嘴（<15cm）时 ASR 识别效果反而变差，甚至不如笔记本电脑自带麦克风阵列；
保持 15cm 左右距离效果最佳。怀疑"音频转换过程信噪比放大太多"。

## 根因（基于代码 + 物理，非猜测）

### 证据链

| 环节 | 代码/事实 |
|---|---|
| 麦克风 PGA 增益 | `audio_pipeline.c:191` `esp_codec_dev_set_in_gain(s_codec, 36.0)` → ES8311 **最大档** `ES8311_MIC_GAIN_36DB`（枚举 0/6/12/18/24/30/36），线性 **63 倍** |
| AGC/ALC | **无**。`esp_codec_dev` 未封装 ES8311 硬件 ALC（reg 0x18~0x1B），未启用，增益完全固定 |
| Opus 码率 | `OPUS_BITRATE 20000` = 20 kbps（极低，为 BLE 带宽优化） |

### 物理解释

1. 声压按距离反平方衰减。嘴距 5cm vs 15cm，到达麦的声压强约 **+19 dB**（球面波近似）。
2. 36 dB PGA 本就把 ADC 推到接近满量程，近场再叠加 +19 dB → **ADC 输入过载，硬削波**。
3. 削波是非线性失真，产生大量谐波，破坏语音频谱 → ASR 字错率显著上升。
4. 15cm 恰是"未削波 + 增益够用"的甜点；再近削波，再远信号弱（噪声占优）。

### 对用户直觉的纠正

"信噪比放大太多"方向对了一半。准确说法是**增益过大导致近场硬削波失真**：
- 固定增益**不改变 SNR**（信号与噪声同等放大）
- 问题是增益超出 ADC 线性区，**削波引入新的非线性失真**，反而降低有效可懂度
- 笔记本麦阵列通常有 AGC（近场自动压增益）+ 多麦波束降噪，故近场不削波

## 方案一（本次采纳）

降低固定 PGA 增益 + 提高 Opus 码率，给近场留出 headroom 并改善失真容限。

| 参数 | 原值 | 新值 | 理由 |
|---|---|---|---|
| PGA 增益 | 36 dB | **24 dB** | 比 36 dB 低 12 dB headroom，近场不易削波；仍是中高增益，15-30cm 信号够用；ES8311 标准档 |
| Opus 码率 | 20 kbps | **32 kbps** | 语音质量明显改善，削波失真容限提升；40ms 帧 160 字节，BLE MTU 安全（220 上限 + 11 header = 171 < 244） |

### 涉及文件

- `firmware/components/audio_pipeline/audio_pipeline.c`
  - `#define OPUS_BITRATE 20000` → `32000`
  - `esp_codec_dev_set_in_gain(s_codec, 36.0)` → `24.0`

### 验证

固件无自动化单测。验证方式：`idf.py build` 编译通过 + 真机运行。
真机验证清单：
1. 近场（5cm）说话，ASR 字错率应明显改善（不再削波）。
2. 中场（15cm）效果应保持或略升（原甜点，增益下调后仍够用）。
3. 远场（30cm+）若明显变差，记录现象，触发方案二。

## 方案二（方案一效果不足时启用）

启用 ES8311 硬件 ADC ALC（自动电平控制），自适应不同距离。

- 直接写 reg 0x18（alc enable + winsize）、0x19（alc maxlevel）、0x1A/0x1B（alc automute + hpf）。
- `esp_codec_dev` 未封装，需在 `init_codec` 后通过 `es8311` codec if 的 `set_reg` 或直接 I2C 写寄存器。
- 参数（target level、attack/release time、winsize）需查 ES8311 datasheet 调参，调不当会引入呼吸感（pumping）或底噪放大。
- 触发条件：方案一在近场或远场任一距离识别效果仍不理想。

## 不改动的部分

- I2S 采样率 16 kHz、单声道、40ms 帧：不变。
- Opus APPLICATION_VOIP、COMPLEXITY=1、VBR=0、DTX=0：不变（COMPLEXITY=1 为 CPU 节流，与音质无关）。
- BLE 音频帧协议：不变。
- 桌面端 ASR/解码链：不变。
