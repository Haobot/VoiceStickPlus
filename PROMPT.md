# 语音编辑批次 1 开发任务书——输入框内定位（P1）+ 可观测性/Electron（P4）+ 句中去句号

你是本仓库（VoiceStick，Mac 桌面端 Swift + Windows 桌面端 C++ 双端同构）的开发者。
本文件是自包含任务书；读完后按「先决约束 → 交付 → 安全红线 → 测试 → 流程」执行。

## 0. 你在哪里开发（先读，防串台）

- **开发目录（你的工作区，一切改动在这里）**：`/Users/hwp/Dev/DVG/VoiceStickPlus-anchor`
  （git worktree，分支 `fix/voice-edit-anchor`，基于 main，已含 cherry-pick 的
  `f81cd8b`、`b604e15` 两个修复提交——**不要动这两个提交**）。
- 主仓库 `/Users/hwp/Dev/DVG/VoiceStickPlus` 在别的分支上，**绝对不要**在其中改文件。
- 仓库根即 VoiceStickPlus；本 worktree 没有 untracked 生成物，状态干净。

## 1. 背景（为什么做，一分钟版）

用户在 Chromium 系目标（ZCode/Electron）与「句中插入」场景下，语音替换/删除大量失败，
弹「语音输入内容已变化，未修改」（field_mismatch）或「请先将光标移回末尾」
（caret_not_end）。2026-11-02 日志复盘确认结构根因：整条语音编辑链假设
「听写追加在字段末尾且其后无内容」——

- targeted planner L1 要求完整 remainingText 是**字段全文**后缀
  （`desktop/macos/Sources/VoiceStickCore/VoiceTargetedEdit.swift` 的
  `isFieldTailValid`，约 501 行；Windows 对应 `PlanVoiceTargeted` 的 tail 语义，
  `desktop/windows/src/voice_targeted_edit.h:110` 起）：句中插入 / 插入点后有字 →
  恒 `fieldMismatch`；
- 尾部删除 planner 要求 `caret == field.utf16.count`
  （`desktop/macos/Sources/VoiceStickCore/VoiceEditCommand.swift:482`；Windows
  `voice_edit_command.cc` 同语义）：句中插入后说「删除」恒 `caretNotEnd`。

用户真实工作流是「选中错词 → 句中重新口述 → 语音清残留」，与上述假设系统性冲突。
本批把「定位」从「字段末尾后缀」升级为「字段内多级锚定」，并让尾部删除能先把
光标移回去。**不做**最近多段输入作用域、**不做**动词词表扩展（后续批次）。

## 2. 先决约束（每条都是硬的）

1. **架构红线**（AGENTS.md）：交互状态机在桌面端；双端同构——同一语义改两端，
   纯逻辑层测试向量双端同表。Swift 在 `Sources/VoiceStickCore`（不依赖 AppKit）；
   C++ 在 `desktop/windows/src`，Google 风格，零平台依赖的纯逻辑 .cc 不 include
   Windows 头。
2. **本机是 macOS（CLT-only）**：Windows 平台代码（含 `#include <Windows.h>` 的
   `app_config.cc`、`localization.cc`、UIA 相关）**无法编译**；这些文件只做语法级
   自查（对照同文件既有写法），在汇报中标注「待 Windows 机编译验证」。纯逻辑 .cc
   必须 `clang++ -std=c++20 -fsyntax-only` 通过（先例：`ima_adpcm_decoder.cc`）。
3. **构建/测试命令**（在 worktree 的 `desktop/macos` 下）：
   - `VOICESTICK_ARCHS=arm64 swift build`（必须带 env；全 target）
   - `swift run VoiceStickTests`（executable runner，失败 exit 1）
   - 全量闭环：worktree 根 `scripts/verify-macos.sh`（build+tests+固件 host 单测）。
   - SwiftPM 依赖已缓存；若构建要拉依赖失败，命令前缀
     `https_proxy=http://127.0.0.1:5782`（以 `scutil --proxy` 为准）。
