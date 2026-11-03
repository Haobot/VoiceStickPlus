# 语音编辑批次 2 开发任务书——追问「第几个」+ 整个输入框可编辑 + 撤销

> 状态：定稿（四项决策已由用户 2026-11-03 确认，均按推荐项）。
> 分支：`feat/voice-edit-followup`，基于合并批次 1 之后的 main（协调者已建好，worktree
> `/Users/hwp/Dev/DVG/VoiceStickPlus-anchor` 已切到该分支）。前置包含：批次 1 全部交付 +
> 「裸句号不剥」交叉修复（标点独词功能与句中剥离的交互，见 §0 末尾）。

你是本仓库（VoiceStick，Mac 桌面端 Swift + Windows 桌面端 C++ 双端同构）的开发者。
本任务书自包含；先读完「背景 → 约束 → 交付 → 红线 → 测试 → 流程」再动手。

## 0. 背景

批次 1（fix/voice-edit-anchor）已真机验收通过：语音编辑能在输入框内多级定位「最近一次语音输入」，
句中插入后替换/删除可用。用户验收后提出两个新需求：

1. **追问「第几个」**：目标词出现多处时，提示用户说第几个；用户紧接着说「第二个」时直接执行修改，
   而不是把「第二个」当文字上屏。现状两个问题：
   - 歧义提示后没有「等待回答」状态，下一句「第一个」被当普通文本粘贴；
   - 位置词只认「第一个/最后一个」（`VoiceTargetedEdit.swift` `consumePosition`），不认「第二个、第三个」。
2. **整个输入框可编辑**：修改不应只局限于最近一次语音输入。最近一次语音输入被手动改过后
   （批次 1 验收 C2：field_mismatch 拒绝），替换仍应可用；更早几轮说的话、手打的文字也应能改。

**安全论证（为什么整框可行）**：执行链的三道保护——AX 选区读回逐字确认、单次动作、执行后文本终验——
本身保证「改的就是此刻输入框里那个词」，不依赖注入记录。注入记录原本承担的是「作用域」与
「自然简写门控」，不承担定位正确性。整框模式下，目标偏移在计划时刻从当前字段全文现算。
**新增的真实风险是误触发**：口述内容恰好是「把X改成Y / 删除X」句式且X在输入框里出现过 →
会被执行成指令。缓解手段：撤销（交付 E）+ 配置开关（交付 A）。

**前置事实（已确认的决策）**：① 搜索范围＝整框、刚说的优先，配置开关默认开；② 撤销＝「撤销」
按上下文还原（30 秒内优先还原上一次语音修改，否则走原「删掉刚才整句」）；③ 尾删定位不到时
降级为删光标前的字/句（deleteAll 不降级）；④ 批次 1 已合并 main，本批从新 main 开分支。
另：main 已含「标点独词输入」（SpokenPunctuation）与「句中剥离不吞裸句号」修复——句中单独说
「句号」注入「。」不会再被剥成空串；本批任何新拦截（追问应答、撤销分流）都必须放在
finishWithFinalText 的正确位置，不得吞掉标点独词路径（finishWithRefinedText 收口，见
VoiceStickCoordinator 中 SpokenPunctuation.symbolIfSoleUtterance 的接入点与注释）。

## 1. 先决约束（与批次 1 相同，全部是硬约束）

1. 只在协调者指定的 worktree 内改动；不推送、不改 VERSION、不动 firmware/、不跑 build_win.bat。
2. 双端同构：纯逻辑层 Swift 在 `Sources/VoiceStickCore`（不依赖 AppKit），C++ 在
   `desktop/windows/src`（Google 风格、纯逻辑文件不 include Windows 头）；测试向量双端同表。
3. 本机 macOS CLT-only：Windows 平台代码（UIA/Win32）只能对照既有写法自查，标注
   `// TODO(win): 待 Windows 机编译+CTest 验证`；纯逻辑 .cc 必须 `clang++ -std=c++20 -fsyntax-only -Wall` 零警告。
4. 构建/测试：`cd desktop/macos && VOICESTICK_ARCHS=arm64 swift build && swift run VoiceStickTests`；
   全量 `scripts/verify-macos.sh`；守卫 `python3 scripts/release_guard.py`（i18n 三集合奇偶会查新增文案）。
   SwiftPM 拉依赖失败时前缀 `https_proxy=http://127.0.0.1:5782`。
