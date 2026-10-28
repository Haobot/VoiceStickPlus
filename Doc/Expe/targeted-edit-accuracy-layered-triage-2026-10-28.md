# 语音功能「准确率不好」先分层定位：同字节回放转写 + 帧比例判据（2026-10-27~28 targeted 实战）

- 日期：2026-10-28
- 相关文件：`desktop/macos/Sources/VoiceStickCore/VoiceStickCoordinator.swift`、
  `VoiceTargetedEdit.swift`、`firmware/components/audio_pipeline/audio_pipeline.c`、
  `scripts/local_align_bench/`
- 相关 commit：`583e454`（基线回归表）、`b453bcc`（本地对齐层 feat 主体）、`d7a6743`、`bfe3b82`
- 时效声明：日志行格式/帧常量/文件行号为记录时点结论，引用前以当前源码为准。

## 症状

targeted 指定词删除/替换上线后用户反馈「有替换效果，但准确率不好」。当晚报 27 轮实测
（22:52–22:55，session 19–45），指令意图 11 轮仅成功 2 轮（18%）。

## 判据（下次直接用）

1. **逐轮还原识别文本——DebugAudio 同字节回放转写**：用与 app 完全相同的腾讯实时
   ASR 端点/引擎/Opus 封装字节回放 `DebugAudio/*.ogg`（裸 socket websocket + HMAC-SHA1
   签名 + `"opus"+u16len+packet` 封装 + slice_type=2 累积 + `{"type":"end"}` 收尾；
   参考实现当时在 `/tmp/vs_asr_replay/`，Ogg 抽帧按段表同 `asr_bench/wsproto.py` 教训）。
   **校准法：回放文本 UTF-8 字节数与日志 `text_len` 逐轮对账**（本次 27 轮 26 轮逐字节
   吻合 ⇒ 可当实况用；1 轮漂移即 ASR 非确定性实证）。
2. **帧比例指纹**：`Audio stream summary` 的 `frames×40ms / span`：
   - ≈1.0：音频完整；
   - 0.5~0.8 恒定：设备端尾帧丢弃（BLE 同特征 notify 保序且 END 标志帧先于 button_up
     到达 ⇒ 缺的帧是**设备没发**，不是空口/app 丢）；配合回放音素时间轴「语速正常 +
     语音恰好终止在最后一帧（可切在词中间）」⇒ 连续前缀尾部整段丢，排除均匀丢帧
     （那会让语音 1.33× 变速而不是干净截断）。
   - 跨端同款判据先例见 `voice-input-latency-root-cause-chain-2026-10-12.md`。
3. **静默回退无日志**：targeted 解析 nil / 自然简写门控未命中都直接 `return false` 走
   普通粘贴，日志零痕迹（只有 paste 的 text_len）——**没有识别文本就对「哪层失败」
   下结论必然瞎猜**。

## 根因（三层分解，均实证）

- **A 层 ≈55%：设备端音频尾帧丢失**（22:52:46–22:54:24 窗口链路吞吐 ≈19/25fps，停止
  时 `audio_pipeline` drain 在 2s 预算内排不完 → `xQueueReset` 丢尾帧；历史上
  TX_DRAIN_TIMEOUT_MS 500→2000ms 就是为同一症状打的补丁）。指令「把北京改成上海」
  被截成「把北京改成。」→ 空替换回退粘贴。**模型/桌面端均无法修复**，需带串口复现
  确认具体丢弃路径（drain timeout / sentinel 满队 reset / 单帧重试耗尽）——待办。
- **B 层 ≈27%：ASR 指令轮目标词漂移**（升旗→分析/身体、成都→都）：指令句缺正文语境，
  低频词向高频词漂移；正文轮同词正常。→ 已落地本地小模型对齐兜底（见下篇互链文档）。
- **C 层 ≈18%：回退粘贴污染 LastInjection 级联**：指令回退成普通文本粘贴后覆盖编辑
  记录 → 正确重试也找不到目标（两次实证），甚至编辑到已粘贴的指令文本上（一次实证）。
  「保留旧记录救重试」不可行（字段已脏，L1 后缀校验会正确拒绝）；**候选修复
  isCommandFallback 标记 + 门控跳过，且是对齐层的安全前置**（对齐在污染文本上会
  「合理地」选错对象）——待拍板。

## 验证程度

- 排查结论：日志+回放双证，用户 session 33 口述「最后的几个字无法跟他说」自证 A 层；
- 对齐层：单测/协调器测试 1584/1584 + 六模型闭环评测 + LocalAlignClient 真链路冒烟 +
  **用户真机验收通过（2026-10-28 上午）**；
- A 层固件侧丢弃路径**未**串口实证；Windows 侧对齐层未实现。

## 经验

1. 「没效果/不准」的第一动作是**建失败样本回归表并分层**（输入/指令/预期/实际/失败
   层级），不要先动匹配或安全逻辑——本次三层里两层不在被投诉的功能里。
2. 隐私友好拿识别文本：日志不记文本是对的（纪律），回放转写给出等价能力且比加日志更强
   （还能听出截断模式）；注意标注「回放」并留 1/27 非确定性的余量。
3. 双端都缺 execute 阶段结果码日志（选区失败/终验 unconfirmed/成功均无痕）——补日志
   是对齐层之前就该有的可观测性（§17.7 待办）。
4. 回归表 `Doc/Test/voice-targeted-accuracy-baseline-2026-10-27.md` 可直接续记新失败样本。

## 互链

- 小模型对齐层的选型与 prompt 教训：`local-llm-alignment-task-shape-lessons-2026-10-28.md`
- Ogg 抽帧段表坑：`asr-bench-lessons-2026-08-01.md`
- 音频质量分层先例：`remote-gateway-audio-speed-timbre-2026-10-16.md`
- 延迟分段判据先例：`voice-input-latency-root-cause-chain-2026-10-12.md`