4. **守卫**：`python3 scripts/release_guard.py` 必须过。重点：桌面端 i18n 奇偶守护
   （L10nKey/StringId ⇄ EN 表 ⇄ ZH 表三集合双向一致——新增枚举必须三处同步，
   缺项静态检查直接红）；`Doc/Plan/` 下 md 必须有 `> 状态：…` 引用行（你要改的
   voice-edit-commands.md 已有，别删）。
5. **提交纪律**：中文 commit（`fix(语音编辑): …` 风格），按交付拆 commit；**不推送
   远端、不改 VERSION、不动 firmware/、不跑 build_win.bat**。
6. **`.gitignore` 整体忽略 `desktop/windows/`**：新增 Windows 文件必须
   `git add -f`，并用 `git ls-files <path>` 逐个验证已跟踪（历史教训 f75af4f5）。
7. **文档同步链**（AGENTS.md）：改配置字段 → `Doc/Ref/desktop-config.md` +
   `desktop/macos/Config/config.example.toml`；改提示文案 → Localization.swift 的
   EN/ZH 双表；设置页提示文案若语义变化 → `settingsVoiceEditHint` 双语同步更新；
   方案文档 `Doc/Plan/voice-edit-commands.md` §17 追加实施记录；`CHANGELOG.md`。
8. **日志纪律**：不记目标词/替换词/字段内容，只记长度、锚点种类、UTF-16 偏移、
   结果码。

## 3. 交付 A（核心）：字段内定位器——双端同构纯逻辑

### 3.1 语义（判定顺序固定，禁止任何模糊/最长前缀容错）

输入：`remaining`（LastInjection.remainingText）、`pressedEnter`、`field`（字段全文）、
`caretUTF16`、`selectionLength`（后三者读不到 = 不可验证，不进定位器）。
构造 `anchored = pressedEnter ? remaining + "\n" : remaining`（**调用方构造**，
定位器内不再拼换行）。判定顺序：

- P1a **光标前缀锚**：`selectionLength == 0` 且 `field` 的 UTF-16 前缀
  `[0, caret)` 以 `anchored` 结尾 → 命中；`anchorEnd = caret`。
- P1b **光标后缀锚**：`selectionLength == 0` 且 UTF-16 后缀 `[caret, len)` 以
  `anchored` 开头 → 命中；`anchorEnd = caret + anchored.utf16.count`。
- P1c **字段末尾锚**：`anchored` 是 `field` 的后缀 → `anchorEnd = field.utf16.count`
  （含现行「remaining+已记录\n」优先判语义，等价保留）。
- P2 **唯一出现锚**：P1 全 miss 且 `anchored` 在 `field` 中非重叠精确出现**恰好一次**
  → 命中该出现；出现 ≥2 次 → 失败 `anchor_ambiguous`。
- 全 miss（不存在）→ 失败 `field_mismatch`（保持现有 toast）。

匹配口径全部 **UTF-16 精确相等**（与平台选区 API 同口径）；图字符（emoji/组合字符）
不拆 surrogate。`field.utf16.count > kVoiceEditMaxVerifiableFieldUTF16`（现值
2,000,000）时调用方早已拒绝，定位器不重复设限。

### 3.2 Swift 落点

新文件 `desktop/macos/Sources/VoiceStickCore/VoiceEditAnchor.swift`：

```swift
public struct VoiceEditAnchor: Equatable {
    public enum Kind: Equatable { case caretPrefix, caretSuffix, fieldEnd, uniqueOccurrence }
    public let kind: Kind
    public let anchorStart: Int    // anchored 在字段中的起始 UTF-16 偏移
    public let anchorEnd: Int      // = anchorStart + anchored.utf16.count
}
public enum VoiceEditAnchorCode: String, Equatable {
    case anchorAmbiguous = "anchor_ambiguous"
    case fieldMismatch = "field_mismatch"
}
public enum VoiceEditAnchorResult: Equatable {
    case anchored(VoiceEditAnchor)
    case failure(VoiceEditAnchorCode)
}
public enum VoiceEditAnchor {
    /// field/caret/selection 为 nil 时调用方已按不可验证拒绝，不进入本函数。
    public static func locate(remaining: String, pressedEnter: Bool,
                              fieldText: String, caretUTF16: Int, selectionLength: Int)
        -> VoiceEditAnchorResult
}
```

