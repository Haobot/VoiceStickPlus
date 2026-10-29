# targeted 替换在 Chromium 系目标必败——AX 选区/光标写入异步生效，写后立即读回必拿旧值

- 日期：2026-10-29
- 相关文件：`desktop/macos/Sources/VoiceStickCore/VoiceStickCoordinator.swift`（executeVoiceTargetedEdit 及拆出的 verifyTargetedSelectionThenAct / finalizeTargetedEdit）、`desktop/macos/Sources/VoiceStickApp/AppDelegate.swift`（FocusedFieldAXReader）
- 相关 commit：`780d1a2`（修复，feat/macos-local-model-settings 分支）；根因探针实证当日在 Edge + ZCode + TextEdit 三应用复现
- 验证程度：探针矩阵实证 + 单测 1719/1719 + 真机复测通过（用户验收 2026-10-29）

## 症状

语音输入 targeted 修改（「把北京改成上海」）命令轮解析、计划全部正常（日志 `voice_targeted action=targetedReplace target_len=2 occ=1/1`），但目标应用字段**零变化**，仅弹失败 toast；连续多次尝试全败。无任何 failure 日志——执行环节的失败路径全部是静默的。

## 日志判据

- `voice_targeted action=...` 之后**没有任何后续日志** = 执行环节静默失败（修复前选区写失败/读回不符/终验未确认/成功全部无痕，排障黑洞）。
- 用户看到 toast `voiceTargetedTextChanged`（fieldMismatch）= 走到「读回不符」拒绝路径：选区写了但读回对不上。
- 修复后新增日志：`voice_targeted readback retry got_len=%d`（首次重试）、`voice_targeted readback mismatch expected_len=%d got_len=%d`（重试耗尽）、`voice_targeted verify unconfirmed expected_len=%d actual_len=%d`（终验耗尽）、`voice_targeted result=ok target_len=%d`（成功）。

## 根因

targeted 执行链是「AX 写选区（kAXSelectedTextRange）→ **立即**读回 kAXSelectedText 逐字确认 → 一次退格/粘贴 → 0.2s 后终验（写光标后**立即**读字段状态）」。其中两处「写后立即读」在 Chromium 系目标（Electron 的 AXTextArea、浏览器 textarea）上必踩：

1. **AX 选区写入经 renderer 异步生效**。写入调用本身返回 success，但选区状态要等 renderer 回程才可见；立即读回拿到写入前的旧值（空串）≠ 目标词 → 判「读回不符」零改动放弃。
2. **终验同理**：粘贴后光标恢复写（末尾折叠）也是异步落地，0.2s 后立即读可能仍见写入前状态 → 假阴性「结果未确认」+ 记录失效。

听写正文恰是 Cmd+V 粘贴上屏的，所以真实使用流里 targeted 替换在 Chromium 系目标**必然失败**。原生应用（TextEdit/备忘录）AX 写同步生效，立即读回即中——这是真机验收（原生目标）通过而用户日常（Electron 聊天输入框）全败的原因。

**排查中的假判别变量（重要教训）**：探针先得出「粘贴填充的字段写选区被忽略、键入填充则正常」的结论，实为伪相关——多轮对照后钉死：**唯一判别变量是写选区到读回之间的时延**（立即读全败；+30~120ms 读全成），与内容是粘贴还是键入无关。早期 probe 之间结果互相矛盾，就是因为有的探针在读回前恰好 sleep 了。

## 修复（`780d1a2`）

- 读回确认与终验改为**小步轮询**：读回节拍 `[0.03, 0.06, 0.12, 0.2]`s（共 5 次尝试）、终验节拍 `[0.05, 0.1, 0.2]`s。原生应用首次读即命中，零重试、行为完全不变；Chromium 系在窗口内确认。
- 轮询只追加**读操作**，不改字段任何状态；重试耗尽仍不符才走原拒绝路径（恢复末尾光标 + toast，零删除/零粘贴）——「验证不过绝不盲改」的安全模型原样保留。
- 每个异步跳转都带 `voiceEditGeneration` 代际守卫（新注入即失效）。
- 补齐上述全部静默路径日志。
- 顺手修第二 bug：`attemptLocalAlignOrFallback` 残留 `!localAlignModel.isEmpty` 死闸——内嵌 llama.cpp 迁移后配置语义是「空 = 默认 1.5B 档」，旧守卫令目标词被 ASR 听漂时的对齐兜底整体失效。模型可用性已由 alignTarget 闭包内 `LocalLlmRuntime.alignModelPath`（缺失返回 nil → 普通文本回退）兜底，门控不再看模型名。

## 验证

- 探针矩阵（独立 Swift CLI，复刻 FocusedFieldAXReader 的 dlsym 四参 ABI）：TextEdit 对照组全通；Edge textarea 与 ZCode 聊天输入框「粘贴填充 → 写选区(3,2) → 轮询读回『北京』→ Cmd+V 替换 → 字段变预期 → Cmd+Down 光标回末尾」全链路通过；立即读回组全败（复现原 bug）。
- 单测：读回不符用例期望更新为重试耗尽语义（5 次读回后恢复光标），1719/1719 全绿。
- 真机：用户实测 targeted 替换恢复正常（2026-10-29 验收）。

## 长期经验

1. **AX/平台写入后绝不能立即读回做验证**——写调用 success ≠ 已落地。带验证的注入类功能（选区/光标/值写入）一律小步轮询确认，轮询只追加读操作，耗尽才走拒绝路径。与 BLE CCCD「订阅假成功」（§3.15 家族）同构：**平台返回的成功不等于对端可见**。
2. **执行环节静默路径必须有日志**——本次 `voice_targeted action=` 之后零日志，排障只能靠探针重建。闭环了 `targeted-edit-accuracy-layered-triage-2026-10-28.md` 待办第 3 条。
3. **验收环境覆盖偏差**：注入/AX 类功能在原生目标（TextEdit）验收通过 ≠ Chromium 系目标可用——真机验收至少覆盖一个 Electron/浏览器文本目标。
4. 排障时用**探针矩阵把判别变量钉死**（单变量对照 + 多次重复），别让首轮结论（「粘贴 vs 键入」伪相关）带偏修复方向。

## 遗留 / 观察项

- Chromium 上读回通道更稳的备选是「读 kAXSelectedTextRange + 从 kAXValue 取子串比对」（range 读回实测始终可靠，kAXSelectedText 成功时也可能给空串）；本次轮询已解决，未切换通道，留作 AX 写彻底失效场景的备选方案。
- 键盘选区兜底（光标算术 + Shift+方向键 + Cmd+Down 回末尾）已在探针 v5 全链路验证可行，留作 AX 写彻底不可用目标（本次未遇见）的最后手段；注意方向键按 grapheme 簇移动，非 BMP/组合字符场景偏移会漂——兜底路径同样必须过读回验证。

## 互链

- 同功能准确率分层排查：`targeted-edit-accuracy-layered-triage-2026-10-28.md`
- 本地对齐层选型与迁移：`local-llm-alignment-task-shape-lessons-2026-10-28.md`
- TCC/AX 授权家族（「授权对运行中进程不生效」「CLI 启动 TCC 归因失效」）：`macos-windows-design-parity-port-2026-09-03.md` §3
- 「平台返回成功 ≠ 对端可见」同构先例：`ble-cccd-cache-subscription-not-delivered-2026-09-19.md`