5. `desktop/windows/` 整体被 .gitignore：新文件 `git add -f` 并 `git ls-files` 逐个验证。
6. 中文 commit，按交付拆分；每个 commit 独立可编译。
7. 日志纪律：不记目标词/替换词/字段内容，只记长度、计数、偏移、阶段与结果码。**提示 toast 可以显示目标词**
   （本地 UI，不落日志）。
8. 文档同步：`Doc/Plan/voice-edit-commands.md` §17 追加批次 2 记录（含行为变化清单）、
   `Doc/Ref/desktop-config.md`、`desktop/macos/Config/config.example.toml`、`CHANGELOG.md`、
   Localization.swift EN/ZH 双表、`settingsVoiceEditHint` 双语（更新为整框语义）。

## 2. 交付 A：搜索范围（domain）解析（已确认：整框，刚说的优先）

### 2.1 规则

| 指令形态 | 搜索域 | 唯一 | 多处 |
|---|---|---|---|
| 无作用域、无位置：「把X改成Y」「删除X」 | **Tier 1**：最近一次语音输入可定位（批次 1 定位器成功）且其中 X 恰好 1 处 → 直接改它；否则 **整框** | 执行 | 追问（交付 B） |
| 显式作用域「刚才的X」 | 最近一次语音输入（可定位时）；**不可定位**（被改过/歧义/过期/无记录）→ 退回整框 | 执行 | 追问，编号在该域内 |
| 无作用域 + 位置「把第二个X改成Y」 | 整框，按文档顺序（从上往下）数 | 执行 | — |
| 显式作用域 + 位置「把刚才的第二个X…」 | 最近一次语音输入内数（不可定位 → 整框） | 执行 | — |

- 自然简写门控（决定吞轮还是按普通文本上屏）：X 在最近一次语音输入里**或**整框里存在 → 接管；
  都不存在 → 现有本地对齐兜底（仍只针对最近一次语音输入，不扩到整框）→ 否则普通文本。
- **配置开关**：`[voice_edit].field_scope`，bool，默认 `true`，双端同名同默认；`false` = 完全回到
  批次 1 行为（只在最近一次语音输入里找）。
- 整框域对超长字段保护：`fieldText.utf16.count > 200_000` → 新失败码 `field_too_long`，
  提示「输入框文字过长，无法在整个输入框里修改」（最近一次语音输入域不受此限，沿用既有 2M 上限）。
- 整框域**不要求**存在注入记录、不检查 120s 时间窗、不检查「应用已切换」——作用对象是当前前台
  输入框本身；仍要求 voice_edit 开启、输出目标 focused_app、AX 权限、字段可读（读不到 = verify_unavailable）。

### 2.2 纯逻辑落点

- 位置限定扩展：`VoiceTargetedPosition` 增加 `.ordinal(Int)`（1 基，第N个）与 `.fromEnd(Int)`
  （倒数第N个；`.last` 等价 `.fromEnd(1)`，保留 `.last` 枚举位以免测试大面积改写，或统一迁移并更新向量——二选一，汇报说明）。
  解析器 `consumePosition` 支持：第N个、第N处、倒数第N个、最后一个；N 支持中文数字（一…九十九，含「十一」「二十」等）
  与阿拉伯数字 1–99。双端同构。
- 新纯函数（建议 `VoiceTargetedDomain.swift` / `voice_targeted_domain.{h,cc}`）：输入
  (command, remainingText?, anchor?, fieldText, fieldScopeEnabled) → 输出 `{domain, occurrences(文档序、绝对 UTF-16 偏移), selected?}`
  或 `{needsClarification(count, domain)}` 或失败码。所有规则表分支都在纯函数里，可单测。

## 3. 交付 B：追问「第几个」

### 3.1 状态机（协调器）

歧义时：存 `pendingClarification = {command, domain, occurrenceCount, appBundleID, createdAt, generation}`，
弹提示 **「找到 N 处「X」，请说第几个（如“第二个”）」**，显示 4 秒（`showTimedMessage(..., duration: 4.0)`）。
新 L10nKey `voiceTargetedClarifyPrompt`（格式串含 `%d` 与 `%@`，现有 `voiceEditText` 只支持单个 `%d`，按需新增格式化助手）。
替换旧 `voiceTargetedAmbiguous` 的使用点（旧 key 若无其他引用则删除，三集合同步）。

