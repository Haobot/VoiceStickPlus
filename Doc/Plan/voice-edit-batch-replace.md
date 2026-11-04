# 语音编辑批量替换/删除：全部 与 范围

> 日期：2026-11-04。基线：`ef857b7` + 本轮 `domain_mode=whole_field`（未提交）。
> 需求方：用户。本文只做方案，不含实现。

## 1. 需求

targeted 编辑（替换/删除指定词）当前只能处理**一处**匹配（首/末/第 N/倒数第 N），
多处时追问「第几个」。期望支持批量：

1. 「全部」→ 替换/删除**所有**匹配；
2. 「第一个到第三个」「第二个到倒数第三个」→ 替换/删除**指定范围**内的匹配。

## 2. 现状

- 位置模型（`voice_targeted_edit.h:40-49`）：
  `kFirst` / `kLast` / `kOrdinal` / `kFromEnd` / `kUnspecified`（仅 1 处才允许）；
- 选择逻辑（`SelectTargetedOccurrence`）输出**单个** `selected`；多处且未指定位置 →
  `kAmbiguous` → 追问「第几个」；
- 计划（`PlanVoiceTargeted` / `PlanVoiceTargetedAnchored`）基于单个 occurrence；
- 整框重写（`BuildWholeFieldReplacement`）替换**单个**区间；
- 执行链：选区读回确认 → 单次动作（退格或粘贴）→ 全文终验；
- 已存在旧指令「删除全部/清空」（`deleteAllForms`）——那是**删掉整个字段**，
  与本次「批量替换多个目标词」无关。

## 3. 方案

### 3.1 语法（解析器）

在 `ParseVoiceTargetedCommand` 的位置解析里新增：

| 说法 | 语义 |
|---|---|
| `全部` / `所有` / `都`（位于目标词前或后） | `kAll` |
| `第一个到第三个` / `第一到第三` | `kRange`（正序 A..B） |
| `第二个到倒数第三个` / `第二到倒数第三` | `kRange`（正序 A .. 倒数 B） |
| `倒数第二个到倒数第一个` | `kRange`（倒数 A .. 倒数 B） |

- 数字解析复用 `ParseVoiceEditOrdinalNumber`（中文/阿拉伯）；
- 范围端点用**有符号**表示：`>0` 正序 1 基，`<0` 倒数（`-1` = 倒数第 1）；
- 「到」的分隔词：`到` / `至` / `-`（口述少用，先只支持前两个）；
- 与既有单点语法互斥：`第一个到第三个` 优先于 `第一个`。

### 3.2 数据模型

```cpp
enum class VoiceTargetedPosition {
    kFirst, kLast, kOrdinal, kFromEnd, kUnspecified,
    kAll,     // 全部匹配
    kRange,   // 范围（range_start/range_end 有效）
};

struct VoiceTargetedCommand {
    ...
    int range_start = 0;  // kRange：1 基正序；负数 = 倒数（-1 = 倒数第 1）
    int range_end = 0;
};
```

域解析结果扩展（`voice_targeted_domain.h`）：

```cpp
struct VoiceTargetedDomainOutcome {
    enum class Kind { kExecute, kClarify, kOutOfRange, kFailure };
    ...
    // 批量（kAll / kRange 命中 ≥1 处）：按文档顺序的全部目标区间。
    // 非空时优先于 selected（单点语义不变）。
    std::vector<VoiceTargetedOccurrence> batch;
    bool is_batch = false;
};
```

### 3.3 执行（批量走整框粘贴）

批量天然不适合「逐个选区 + 逐个动作」（偏移会变、每步都要读回）。改为：

1. 由 `batch` 构造**多区间替换**的全文：
   `BuildWholeFieldBatchReplacement(field_full, ranges, replacement)`——
   从**后往前**替换（或一次遍历拼接），保证偏移不受影响；
