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

## 追加（2026-10-31）：终验窗 0.6s 仍短于粘贴落地时延——「首次成功却提示失败、二次命令变普通文本」连锁

- 症状（用户实报）：targeted 替换第一次**实际成功**但浮窗提示失败；紧接的第二次命令完全不生效、命令文本被当普通文本粘贴上屏。
- 日志判据：`voice_targeted verify unconfirmed expected_len=27 actual_len=27`——**同长替换**下 expected/actual 字节数相同但内容不同，即终验读到的是动作前旧文本（动作后 0.55s 粘贴仍未在 AX 可见态落地）。本文修复覆盖的是「写选区→读回」时延（30~120ms），**动作本体（Cmd+V 粘贴）的落地时延是另一档**（>0.6s，上限未钉死；修复窗按 ≈2.7s 留余量）。
- 连锁根因：误判「结果未确认」→ `invalidateVoiceTargetedRecord` 清记录 → 自然简写命令（不带「刚才的」）失去作用域记录 → 按设计回退普通文本上屏（显式作用域命令恒拦截弹 noInjection，门控未漏拦）。**失败判据的连锁副作用要和失败本身一起审**：清记录是防陈旧偏移的安全动作，但把「动作未落地」错归为「结果真不符」时，代价是后续命令整条链路静默降级成打字。
- 修复（双端同构）：① 终验轮询窗 `[0.05, 0.1, 0.2]` → `[0.1, 0.2, 0.4, 0.8, 1.0]`（总窗 ≈2.7s）；② **窗末按内容分类**——文本仍是动作前原样＝动作未落地 → 记录保留只提示（后缀不变量仍成立：粘贴丢失时同命令重试直接生效，迟到落地时被 planner 字段校验拦下，均不污染字段）；文本变成第三种内容＝真不符 → 才失效记录；③ 中间轮次读到第三方文本不判死（粘贴中间态可能短暂为第三种文本），只在窗末分类。日志 `verify unconfirmed … still_old=0/1` 区分。
- 教训：验证窗的长度要按**被验证动作自身的落地时延**定，而不是按读回通道的时延定；「平台返回成功 ≠ 对端可见」家族里，读回延迟与动作落地延迟是两个独立档位（后者更大）。

## 追加（2026-11-04）：Windows 端同步缺失重演必败——v2.5.0 移植漏带本文修复，DebugAudio 回放定性「微信没显示」

- 症状（Windows 用户实报）：Win 端记事本替换/删除正常，ZCode 输入框不行，微信输入框「更没有显示」。
- 日志判据（Windows 旧二进制，< `950abd4`）：`voice targeted action=` 后**零日志** = 静默失败（Windows 移植时连本文的 result=ok 日志都未带，成功与 fieldMismatch 在日志上不可区分——本节教训在 Windows 重演）。
- 根因：v2.5.0 的 Windows targeted 移植**带了 2026-10-31 终验轮询却漏了本文的读回轮询**——`focus_text_reader.cc` 的 `SelectAndReadBack` 在 UIA `Select()` 后立即 `GetSelection` 读回，Chromium UIA provider（ZCode=Electron）异步生效 → 必拿旧值 → fieldMismatch 零改动放弃。与 Mac 修复前完全同构（8 次 action 全部零日志：ZCode 3 败 + Notepad 5 成，用户报告与日志盲区完全吻合）。
- 修复（Windows `950abd4`，双端同构）：`ITargetedFieldEditor` 写/读拆分（`SelectTailRange`/`SelectAbsoluteRange` 只写 + `ReadSelectedText` 只读），协调器按 `[30,60,120,200]`ms 轮询读回（只追加读操作）；补齐 `readback retry`/`readback mismatch`/`result=ok` 日志；TDD 两用例（stale_reads=2 收敛 / 耗尽零改动记录保留）先红后绿，CTest 2/2 绿。**移植教训：跨端移植功能要把源端该功能的全部后续修复 commit 链拉出来逐条对齐，不能只对齐「当时看到的代码形态」**——10-29 与 10-31 是两笔独立修复，只带后者的移植必然复现前者。
- 微信（Weixin.exe 4.x）场景定性：**31 轮 DebugAudio 全量回放**（`scripts/e2e_test/replay_tencent_asr.py`，凭据从本机 config.toml 注入）证明微信场景 3 轮全是正文「我要去北京吃烤鸭。」（app 侧 paste_complete + record len=27=9 字剥尾标点吻合），**命令轮从未在微信发生**——用户主观「没有显示」到底是粘贴未上屏还是记忆混叠，修复后凭新日志复测定性（`result=ok`=闭环 / `readback mismatch` 耗尽=微信 UIA 选区写不可用需二期键盘兜底 / 正文 app 记录了但用户没见到=粘贴链路独立缺陷，头号嫌疑 150ms 剪贴板恢复竞态）。**没有识别文本前别给「微信不行」归因**——本次若按「微信也必败」提前改粘贴链路，就是朝错误方向动代码。
- Windows 遗留：撤销链路（undo）已迁移到写读分离接口但保持单次读回（行为不变），Chromium 上 undo 读回为已知遗留，待测试基建后同步轮询。

## 追加（2026-11-04 第二轮）：读回轮询不足以覆盖「选区写零落地」——键盘选区兜底（`9f59d56`）