`finishWithFinalText` **最前面**（早于旧解析器 2028 行附近与 targeted 2031 行附近）新增
`handlePendingClarificationIfMatched(text)`，仅当 pending 存在且未过期（20 秒）且前台应用未变：

| 下一句 | 处理 |
|---|---|
| 序数应答（见 3.2） | 吞轮不上屏；**执行前重新读字段、在同一域内重算出现次数**：次数 == 存储值 → 执行第 N 个（走原 verified 链）；次数变化 → 提示「文本已变化，请重新说修改指令」，清 pending |
| 序号越界（只有 3 处却说第五个） | 吞轮，提示「只有 N 处，请说第 1 到第 N 个」，**保留** pending |
| 取消类（取消/算了/不改了/不用了） | 吞轮，提示「已取消修改」，清 pending |
| 其他任何内容（含新的完整指令） | 清 pending，**本句按正常链路处理**（不吞用户的正常说话） |

pending 清除时机：执行完成、取消、20 秒超时、任何新注入（`completePendingPaste`）、任何其他语音编辑指令、
前台应用变化、`updateConfig`。代际守卫：执行回调认 `voiceEditGeneration`。

### 3.2 序数应答解析（纯逻辑，双端同构）

新纯函数 `VoiceEditOrdinalReply.parse(text, target?) -> .ordinal(n) | .fromEnd(n) | .cancel | nil`：
- 接受：第N个 / 第N处 / 第N / 第N个X（X 与 pending 目标相同时）/ 倒数第N个 / 最后一个 / 最后那个 / 最后一处；
  **裸数字**（中文一…二十、阿拉伯 1–20，仅在 pending 状态下由协调器调用，误触面极小）；
- 前缀可带：就 / 选 / 是 / 就是 / 选择 / 要 / 改；尾部可带：吧 / 啊 / 呀 / 的 / 就行 / 那个；剥尾部标点；
- 取消：取消 / 算了 / 不改了 / 不用了 / 不要了；
- 其余返回 nil。

## 4. 交付 C：整框执行与光标、记录维护

- 整框域选区绝对偏移 = 目标 occurrence 的 UTF-16 起点（haystack = 字段全文）；读回逐字确认、单次动作、
  终验期望 = 字段全文按该 occurrence 替换/删除后的文本；终验轮询窗与 still_old 分类**沿用**批次 1。
- **光标恢复统一为「用户原来的位置 + 平移」**（所有域）：编辑后光标 = 原光标；原光标 ≥ 编辑区间终点 →
  加上长度差；原光标在编辑区间内 → 编辑后区间末尾；原光标 ≤ 起点 → 不变。纯函数实现 + 单测。
  这会改变批次 1 中「光标在段首/在别处」两种情形的恢复位置（原来移到段末），**属有意变化**，
  更新对应向量并在 §17 记录；**光标在末尾追加的情形结果与批次 1 完全一致**（回归底线）。
- 注入记录维护（整框域编辑后）：计划时刻若最近一次语音输入可定位——编辑区间完全落在其内 → 同步更新
  `remainingText`；完全在外 → 记录不变；跨越边界 → 记录作废（`lastInjection = nil`）。不可定位 → 不变。
- 不变：读不到字段拒绝、密码框（AX 值不可读）不碰、unverified 盲改永不放开。

## 5. 交付 D：尾删定位不到时按光标前删（已确认）

`VoiceEditPlanner.plan` verified 分支：定位器失败（field_mismatch / anchor_ambiguous）且
`selectionLength == 0` 且 `[voice_edit].field_scope == true` 时，降级为**光标前**操作：
- 「删掉最后N个字」= 删光标前 N 个字（grapheme 计数，clamp 到光标前可用字数，`max_chars` 纵深防御不变）；
- 「删掉最后N句」= 用既有 `trailingSentenceGraphemeCount` 对 `field[0..<caret]` 计算；
- 「删除刚才的/清空」（deleteAll）**不降级**（不知道该删什么），仍拒绝；
- 降级计划 `anchorEndUTF16 = caret`、`needsCaretMove = false`；注入记录不变；日志 `voice_edit tail_caret_fallback=1`。
- 同时修文案：尾删通道 `voiceEditFieldMismatch` 现文案「光标位置已变化，未删除」在批次 1 后语义不符，
  改为 **「刚才的输入已被修改，未删除」**（EN 同步）。

