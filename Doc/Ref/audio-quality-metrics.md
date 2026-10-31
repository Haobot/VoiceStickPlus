# 音频质量评价标准与实验台（audio_lab）

> 状态：现行（2026-10-31 建立）。配套工具 `scripts/e2e_test/audio_lab.py`；数据来源
> DebugAudio 会话（macOS 侧新增来源标注/seq 缺口实测/侧车 json/raw 双保存）。
> 阈值锚定 2026-10-16 网关音频取证（`Doc/Expe/remote-gateway-audio-speed-timbre-2026-10-16.md`、
> `Doc/Plan/xiaomi-gateway-audio-timebase-and-dma.md`）与 RNNoise 判据（`Doc/Plan/software-agc.md`
> 附录/whisper-level-agc-v2 附录 A）。改阈值必须写明新取证出处。

## 一、为什么要这套标准

识别精度排查已确认瓶颈在「音频链路 + 模型能力」而非量化；但此前判断某次音频好坏只能听
ogg + 翻日志，且**两个音源（小米遥控器网关中继 / StickS3 自带麦）在 DebugAudio 文件名上
无法区分**。本标准把「音频好坏」拆成三层可量化指标，全部在 audio_lab 页面以红绿灯呈现，
并支持 A/B 双轨对比与同步试听。

## 二、三层指标定义与阈值

### 第 1 层：传输完整性（会话侧车 `<同名 ogg>.json`）

侧车由 macOS 桌面端在会话结束时写入（`DebugAudioSidecar`），ASR 完成后补写 `finalText`。

| 指标 | 定义 | 🟢 健康 | 🟡 注意 | 🔴 异常 | 判据出处 |
|---|---|---|---|---|---|
| 帧覆盖率 | 非空帧数 × 40ms ÷ 录音时长 | ≥0.95 | 0.8~0.95 | <0.8 | **恒 0.5~0.8 = 设备端尾帧丢弃指纹**（END 保序先到，设备没发）；frames=0 = 设备没发帧（targeted-edit-accuracy-layered-triage） |
| seq 缺口丢帧 | BLE 音频帧序号缺口累计（实测） | 0 | 1~2 | >2 | 新增实测（此前只能靠覆盖率推断） |
| span/录音 | 首末帧到达跨度 ÷ 录音时长 | 0.9~1.6 | 其余 | — | span ≫ 说话时长 = BLE 侧积压拖尾（voice-input-latency-root-cause-chain） |

### 第 2 层：信号质量（浏览器从 PCM 计算）

| 指标 | 定义 | 🟢 | 🟡 | 🔴 | 说明 |
|---|---|---|---|---|---|
| 峰值电平 | 20log10(max\|x\|) | <−3 dBFS | −3~−1 | >−1 | 削波风险 |
| 削波占比 | \|x\|>0.98 样本比例 | 0 | >0 | >0.05% | |
| 语音段 RMS | VAD 语音帧内样本 RMS | −32~−8 dBFS | 其余 | — | 过低 = 增益不足（AGC 目标 −12dBFS） |
| 底噪 | 最低 15% 能量帧分位（20ms 帧） | 显示值 | — | — | RNNoise 效果对照基线 |
| SNR 估计 | 语音段 RMS − 底噪 | ≥20 dB | 10~20 | <10 | |
| <300Hz 占比 | 语音帧频谱能量占比 | ≤34% | 34~45% | >45% | **轰鸣/直流指纹**（取证健康值 20.3~31.4%，异常 45.2%） |
| 4–6.5k 占比 | 同上 | ≥15% | 8~15% | <8% | **发闷指纹**（健康 15.5~21.4%，异常 7.7~12.6%；线性插值致 16.5%→11.7% 即 4kHz −3dB） |
| F0 中位 | 语音帧自相关基频（70–400Hz） | 显示值 | — | — | 单轨看稳定性；双轨看比值 |
| 语音时长 | 能量 VAD（底噪+6dB 门限） | 显示值 | — | — | |

### 第 3 层：后处理/双源对照（A/B 模式 Δ 列）

| 判据 | 🟢 | 说明 |
|---|---|---|
| Δ 语音 RMS | \|Δ\| ≤ 0.5 dB | RNNoise 判据（语音电平不动 ≤0.3dB，留 0.5 容差）；±0.3dB 为严格线 |
| Δ 底噪 | < −0.5 dB（应下降） | 降噪有效的直接证据（取证量级 1.3~9.8dB） |
| Δ 4–6.5k | \|Δ\| ≤ 0.5（用 dRms 灯） | 音色保持 |
| F0 比值 B/A | 0.97~1.05 | **时间基漂移指纹**：同一人双源基频中位比 ≈ 伸缩比（取证 108.1/98.8=1.094 即遥控器压缩 9%） |
| 现场识别文本 | 侧车 finalText | 「音频好坏」的最终锚点（ASR 实况输出） |