### 3.3 C++ 落点

新文件 `desktop/windows/src/voice_edit_anchor.{h,cc}`（UTF-8 输入，内部换算 UTF-16
偏移；复用 `utf8_grapheme.h` 的解码工具，输出 UTF-16 偏移供 UIA 选区用）：

```cpp
namespace voicestick {
struct VoiceEditAnchor {
    enum class Kind { kCaretPrefix, kCaretSuffix, kFieldEnd, kUniqueOccurrence };
    Kind kind = Kind::kFieldEnd;
    std::size_t anchor_start = 0;  // UTF-16 偏移
    std::size_t anchor_end = 0;
};
enum class VoiceEditAnchorFailure { kAnchorAmbiguous, kFieldMismatch };
struct VoiceEditAnchorResult {
    bool ok = false;
    VoiceEditAnchorFailure failure = VoiceEditAnchorFailure::kAnchorAmbiguous;
    VoiceEditAnchor anchor;
};
// anchored 由调用方构造好传入（含已记录换行）。
VoiceEditAnchorResult LocateVoiceEditAnchor(std::string_view field_utf8,
                                             std::size_t caret_utf16,
                                             std::size_t selection_len,
                                             std::string_view anchored_utf8);
}
```

`CMakeLists.txt` 的 core 逻辑测试目标（171 行附近 `voice_edit_command.cc`、
175 行 `voice_targeted_edit.cc` 所在清单）加 `src/voice_edit_anchor.cc`；
`git add -f` + `git ls-files` 验证。

### 3.4 失败码接入现有枚举（双端同构追加，别插中间）

- macOS `VoiceTargetedFailureCode` 尾部追加 `anchorAmbiguous = "anchor_ambiguous"`
  （`caretMoveFailed` 见交付 B）；协调器映射到新 L10nKey。
- macOS 旧 `VoiceEditFailureCode`（尾删通道）同样追加 `anchorAmbiguous`、
  `caretMoveFailed`。
- Windows `VoiceTargetedFailure`/`VoiceEditFailure` 枚举与失败名表同步
  （`voice_targeted_edit.cc:357` 附近的 `FailureName` 表）。

### 3.5 用户文案（三处同步，守卫 N9 校验三集合）

- `voiceEditAnchorAmbiguous` / StringId `kVoiceEditAnchorAmbiguous`：
  zh「刚才的输入在文本里出现多次，请把光标移到它旁边再试」/
  en "That dictation appears multiple times; move the cursor next to it and try again"
- `voiceEditCaretMoveFailed` / `kVoiceEditCaretMoveFailed`：
  zh「无法把光标移到刚才输入的末尾，请手动移过去再试」/
  en "Couldn't move the cursor to the end of that dictation; move it there manually and try again"
- 同步更新 `settingsVoiceEditHint` 双语：去掉「『删掉最后几个字』仍需光标在文本末尾」
  措辞，改为「删掉最后几个字会自动把光标移回刚才输入的末尾」对应英文。

## 4. 交付 B：协调器接入（双端）

### 4.1 targeted 替换/删除（macOS `VoiceStickCoordinator.swift`）

