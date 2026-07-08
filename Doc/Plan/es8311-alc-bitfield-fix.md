# ES8311 ALC 位域修复：让自动电平控制真正生效

## 背景与症状

用户反馈：手持麦克风距离太近，或说话声音太小，ASR 识别精准度反而下降。

此前已有 `Doc/Plan/audio-gain-tuning.md`（方案一降 PGA 36->24dB + Opus 20->32kbps；方案二启用 ES8311 硬件 ALC）。方案二号称已实现并真机验证生效，但本次核查发现**方案二的 ALC 寄存器位域写反，ALC 从未真正使能**。

## 根因（基于权威代码证据，非猜测）

### 证据链

| 环节 | 证据 |
|---|---|
| 当前代码写入 | `audio_pipeline.c:197` `esp_codec_dev_write_reg(s_codec, 0x18, 0x23)` |
| `0x23` 二进制 | `0b0010_0011` |
| **权威位域** | Linux 主线 `sound/soc/codecs/es8311.h`：`ES8311_ADC4_ALC_EN_SHIFT = 7`（ALC 使能在 **bit[7]**），`ES8311_ADC4_ALC_WINSIZE_SHIFT = 0`（winsize 在 **bit[3:0]**） |
| 结论 | `0x23` 的 bit[7]=**0**，**ALC 未使能**；bit[3:0]=3 只是设了 winsize（但 ALC 没开，无意义） |

仓库内 `Doc/Plan/audio-gain-tuning.md` 与 `es8311_reg.h` 注释都按"bit[7:4]=winsize、bit[3]=ALC enable"理解，与 Linux 主线权威定义相反。**位域搞反使 ALC 自启用以来一直处于关闭状态**，当前真正生效的只有 PGA=24dB 固定增益。

### 物理解释

固定 24dB PGA 无自适应，出现两头塌：

- **近场（<15cm）**：声压大，24dB 把 ADC 推到接近满量程 -> 硬削波，谐波失真 -> ASR 字错率上升。
- **远场 / 小声**：信号弱，固定增益拉不起来 -> SNR 不足 -> ASR 下降。

ALC 本应"近场压、远场拉"，扩大甜点窗口，但它没工作，故两端都变差。这与用户症状完全吻合。

### 链路确认：电平唯一控制点在固件 codec

微信输入法模式全链路：`ES8311 PGA+ALC -> Opus 编码 -> BLE -> Windows Opus 解码 -> PcmRingBuffer -> WASAPI(AUTOCONVERTPCM 重采样) -> CABLE Input`。

经核查 `audio_opus_decoder.cc`、`wasapi_virtual_mic_renderer.cc`、`voice_stick_coordinator.cc`，**Opus 编解码、ring buffer、WASAPI 重采样全为幅度线性，无任何软件 AGC/gain/normalize**。AUTOCONVERTPCM/SRC_DEFAULT_QUALITY 是线性重采样，不改电平。故电平的唯一控制点就是固件 ES8311 的 PGA + ALC，修复必须落在此处。

## 权威位域（来源：Linux 主线 es8311.h / es8311.c）

| 寄存器 | 地址 | 位域 | 来源宏 |
|---|---|---|---|
| ADC4 | 0x18 | bit[7]=ALC_EN, bit[6]=AUTOMUTE_EN, bit[3:0]=ALC_WINSIZE | `ES8311_ADC4_ALC_EN_SHIFT=7` 等 |
| ADC5 | 0x19 | bit[7:4]=ALC_MAXLEVEL, bit[3:0]=ALC_MINLEVEL | `ES8311_ADC5_ALC_MAXLEVEL_SHIFT=4` 等 |
| ADC6 | 0x1A | bit[7:4]=AUTOMUTE_WS, bit[3:0]=AUTOMUTE_NG | `ES8311_ADC6_*` |

`level_tlv`（maxlevel/minlevel 共用）dBFS 映射（节选）：

| 编码值 | 0 | 1 | 2 | 3 | 4 | 6 | 8 | 10 | 12 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|
| dBFS | -3010 | -2410 | -2060 | -1810 | -1610 | -1320 | -1100 | -930 | -780 | -600 |