## 6. 交付 E：上下文撤销（已确认：「撤销」按上下文还原）

- 每次语音编辑成功（targeted 替换/删除、尾删）后记录 `lastVoiceEdit = {postEditFieldText, 还原所需的
  区间与原文本, preEditRemainingText?, appBundleID, createdAt, generation}`（尾删在退格完成回调里读一次字段得到 postEditFieldText；读不到则不记，撤销不可用）。
- 「撤销/撤回/撤销刚才的/撤回刚才的」（现为 deleteAll 白名单）在协调器分流：
  `lastVoiceEdit` 存在、≤ 30 秒、同一前台应用、且**当前字段全文 == postEditFieldText**（严格相等，宁拒勿错）
  → 执行还原：选中编辑后区间（删除类为折叠光标）→ 读回确认 → 粘贴原文本 → 终验 == 编辑前全文 →
  恢复 `remainingText` → 提示「已撤销修改」；字段已变化 → 提示「文本已变化，无法撤销」；
  不满足其余条件（无记录/超时/换应用）→ 走原 deleteAll 语义（行为不变）。
- 只支持一级撤销；撤销本身不可再撤销。日志 `voice_edit undo result=ok|failure code=…`。

## 7. 交付 F：Windows

- 纯逻辑全部同构：位置限定扩展、域解析、序数应答解析、光标平移、尾删光标降级、撤销判定（状态与比对部分）。
- 平台：`focus_text_reader.cc` 现只读字段末尾 512 码元（`kTailUtf16Limit`）。整框域需要**读取全文**
  （`GetDocumentRange` → `GetText`，上限 200,000 码元，超出按 field_too_long），并以绝对偏移设置选区。
  写好后标注 TODO(win)；尾部窗口相关既有逻辑保持可用。
- 协调器（`voice_stick_coordinator_tail.cc` 等）接入同构；`core_tests_voice_edit.cc` 同向量 + `_WIN32` Fake。

## 8. 安全红线（违反任一条返工）

1. targeted 任何 verify_mode 下必须 verified；读不到字段一律拒绝；不存在盲改路径。
2. 读回逐字确认、单次动作、文本终验、still_old 分类全部保留。
3. 匹配零容错：精确 UTF-16 匹配，不做模糊/最长前缀。
4. 追问应答只在 pending 有效期内生效；非应答内容一律放行按正常链路处理（绝不吞用户正常说话）。
5. 撤销仅在字段与编辑后状态严格一致时执行。
6. `field_scope = false` 时行为与批次 1 逐字节一致（回归开关）。
7. 光标在末尾追加 + 刚说那句里唯一匹配的常见情形，结果与批次 1 完全一致（回归底线）。

## 9. 测试（双端同向量）

纯逻辑：
- 位置解析：第二个/第2个/第十一个/倒数第二个/最后一个/刚才的第二个；越界与非法。
- 域解析全规则表：Tier 1 命中；Tier 1 不唯一转整框；注入不可定位转整框；显式作用域；位置限定；
  `field_scope=false` 回到旧行为；field_too_long。
- 序数应答：全部接受形态、前后缀、裸数字、取消、nil。
- 光标平移三情形 + emoji。
- 尾删光标降级：字/句/deleteAll 不降级/选区非空不降级/开关关不降级。
协调器级（CoordinatorFsmTests Fake 风格）：
- 追问全流程：歧义 → 提示含计数 → 「第二个」→ 执行第 2 处且该句不粘贴；越界保留 pending；取消；超时；
  非应答放行粘贴；重算次数变化拒绝；新注入清 pending。
- 整框替换：注入被手改后（批次 1 C2 场景）替换成功；手打文字（无注入记录）替换成功；记录维护三情形。
- 撤销：成功还原 + 记录恢复；字段变化拒绝；超时走 deleteAll 原语义。
- 回归：批次 1 全部协调器用例在 `field_scope=true/false` 下按预期（有意变化处更新并记录）。

## 10. 交付物与汇报

按 A→F 顺序实现，每个交付后跑一次 swift build + VoiceStickTests；最后 verify-macos.sh、release_guard.py、
clang 预检全过。汇报：每交付「改了什么/为什么/怎么验证」、所有**有意行为变化清单**、Windows 待编译清单、
新增文件 git ls-files 验证、测试计数、git log --oneline。无法完成的如实说明，不伪造验证结果。