- `evaluateVoiceTargetedCommand`（约 3214 行）：构造 anchored → 调定位器。
  失败码透传。成功后 `VoiceTargetedEditContext` 扩展字段：
  - `anchorKind`（Kind）
  - `anchorStartUTF16`（选区绝对偏移 = `anchorStart + occurrence.utf16Start`）
  - `textAfterAnchor: String`（计划时刻读到的 anchored 之后的内容——P1c 时为
    「0/1 个已记录换行」，P1a/P1b 时为实际段后文本，终验现场比对用）
  - 现行 `hasRecordedNewline`/`fieldPrefix` 拆解逻辑在 P1c 路径**原样保留**
    （回归底线）；P1a/P1b 路径用新字段构造期望终验文本：
    `expectedFinal = field[..<anchorStart] + newRemaining + textAfterAnchor + field[anchorEnd...]`
    （grapheme/UTF-16 换算注意用 `String.index` 系列，禁裸 Int 下标）。
- `executeVoiceTargetedEdit`（约 3274 行）与 `finalizeTargetedEdit`（约 3373 行）：
  选区偏移、恢复光标位置（P1a/P1b 下恢复到 anchorEnd，P1c 保持字段末尾）、终验
  期望文本按 context 新字段。读回确认（`verifyTargetedSelectionThenAct`）与终验
  分类（still_old/真不符）逻辑**不动**。
- planner 纯逻辑（`VoiceTargetedPlanner.plan`）保持签名兼容：新增带锚点的重载
  `plan(command:remainingText:pressedEnter:anchor:fieldText:caretUTF16:selectionLength:)`，
  旧签名（校验 field 后缀）委托到定位器 P1c 等价实现，现有测试不动仍须全绿。

### 4.2 尾部删除通道（「删除/删掉最后 N 字/句/整段」）

- planner（`VoiceEditCommand.swift:442` `plan`）语义升级：定位成功 →
  `effectiveText` 语义改为「anchored 段内容」，`newlineCount` 仍只算已记录换行
  （pressedEnter && anchored 含 \n）；**caret==end 检查删除**，改为产出计划带
  `anchorEndUTF16` + `needsCaretMove: Bool`（caret != anchorEnd 时 true）。
  P1c 且 caret==end 时计划与现行**逐字段等价**（回归底线）。
- 协调器 `handleVoiceEditCommandIfMatched` 的 plan 分支（约 2964 行）：注入退格前，
  `needsCaretMove` → 用 `writeTargetedSelection(anchorEnd, 0)` 移光标，按
  `kVoiceTargetedReadbackPollDelays`（`[0.03,0.06,0.12,0.2]`）轮询
  `readFocusedFieldState` 确认 `caretUTF16 == anchorEnd && selectionLength == 0`；
  耗尽 → `caret_move_failed`，零退格，记录保留。到位后退格路径照旧
  （`injectVoiceEditBackspaces` + `lastInjection` 更照旧）。
- unverified 降级路径（strict/auto/off 语义）：**不变**——unverified 时无字段可读，
  无从定位，行为保持现行（盲退格窗口照旧）；这条红线不放松。

### 4.3 Windows 同步

`voice_stick_coordinator_tail.cc` / `voice_stick_coordinator.cc` 的纯逻辑部分
（定位器调用、计划构造、失败分类、`needsCaretMove` 状态）同步实现；UIA 移动光标
（MoveCaretToEnd 的泛化：MoveCaretTo(utf16)）代码写好，标注
`// TODO(win): 待 Windows 机编译+CTest 验证`。`core_tests_voice_edit.cc` 的
Fake 桩同步扩展。

## 5. 交付 C：句中插入去句末句号（新配置）

- 配置：`[output].midline_strip_period`，bool，默认 **true**，双端同名同默认。
  - macOS：`ConfigFileModels.swift` `[output]` 表加 `midline_strip_period: Bool?`
    （同 `auto_enter` 模式，35 行附近）+ `AppConfig.swift` 字段/默认/回写/两处
    解析（270、491 行附近模式）。
  - Windows：`app_config.cc` `[output]` 表 TomlBool 解析（822 行附近）+
    `app_config.h` 字段+默认。
  - 文档：`Doc/Ref/desktop-config.md` `[output]` 段 + `config.example.toml`。
