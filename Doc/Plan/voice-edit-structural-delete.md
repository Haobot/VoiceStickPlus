# 语音编辑：光标结构化删除（这句话 / 这一段 / 这一行）

> 日期：2026-11-05。基线：`df3bde9`（`domain_mode=whole_field` + 批量替换）。
> 需求方：用户。本文只做方案，不含实现。

## 1. 需求

现有删除能力都绑定「语音输入的内容」：

- 旧指令「删除/删掉最后 N 个字/句」只在**最近一次语音注入**的 remaining 里删；
- targeted「删除 X」要求 X 作为**目标词**存在（最近注入域或整框域）。

期望新增**位置相对**的结构化删除，不再依赖语音输入：

- 「**删掉这句话**」→ 删除**光标所在句子**（上一个句末标点到下一个句末标点之间）；
- 「删掉这一段」→ 删除光标所在段落；
- 「删掉这一行」→ 删除光标所在行；
- 语义与「是不是语音输入的」完全解耦，任意光标位置可用。

## 2. 现状

| 能力 | 现状 |
|---|---|
| 删最后 N 字/句 | `VoiceEditAction::kDeleteLastChars/kDeleteLastSentences`，基于 `remaining_text`（最近注入） |
| 删整段（清空刚才的） | `kDeleteAll`，基于 `remaining_text` |
| targeted「删除 X」 | 需目标词存在（`FindTargetedOccurrences`） |
| 光标前 N 字/句降级 | `PlanVoiceEdit` 的 `caret_fallback`（批次 2 交付 D）——仍是「光标前 N 个」，不是「所在句」 |
| 光标绝对偏移 | `VoiceEditFieldState::caret_abs_utf16`（UIA 可读；**剪贴板通道不可得**） |

**结论**：缺的是「按光标位置找句子/段落/行边界并删除」这一族；执行链（绝对选区/整框粘贴/终验）已完备。

## 3. 方案

### 3.1 新命令族（解析层）

`VoiceEditAction` 尾部追加（双端同构，别插中间）：

```cpp
enum class VoiceEditAction {
    ...
    kDeleteSentenceAtCaret,   // 删掉这句话
    kDeleteParagraphAtCaret,  // 删掉这一段
    kDeleteLineAtCaret,       // 删掉这一行
};
```

新增独立解析函数（不混入 targeted 白名单，避免与「删除 X」争抢）：

```cpp
enum class VoiceEditCaretDeleteKind { kSentence, kParagraph, kLine };
std::optional<VoiceEditCaretDeleteKind> ParseCaretDeleteCommand(std::string_view text);
```

白名单（整句精确匹配，归一化后）：
- 句子：`删掉这句话` / `删除这句话` / `删掉这一句` / `删除这一句` / `删掉这句` / `这一句删掉`
- 段落：`删掉这一段` / `删除这一段` / `删掉这段` / `这一段删掉`
- 行：`删掉这一行` / `删除这一行` / `删掉这行` / `这一行删掉`

### 3.2 定位（纯逻辑）

```cpp
// 光标所在文本区间（UTF-16，[start, end)；end 不含句末标点后的换行）。
struct VoiceEditTextRange {
    std::size_t start_utf16 = 0;
    std::size_t end_utf16 = 0;
    bool valid = false;
};

VoiceEditTextRange FindSentenceAtCaret(std::string_view field_full_utf8,
                                       std::size_t caret_abs_utf16);
VoiceEditTextRange FindParagraphAtCaret(std::string_view field_full_utf8,
                                        std::size_t caret_abs_utf16);
VoiceEditTextRange FindLineAtCaret(std::string_view field_full_utf8,
                                   std::size_t caret_abs_utf16);
```

规则：

| 类型 | 边界 |
|---|---|
| 句子 | 句末标点：`。！？；` + 英文 `.!?;`；句首 = 上一个句末标点之后（或全文起点）；句尾 = 下一个句末标点**含标点**。光标恰在标点后 → 归属下一句（若下一句非空） |
| 段落 | 空行分隔（`\n\n`，容忍 `\n \n` 等空白行）；段首 = 上一个空行之后；段尾 = 下一个空行之前（不含空行） |
| 行 | `\n` 分隔；行首 = 上一个 `\n` 之后；行尾 = 下一个 `\n` 之前（不含换行符） |