- 复测判据（Windows `950abd4` 后 ZCode）：`readback retry got_len=0` → 轮询 5 次全空串。**got_len=0 = GetSelection 恒折叠光标 = UIA `Select()` 返回 S_OK 但选区零落地**——不是异步慢（410ms 窗内恒空），轮询救不了。对照组 Notepad 完美（`got_len=3` 重试命中 `result=ok`）。
- 生态佐证：Chromium 原生 UIA 自 Chrome 138 默认开启，ITextRangeProvider 有已知缺陷（issue 568011891），Electron 滞后。**「平台写调用返回成功」在 Chromium UIA 上甚至可能是纯 no-op——比「异步生效」更深的失效档位**。
- 修复（`9f59d56`，本文预留的「AX 写彻底不可用目标的最后手段」落地）：UIA 读回耗尽 → 键盘选区兜底——`PlanKeyboardSelection`（grapheme 表精确边界匹配，落在簇中间一律拒绝；末尾锚 `Ctrl+End + Left×(G-ge) + Shift+Left×(ge-gs)` / 首锚 `Ctrl+Home + Right×gs + Shift+Right×(ge-gs)` 双方案按事件数取短；>2048 事件不可行）→ 后台线程逐键（12ms/键）→ 回 UI 线程读回轮询验证 → 命中才执行动作；验证不过 Ctrl+End 恢复 + fieldMismatch（零改动、记录保留——安全模型不变）。原生目标 UIA 路径优先命中，零回归。
- **锚序列方向教训（测试对拍钉出来的）**：末尾锚不存在「先扩后缩」——锚在右端时 Shift+Right 移动的是左端（选区只会左移或消失）；正确序列是「无 Shift 方向键移到目标右端 ge，再 Shift+Left 选中 [gs,ge)」（锚固定在 ge，Shift 朝锚反方向移动）。首锚同理（Right 到 gs 再 Shift+Right）。fake 编辑器推演 + 断言失败暴露，纯靠公式推导时两处独立写反。
- 键盘兜底复测实锤（`9f59d56` 后）：`keyboard fallback start anchor=home plain=3` 算法正确执行，但 `verify failed got_len=0`——**选区读通道也死**（GetSelection 恒空），键盘选区无法验证。至此 ZCode 的选区读写两通道全灭，而全文读通道 ✓、退格注入 ✓ 始终正常——「失效面」是选区专属。第三层兜底（`27e6896`）：**整框重写**——真机探测 ZCode 输入框有 ValuePattern（readOnly=False），读全文 → BuildWholeFieldReplacement 全文定位替换（簇表精确边界匹配，落在簇中间/越界拒绝）→ SetValue 写回 → 全文终验。不依赖选区通道、不依赖语音注入记录（「对整个输入框内容进行编辑，判断不依赖语音输入内容变化」的用户语义）。兜底层级：选区写+读回轮询（原生零回归）→ 整框重写 → 键盘选区 → 兜底拒绝。日志判据：`whole-field rewrite applied full_len=` + `result=ok`=闭环；`whole-field rewrite unavailable` = 目标无 ValuePattern（微信能力未知，凭日志定性）。SetValue 落地质量待真机验收（终验兜底防误伤）。

## 追加（2026-11-04 第三轮）：SetValue 同为 S_OK 空转——整框替换改走 Ctrl+A 全选粘贴（`e40537f`）

- 复测判据：`whole-field rewrite applied full_len=27` → `verify unconfirmed expected_len=27 actual_len=27 still_old=1`——**SetValue 返回成功但字段一字未变**（still_old=1=动作前原样）。至此 ZCode 上 UIA 写 API 全线实证 S_OK 空转（Select、SetValue），选区读（GetSelection）恒空；键盘兜底因无法验证被安全模型正确拦截。**UIA 读写通道全灭，SendInput 键盘是唯一活着的写通道**（退格删除一直在该目标上成功）。对照组 Notepad 同轮 `result=ok`。
- 修复：整框替换改走 `Ctrl+A 全选 + Ctrl+V 粘贴`（`ReplaceAllWithPaste`，复用 ClipboardVault 快照基建）——**Ctrl+A 语义固定=全选，无光标算术无错位；失败模式全部无损**（焦点错/粘贴被忽略 → 字段原样 → still_old 提示）。砍掉 SetValue 层（实证死路）与键盘选区层（依赖的选区读验证在同目标上已死）。终验轮询窗 ≈2.7s 覆盖粘贴异步落地。
- 日志判据：`voice targeted whole-field paste applied full_len=` → `result=ok`=闭环；`verify unconfirmed still_old=1` = Ctrl+A/Ctrl+V 也未落地（下一层查焦点 HWND 分发）。
- 教训累积：**Chromium 原生 UIA 的「写」面（Select/SetValue）在部分 Electron 应用上整体空转且返回成功——平台写 API 的返回值在 Chromium 上完全没有可信度**，验证必须走「动作后读回」。终态方案反而最朴素：全文读（活）+ 键盘全选粘贴（活）+ 全文终验（活）。

## 互链

- 同功能准确率分层排查：`targeted-edit-accuracy-layered-triage-2026-10-28.md`
- 本地对齐层选型与迁移：`local-llm-alignment-task-shape-lessons-2026-10-28.md`
- TCC/AX 授权家族（「授权对运行中进程不生效」「CLI 启动 TCC 归因失效」）：`macos-windows-design-parity-port-2026-09-03.md` §3
- 「平台返回成功 ≠ 对端可见」同构先例：`ble-cccd-cache-subscription-not-delivered-2026-09-19.md`