- 判定与作用点（`completePendingPaste` 入口，`VoiceStickCoordinator.swift:2887`）：
  - 仅 `outputProfile(for:).target == .focused_app` 且配置开；读一次
    `readFocusedFieldState`；**可读** 且 `selectionLength == 0` 且光标后**同一逻辑行**
    还有非空白字符（即光标后第一个字符存在、不是 `\n`/`\r`、非空白）→ 句中插入。
  - 句中插入且注入文本以一个句号结尾 → 去掉末尾一个字符。句号集合：`。`(U+3002)、
    `．`(U+FF0E)、`.`(U+002E，**仅当其前一个字符不是 ASCII 数字/字母**，保 `3.`、
    `v1.`)。`！？`等语气标点**不去**。
  - 读不到字段（不可验证应用）→ 原样注入，不处理（零行为变化）。
  - 处理发生在送 `inputInjector.paste`、`completeSessionHistory`、`beginEditTracking`、
    `lastInjection` 记录**之前**——历史库/编辑追踪/语音编辑作用域记录的都是处理后
    文本（三处一致，避免「记录的文本和字段里的不一致」重新制造 field_mismatch）。
    实现为纯函数 `stripMidlinePeriod(text:isMidline:) -> String`（放
    `VoiceEditAnchor.swift` 或独立文件，双端同构 `StripMidlinePeriod`），带单测。
  - auto_enter 换行不受影响（只剥句号字符，`pressedEnter` 照旧）。
  - **注意**：翻译模式（transform=translate）产物以句号结尾是正常语义，本逻辑
    同样适用（判定只看插入位置，不看模式）——在文档与测试向量中各留一个翻译样本。
- Windows：`CoordinatorTail` 注入收口处同构接入（纯函数 + 配置读取），平台读取
  部分标注待编译。

## 6. 交付 D：可观测性 + Electron AX 支持（macOS 为主）

- **失败日志带上下文**：targeted 与尾删的失败路径（`showVoiceTargetedFailureToast`
  与 `showVoiceEditFailureToast` 的调用点之前）统一打一行：
  `voice_edit fail_stage=<stage> bundle=<bundleID|unknown> code=<code>`；
  stage ∈ {`focused_element`, `field_text`, `selected_range`, `selected_text`,
  `anchor`, `caret_move`}（对应读取链的失败环节；定位器语义失败 stage=`anchor`）。
  bundle id 用现有 `frontmostAppBundleID?()`，nil → `unknown`。**只在失败路径打**
  （成功路径不加日志）；内容纪律不变。
- **Electron AX 三级回退**（`FocusedFieldAXReader.focusedElement`，
  `AppDelegate.swift:1091`）：system-wide → 前台应用两级都失败时，第三级：
  对前台应用元素读 `AXManualAccessibility`；值为 false 或不可读时，用
  `AXUIElementSetAttributeValue(appElement, kAXManualAccessibility as CFString,
  kCFBooleanTrue)` 设置后**重试一次**聚焦元素读取；结果打日志
  `voice_edit ax_manual=0/1 bundle=…`（设置失败 ax_manual=0）。调用形态照抄同文件
  `writeSelectedRange` 里 `AXUIElementSetAttributeValue` 的既有写法（3 参直调），
  不要自造 dlsym 包装。**每轮注入至多重试一次**（静态缓存「已开」bundle，避免
  每次读失败都设一遍；缓存只在 app 进程内存，不落盘）。
- Windows：仅纯逻辑可达的 stage 日志点（`fail_stage=anchor` 等），AX 部分不涉及。

## 7. 测试（双端同向量；写进测试文件并跑绿才算数）

纯逻辑（macOS 新 `VoiceEditAnchorTests.swift` + `VoiceTargetedEditTests.swift`/
`VoiceEditCommandTests.swift` 扩展；Windows `core_tests_voice_edit.cc` 同向量）：