- **maxlevel**：ALC 压缩的目标电平上限（输出被压向此电平，防削波）。越大输出越饱满但越接近削波边界。
- **minlevel**：ALC 拉起增益的下限电平（信号低于此则放大，拉起小声）。越小（0）远场拉得越多。
- **winsize**：响应窗口（枚举 0..15，越大响应越慢但越平滑）。0="0.25dB/2LRCK"（最快），3="0.25dB/32LRCK"（短响应）。

HPF（高通滤波）在 `ES8311_ADC_REG1B`(0x1B) bit[5]（`ES8311_ADC8_HPF_SHIFT=5`）。es8311 open 写 REG1B=0x0A、REG1C=0x6A。**我们只写 0x1A，不碰 0x1B/0x1C**，HPF 不受影响。

## 方案（用户已确认：ALC 修复 + 重调 PGA/target）

### 参数变更

| 参数 | 原值 | 新值 | 理由 |
|---|---|---|---|
| PGA | 24 dB | **18 dB** | 再降 6dB headroom，配合 ALC 近场防削波；ALC 负责远场拉起补偿，PGA 无需太高 |
| REG18 (0x18) | `0x23`（ALC 未使能） | **`0x83`** | bit[7]=1 使能 ALC，bit[6]=0 不开 automute，bit[3:0]=3 winsize=3（短响应） |
| REG19 (0x19) | `0x30`（target≈-18dBFS） | **`0x80`** | maxlevel=8（≈-11dBFS，比原 -18 更饱满但仍在安全区），minlevel=0（-3010，远场拉到最低） |
| REG1A (0x1A) | `0x00` | `0x00`（不变） | automute 关，避免误判停顿为静音；不动 HPF |

### 二进制核对

- `0x83` = `0b1000_0011`：bit[7]=1 ✓ ALC_EN，bit[6]=0，bit[3:0]=0011=3 ✓ winsize
- `0x80` = `0b1000_0000`：bit[7:4]=1000=8 ✓ maxlevel，bit[3:0]=0000=0 ✓ minlevel

### 涉及文件

`firmware/components/audio_pipeline/audio_pipeline.c` `init_codec()`：

- `esp_codec_dev_set_in_gain(s_codec, 24.0)` -> `18.0`
- `esp_codec_dev_write_reg(s_codec, 0x18, 0x23)` -> `0x83`
- `esp_codec_dev_write_reg(s_codec, 0x19, 0x30)` -> `0x80`
- `0x1A` -> `0x00`（不变）
- 同步更新三处注释的位域说明与参数值

### 不改动

- I2S 16kHz/单声道/40ms 帧、Opus VOIP/CBR 32kbps/COMPLEXITY=1/DTX=0：不变。
- BLE 音频帧协议、Windows 解码/渲染链：不变。
- REG1B/REG1C：不写，保留 es8311 open 默认（HPF 等）。

## 验证

固件无自动化单测。验证方式：`idf.py build` 编译通过 + 真机运行。

真机验证清单（用微信输入法模式或调试音频缓存回放核查）：

1. **近场（5cm）大声说话**：应不再削波，ASR 字错率应优于现状。
2. **中场（15cm）**：效果应保持或改善。
3. **远场（30cm+）/小声说话**：应被 ALC 拉起，识别改善（minlevel=0 允许最大拉起）。
4. **持续说话无呼吸感（pumping）、无底噪异常放大**：若出现 pumping 调大 winsize（如 0x84..0x87）；若底噪被放大调高 minlevel（REG19 低 4 位）。
5. **回归**：正常距离识别不应变差。

若近场仍削波，回调 PGA 到 12dB 或降 maxlevel（如 0x60，maxlevel=6≈-13.2dBFS）；若远场仍不够响，可调大 maxlevel（如 0xA0，maxlevel=10≈-9.3dBFS，接近削波边界需谨慎）。

## 回退

单次改动小（3 个寄存器值 + PGA），若真机回归可直接还原：PGA=24、REG18=0x23、REG19=0x30。
