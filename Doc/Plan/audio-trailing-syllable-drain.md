# 录音松开尾音截断修复（audio_task drain）

## 背景

wechat 延迟优化（B2 instant 模式）真机验证后，用户反馈：说完话立即松开按键，最后 1-2 个字会丢。优化前因 hold 阈值 300ms 让用户有松开预期延迟，该问题不明显；instant 模式让"说完即松"更常见而凸显。实际是固件既存 bug。

## 根因

`firmware/components/audio_pipeline/audio_pipeline.c`：

- `audio_pipeline_stop`（537-554）：设 `s_running=false` → 发 sentinel 给 `s_tx_queue`，触发 `tx_task` drain 已入队包。
- `audio_task`（285-343）：`while (atomic_load(&s_running))` 循环，`s_running=false` 后正在进行的 `esp_codec_dev_read` 完成后下一轮检查退出，**不再读取 ES8311/I2S DMA 缓冲区里残留的尾音 PCM**。
- I2S DMA 配置：4 描述符 × 120 帧/描述符 = 总缓冲 ~60ms 音频（16kHz stereo）。松开瞬间 DMA 里最多残留 ~60ms 尾音不被读出编码发出。

`tx_task` 的 drain（387 行）只能排空已入队的包，救不回未读取的 DMA 尾音。

## 第一次修复（drain）不够

加 audio_task drain 后真机测试，用户反馈"最后 2 字仍丢"。进一步分析发现根因更深：

- `audio_pipeline_stop` 发 sentinel 后**立即返回**（不等 drain 完成）。
- `stop_recording` 返回后立即发 `button_up` notify（main.c:1071）。
- `button_up` 走 `state_tx`，drain 帧 + audio_end 走 `audio_tx`，两者并发，**button_up 先于 drain 帧到达桌面端**。
- 桌面端 wechat 模式收到 button_up → `StopWechatInputMethodSession`（停 renderer + 松热键 + `wechat_active=false`）→ 之后 drain 帧和 audio_end 到达时 `active_session_id_` 已 reset，早退丢弃。
- drain 帧虽编码发出了，但桌面端已结束会话，**drain 音频丢失**。

## 最终修复

`audio_pipeline_stop` 发 sentinel 后**同步等 audio_task + tx_task 退出**（`wait_for_tasks_to_exit`），再返回：

- audio_task 退出前做 drain（读 2 帧 DMA 残留编码入队）。
- tx_task 收 sentinel 后 drain 队列（把含 drain 帧的剩余包发往 BLE）→ 发 audio_end → 等 audio_task 退出 → deinit codec/i2s → 自删。
- 两者都退出后 `audio_pipeline_stop` 返回，`stop_recording` 才发 button_up。
- 此时所有 audio notify + audio_end 已提交给 BLE 栈，button_up 在其后，桌面端按序收到：drain 帧 → audio_end → button_up（结束会话）。尾音不再丢。

超时兜底 `TASK_EXIT_WAIT_MS=800ms`，避免极端积压卡死。`audio_pipeline_start` 不调 stop（`s_running` 已 true 直接返回），无 start/stop 双等待死锁。

## 修复

`audio_task` 的 while 循环退出后、`vTaskDeleteWithCaps` 前，加 drain 段：把 I2S DMA 残留样本读出编码并入队，再退出。

- 读最多 2 帧（80ms，覆盖 60ms DMA 残留 + 余量）。
- 每帧：`esp_codec_dev_read` → 降混 mono → `opus_encode` → 入队 `s_tx_queue`。
- `esp_codec_dev_read` 阻塞读 DMA 已就绪样本；DMA 无新数据时会阻塞等下一帧，但 drain 限 2 帧即停，最多阻塞 80ms，可接受（`audio_pipeline_start` 的 `wait_for_tasks_to_exit` 等旧任务退出，80ms 在 TASK_EXIT_WAIT_MS 内）。
- drain 帧用当前 `s_session_id` / 递增 `s_seq`，与正常帧一致，无缝接在录音末尾。
- drain 完成后 audio_task 自删，`audio_pipeline_stop` 的 sentinel 已发，`tx_task` drain 把剩余包（含 drain 帧）发完。

## 边界与不变量

- drain 帧数固定 2（不依赖 DMA 是否有数据，避免阻塞探测复杂度）。
- drain 期间 `s_running=false`，新 `audio_pipeline_start` 会被 `wait_for_tasks_to_exit` 阻塞直到 drain 完成 + 任务退出，无并发。
- drain 帧的 session_id 与录音一致，桌面端按 session_id 归属，正常接续。
- opus 编码器状态连续（不 reset），尾音编码质量与正常帧一致。
- stop_recording 流程不变（main.c），audio_pipeline_stop 仍发 sentinel，时序不变。

## 验证

固件无单测，靠 `idf.py build` + 真机：

1. `idf.py build` 编译通过。
2. COM17 串口烧录（设备已配 Boot 流程，长按进 Boot、短按重启）。
3. 真机回归：说完话立即松开，最后 1-2 字不再丢；正常长按松开无异常；连续录音无串音。

## 实施步骤

1. 本方案文档。
2. 改 `audio_pipeline.c` audio_task 加 drain 段。
3. `idf.py build` + 烧录 + 真机验证。
4. 更新记忆 `wechat-press-to-popup-latency-optimization`（补充尾音 drain）。
5. `git add -f` 提交。