- T1 光标紧邻段尾（P1a）：句中插入替换成功，选区/终验偏移正确
- T2 光标紧邻段首（P1b）：同上
- T3 字段末尾追加（P1c）：与现行行为**逐字段断言等价**（回归底线）
- T4 段唯一出现（P2）：光标在别处，替换成功
- T5 段出现两次且 P1 全 miss：`anchor_ambiguous`
- T6 用户改过段内文本（整段精确匹配全 miss）：`field_mismatch`（保留旧 toast）
- T7 pressedEnter 已记录换行 + P1a：anchored 含 \n 语义正确
- T8 anchored 自身以 \n 结尾（口述多行）：不误并前缀（对齐 §17.5 返修 4）
- T9 尾删：光标不在 anchorEnd → 先移后删；移动失败 → `caret_move_failed` 零退格
- T10 光标已在 anchorEnd：退格计划与现状逐字段等价（回归）
- T11 替换目标跨 surrogate 对（emoji）：UTF-16 偏移正确
- T12 unverified/off 下 targeted：仍拒绝（红线）
- T13 句中句号：标准剥离；`！？`保留；`3.`/`v1.` 的 `.` 保留；非句中不剥；
  读不到字段不处理；配置 false 关闭；翻译模式句中样本
- T14 Electron 回退（协调器级 Fake）：首读 nil → 设置 AXManualAccessibility 后
  二次读成功 → 功能恢复；设置成功路径不重复设置（缓存生效）
- T15 失败日志行（Fake 日志收集）：`fail_stage`/`bundle` 字段存在

协调器级（`CoordinatorFsmTests.swift`，Fake 闭包风格照 434-439 行既有写法）：
P1a 替换全链路调用顺序（定位 → 选区 → 读回 → 动作 → 终验 → 记录更新）、
尾删光标移动失败零退格、T3 回归底线完整链路断言、句号剥离注入点（注入文本/
历史记录/lastInjection 三处一致断言）。

## 8. 流程

1. 按交付 A → B → C → D 顺序实现，每交付完成即跑：
   `cd desktop/macos && VOICESTICK_ARCHS=arm64 swift build && swift run VoiceStickTests`
2. 全量：worktree 根 `scripts/verify-macos.sh`；`python3 scripts/release_guard.py`。
3. Windows 纯逻辑：`clang++ -std=c++20 -fsyntax-only` 逐个过你新增/改动的
   无平台依赖 .cc/.h；行为对拍用小程序 main + 与 Swift 向量对数（先例：
   `pcm_postprocessor.cc` 预验证法）。
4. 文档与 CHANGELOG 同步（§2 第 7 条清单）。
5. 汇报格式（最后一并输出）：每个交付一节「改了什么/为什么/怎么验证」；
   verify-macos.sh 与 release_guard.py 输出摘要；Windows 待编译项清单；
   新增文件清单（含 git ls-files 验证结果）；测试计数（新增 N 个用例、总数 M）。

## 9. 汇报前的自检清单（逐条勾选）

- [ ] cherry-pick 的 f81cd8b、b604e15 仍在历史中未被改写
- [ ] P1c 路径回归：字段末尾追加场景行为与改动前逐字节等价（T3+T10 链路断言）
- [ ] 定位器零容错：无最长前缀/模糊匹配；`anchor_ambiguous`/`field_mismatch` 路径齐
- [ ] 尾删移动失败零退格；unverified 语义未变
- [ ] 新配置三份文档 + 双端解析 + 默认 true
- [ ] 新文案 L10nKey/StringId ⇄ EN ⇄ ZH 三处同步（守卫会查）
- [ ] settingsVoiceEditHint 双语已更新（不再宣称「仍需光标在文本末尾」）
- [ ] Windows 新文件 `git add -f` 且 `git ls-files` 全部可见
- [ ] verify-macos.sh 全绿；release_guard.py 全过；clang 预检过
- [ ] 日志无文本内容泄漏；失败路径有 fail_stage/bundle
