# VB-CABLE 虚拟声卡延迟测量指南

## 背景与目的

微信输入法模式下，音频经 VB-CABLE 虚拟声卡传输到微信输入法：

```
桌面端 Opus 解码 -> PCM -> WASAPI render(CABLE Input) -> VB-CABLE 内部缓冲 -> CABLE Output 录制 -> 微信输入法 ASR
```

VB-CABLE 内部缓冲是"按下到出首字"延迟的潜在贡献者。真机日志已证桌面端链路（`button_down -> SendDown`）仅 74-296ms（见 `Doc/Plan/wechat-latency-test-framework.md`），ring 积压上限 512ms，WASAPI buffer 已优化至 20ms，合计不足 1 秒。**1-2 秒差额在 VB-CABLE 缓冲 + 微信 ASR 黑盒**。本指南测量 VB-CABLE 端到端延迟，定位主因归属。

## 方法1：VB-CABLE 控制面板（最简，1 分钟）

1. 打开 **VB-Audio Virtual Audio Cable Control Panel**（开始菜单搜索 "VB-CABLE" 或安装目录 `vbcablectl.exe`，需管理员）。
2. 查看 **CABLE Input** 一栏的采样率与缓冲（Latency / Nb Buffers × Buffer Size）。
3. VB-CABLE 的端到端延迟 ≈ `NbBuffers × BufferSize / SampleRate`。典型配置 1×256@48000Hz ≈ 5ms（很小），但若设大缓冲（如 8×2048）可达数百 ms。
4. 若缓冲偏大，调小（如 1×256 或 2×256），点 **Apply**，重启使用 VB-CABLE 的应用。

> VB-CABLE 默认缓冲通常很小（~5-10ms），**若默认配置则 VB-CABLE 非主因**，1-2 秒大概率在微信 ASR 黑盒。

## 方法2：Audacity loopback 测量（准确，5 分钟）

1. Audacity 顶部录制设备选 **CABLE Output**。
2. 菜单 生成 -> 静音 -> 1 秒；再 生成 -> 音调 -> 1000Hz，幅度 0.8，时长 50ms（作为 click 标记）。
3. 播放设备选 **CABLE Input**，点播放 + 同时按 R 录制。
4. 停止后查看录制波形：click 出现的时间（相对播放起点）减去原始 click 位置 = **VB-CABLE 延迟**。
5. 重复 3 次取平均。

## 方法3：LogWechatLatency 对照法（无需额外软件）

1. 启用 VoiceStick 微信模式，按设备键录音说一句话。
2. 手动记录"按下时刻"与"微信输入法出首字时刻"（秒表或录屏逐帧），得端到端延迟 T_end。
3. 读 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`，取该 session 的 `wechat latency: SendDown end (popup triggered) +Nms`（桌面端链路，约 74-296ms）。
4. **下游延迟 = T_end - 桌面端 Nms - 固件按键/hold（~300-360ms）**。此值即 VB-CABLE + 微信 ASR 缓冲。

## 解读与调优建议

| 观测 | 结论 | 行动 |
|---|---|---|
| VB-CABLE 控制面板缓冲 < 50ms | VB-CABLE 非主因 | 1-2 秒在微信 ASR 黑盒，接受 |
| VB-CABLE 缓冲 > 200ms | VB-CABLE 是主因之一 | 方法1 调小缓冲 |
| 下游延迟（方法3）> 800ms | 微信 ASR 缓冲主导 | 不可控，接受或换 ASR |
| 下游延迟 ~ 300-500ms | VB-CABLE 主导 | 调 VB-CABLE 配置 |

## 诚实预期

桌面端已优化（`buffer_duration_ms` 50->20 省 30ms，见提交 `e6bd975`）。VB-CABLE 可测可调（若缓冲大）。**微信 ASR 黑盒不可控**。1-2 秒主因大概率在微信 ASR 缓冲（按下后微信需积累音频 + VAD + ASR 处理才出首字），桌面端 + VB-CABLE 优化空间合计约几百 ms。完整自动化 loopback 测量 C++ 程序（WASAPI render+capture+脉冲检测）可作为后续工作按需实现。