边界：光标越界 / 全文为空 / 区间为空 → `valid=false`（拒绝）。

### 3.3 执行（复用整框批量粘贴）

定位出 `[start, end)` 后，**构造单区间的批量删除**，直接复用已有执行链：

```cpp
TargetedExecutionContext exec;
exec.absolute_coords = true;
exec.is_batch = true;
exec.batch = { VoiceTargetedOccurrence{ .utf16_start = start,
                                        .utf16_length = end - start } };
exec.pre_action_text = field_full;
exec.domain = VoiceTargetedDomainKind::kWholeField;
exec.record_action = RecordAction::kInvalidate;  // 手改文本，旧注入记录作废
TryWholeFieldPasteRewrite(command, VoiceTargetedPlan{}, exec, generation);
```

- `TryWholeFieldPasteRewrite` 的批量分支已实现（`BuildWholeFieldBatchReplacement` + `ReplaceAllWithPaste` + 全文终验）；
- **零新执行代码**；终验、失败拒绝、清空拒绝（决策 2）全部沿用。

### 3.4 入口与优先级

`HandleVoiceEditIfMatched`（旧解析器）**之前**插入：

```cpp
if (HandleCaretDeleteIfMatched(text)) return;   // 新：这句话/这一段/这一行
if (HandleVoiceEditIfMatched(text)) return;     // 旧：删最后 N 字/句
if (HandleVoiceTargetedEditIfMatched(text)) return;  // targeted：删除 X
```

优先级理由：新指令是**位置相对**语义，与「删最后 N 字」的**数量相对**语义互不重叠（白名单精确匹配，无歧义）。

### 3.5 微信（剪贴板通道）限制

微信等无 UIA 窗口的 `caret_abs_utf16` 不可得：

- 「删掉这句话/这一段/这一行」→ **拒绝并提示**「当前位置无法定位光标，请用『删除最后一句』」；
- **不降级**为「删最后一句」——位置语义不同，静默替换会误删，违反「不盲改」。

## 4. 安全边界

| 情况 | 处理 |
|---|---|
| 光标不可得（微信/无 UIA） | 拒绝并提示（零写入） |
| 定位区间为空（空文本/光标越界） | 拒绝（零写入） |
| 删除后字段为空（全文仅这一句） | 拒绝并提示（沿用「不支持删除全部内容」决策） |
| 终验不符 | `result_unconfirmed`（沿用） |
| 密码框 / 读不到全文 | 拒绝（沿用） |

## 5. 影响面

| 模块 | 改动 |
|---|---|
| `voice_edit_command.h/.cc` | 3 个新 action + `ParseCaretDeleteCommand` + 3 个定位函数（纯逻辑） |
| `voice_stick_coordinator_tail.cc` | 新入口 `HandleCaretDeleteIfMatched`（定位 + 构造 batch + 调 `TryWholeFieldPasteRewrite`） |
| `voice_stick_coordinator.cc` | 调用顺序插入新入口 |
| 测试 | 解析白名单正/负向、三种定位边界向量、协调器集成（正常/无光标/删空拒绝） |
| 文档 | `Doc/Plan/voice-edit-commands.md` 指令表、`Doc/Ref/desktop-config.md` 说明 |

## 6. 实施步骤（TDD）

1. 定位层：`FindSentenceAtCaret` / `FindParagraphAtCaret` / `FindLineAtCaret`（纯逻辑 + 向量表）；
2. 解析层：`ParseCaretDeleteCommand`（白名单 + 负向：普通说话不吞）；
3. 协调器入口：`HandleCaretDeleteIfMatched`（读全文 + 光标 → 定位 → batch → 整框粘贴）；
4. 集成测试：ZCode/记事本正常删除、微信无光标拒绝、删空拒绝；
5. 真机验收 + 文档。

## 7. 决策（已定稿）

1. **句子边界包含句末标点**（「北京很好。」整体删除，不留孤立句号）；
2. **段落以空行（`\n\n`）为分隔**（单换行视为段内换行）；
3. **「删掉这一行」连同行尾换行一起删**（避免留下空行）；
4. **「删掉这句话」允许删空**（2026-11-05 修订）：整段只有一句（无句末标点）时
   删空是用户意图；批量「全部删掉」仍拒绝并提示「不支持删除全部内容」。