## 三、DebugAudio 会话标注（macOS 端，2026-10-31 起）

- **文件名前缀三态**：`VS-` Stick 麦 / `VG-` 网关中继（遥控器经 Stick 转发）/ `RC-` 遥控器直连。
  解析纯函数 `AudioSessionSource.resolve(triggerSource:deviceClass:)`（VoiceStickCore）：
  网关中继依赖固件既有的按键 `"source":"xiaomi"` 事件（protocol.md StateEvent），**零协议/固件改动**。
- **侧车 `<同名>.json`**：source / deviceClass / sessionID / frames / span / recording /
  droppedFrames（seq 缺口实测）/ noiseSuppression / finalText（ASR 后补写）/ savedAt。
  取消/出错会话无 finalText；历史与 Windows 文件无侧车，工具按"—"容错。
- **增强双保存**：`audio_noise_suppression=on` 时另存 `<名>.raw.ogg`（增强前原件，与主文件共享
  时间戳基名）——RNNoise 前后对比从此有真数据（此前增强开启时原始音频即丢失）。
- **Windows 同构**：登记 backlog（`debug_audio_recorder.cc` 命名注释「对齐 macOS」会漂移；
  Windows 暂仍全 `VS-`，工具按前缀容错显示「未知/Stick 麦」）。

## 四、A/B 对比方法

1. **顺序复述配对（现行，零固件）**：同一句话分别用两个音源各说一遍 → audio_lab 自动包络
   互相关对齐（±10ms 手动微调）。**注意：两次说话本身的差异会混入对比**，UI 有明示。
2. **真同事件双录（Phase 2，未实施）**：固件在转发遥控器时同步采集 Stick 麦至 PSRAM、会话后
   经 audio_tx 调试流（flags bit2，向后兼容零版本变更）导出为 `*-micref.ogg` 自动配对。
   实施须同步 protocol.md + 固件 + macOS 并过 OTA/功耗回归（跨版本契约红线）。

试听三模式：只听 A / 只听 B（空格即时切换）/ **左右分耳同步**（左耳=A 右耳=B——「同时播放
两个音源」的直接实现）；等响度开关把较响一路衰减到另一路电平（只衰减不放大，防削波）；
循环播放。听感对比务必开等响度——响的那路总会被主观判为「更清楚」。

## 五、工具用法

```bash
python3 scripts/e2e_test/audio_lab.py            # 自动定位 DebugAudio 目录（双平台）
python3 scripts/e2e_test/audio_lab.py --dir scripts/e2e_test/corpus   # 指定目录（.ogg/.wav）
```

- 左列表点行 = A 轨单轨视图；每行 A/B 键选两轨进入对比。
- 交互沿用 spectrogram_server 习惯：滚轮缩放、拖动平移、悬停读数（t/f/dB）、点击定位播放。
- 指标全部浏览器内计算（Web Audio 解码 + JS FFT/自相关），纯标准库服务端，无第三方依赖。
- 兼容性：推荐 Chrome/Edge；Safari 对 ogg/opus 解码不稳，页面会给出提示。
- 单会话快速看图也可继续用 `scripts/e2e_test/spectrogram_server.py`（本工具是其超集）。

## 六、判读速查（排障路径）

| 现象 | 先查 | 再查 |
|---|---|---|
| 帧覆盖率 0.5~0.8 | 设备端尾帧丢弃（非桌面问题） | 串口 `frame=`/`source=`；DebugAudio 回放确认语速正常止于最后一帧 |
| seq 缺口 >0 | BLE 传输丢帧（链路质量） | 换距离/干扰环境复测；对照串口 `tx failed/dropped` 计数 |
| <300Hz >45% | 拾音轰鸣/直流/手持姿势 | Stick 麦 vs 遥控器分源对比定位是哪一路 |
| 4–6.5k <8% | 发闷（高频损失） | 双源 F0 比值同时 >1.05 ⇒ 时间基插值链；否则查编码/麦克风遮挡 |
| F0 比值 ≈1.09 | 遥控器时间基压缩复发 | 串口 `stretch=`（健康 1.000~1.033） |
| Δ底噪未降 | RNNoise 无效 | 输入量纲（int16 浮点）与 VAD 判据；对照 raw vs 主文件 |