2. 走既有整框粘贴（`ReplaceAllWithPaste`，Ctrl+A + Ctrl+V）——语义固定、
   失败无损；
3. 终验：全文精确比对（现有 `FinalizeVoiceTargetedEdit` 的整框域分支）。

单点路径（现有 `SelectTailRange`/`SelectAbsoluteRange` + 单次动作）**保持不变**。

### 3.4 安全边界（不放松）

| 情况 | 处理 |
|---|---|
| 「全部」匹配数为 0 | 拒绝（not_found），零写入 |
| 「全部」匹配数超过上限（建议 **50**） | 拒绝并提示「匹配太多（N 处），请指定范围」——防「的/了」这类高频词误伤 |
| 范围越界（起点/终点超过匹配数，或起点 > 终点） | 拒绝（position_out_of_range），零写入 |
| 范围解析出 0 处 | 拒绝 |
| 批量删除后字段为空 | **需决策**（见 §6）：要么拒绝「不支持删空」，要么允许清空 |
| 终验不符 | `result_unconfirmed`（现有语义） |
| 密码框 / 读不到全文 | 拒绝（现有语义） |

### 3.5 交互（追问应答）

`ParseVoiceEditOrdinalReply` 扩展：pending 追问时允许应答

- `全部` / `所有` / `都` → `kAll`
- `第一个到第三个` / `第二到倒数第三` → `kRange`

问句文案不变（「有 N 处「X」，第几个？」），但用户可直接说「全部」。

### 3.6 双端

macOS 的 `VoiceTargetedEdit.swift` / `VoiceTargetedDomain.swift` 同构：同一张解析
白名单、同一份测试向量。本次先 Windows，macOS 列入下一轮同步（与
`domain_mode` 同一批）。

## 4. 影响面

| 模块 | 改动 |
|---|---|
| `voice_edit_command.cc` | 位置语法（全部/范围）+ 追问应答扩展 |
| `voice_targeted_edit.h/.cc` | 枚举 + `range_*` + 批量选择 + 多区间替换构造 |
| `voice_targeted_domain.h/.cc` | 结果加 `batch` + 范围解析 |
| `voice_stick_coordinator_tail.cc` | 批量分支 → 整框粘贴路径（跳过单点选区） |
| `voice_stick_coordinator.cc`（clarify） | pending 应答支持批量 |
| 测试 | 解析向量 + 选区/范围向量 + 协调器集成（全部/范围/超限/越界） |
| 文档 | `Doc/Plan/voice-edit-commands.md` §2.1 规则表、`Doc/Ref/desktop-config.md` |

## 5. 实施步骤（TDD）

1. **解析层**（纯逻辑）：
   - `kAll` 关键词（全部/所有/都）→ 单测正/负向（「全部删除」旧指令不冲突）；
   - `kRange`（第 A 到第 B、A/B 支持倒数）→ 单测端点解析、非法形态回退普通文本；
2. **选择层**：`SelectTargetedBatch`（返回 batch + 越界/超限错误）→ 单测向量；
3. **替换构造**：`BuildWholeFieldBatchReplacement` → 单测（重叠/相邻/首尾/删除）；
4. **协调器**：批量分支走整框粘贴 → 集成测试（全部替换、范围替换、超限拒绝、越界拒绝）；
5. **追问应答**：`ParseVoiceEditOrdinalReply` 支持全部/范围 → 单测；
6. **文档 + 双端同步**。

## 6. 决策（已定稿）

1. **「全部」匹配数上限 = 50 处**（超过拒绝并提示「匹配 N 处，请指定范围」）；
2. **批量删除导致字段清空 → 拒绝并提示**「不支持删除全部内容」（零写入）；
3. **范围语法**：在「第A个到第B个」基础上**额外支持**：
   - 「从第一个到第三个」→ `kRange(1, 3)`；
   - 「头三个」「前三个」→ `kRange(1, 3)`；
   - 「最后三个」→ `kRange(-3, -1)`。
