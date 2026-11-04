# 语音编辑作用域扩展：整框检索 + 取消时间限制

> 日期：2026-11-04。基线：`ef857b7`（微信剪贴板回读已落地）。
> 需求方：用户。本文只做方案，不含实现。

## 1. 需求

当前 targeted 编辑（替换/删除指定词）只在「最近一次语音输入」范围内检索，且受
`[voice_edit].scope_seconds` 时间窗限制。期望：

1. **检索范围 = 光标所在文本框全文**（文档/输入框任意位置），不再优先最近注入域；
2. **取消时间限制**：几分钟前输入的内容也能替换/删除；
3. 安全语义不变：不伪造成功、读不到全文拒绝、多处歧义追问「第几个」。

## 2. 现状分析

### 2.1 已有能力（无需新增）

`[voice_edit].field_scope = true`（默认）时已实现**整框域**（批次 2）：

- 读全文 → `ResolveVoiceTargetedDomain` → 整框定位 → 绝对坐标选区 / 整框粘贴 →
  全文终验；
- 多处匹配 → `kClarify` 追问「第几个」；超长（> `kVoiceTargetedFieldScopeMaxUtf16`
  = 200k UTF-16）→ `field_too_long` 拒绝；
- **微信等无 UIA 窗口已经走整框**：剪贴板通道 `caret_abs` 不可得 → `recent` 恒为
  `nullopt` → 整框域，**天然无时间限制**。

### 2.2 差距（两处）

`ResolveVoiceTargetedDomain` 规则表 Tier 1（`voice_targeted_domain.h:73-81`）：

> 无作用域无位置：Tier 1（recent 可用且 remaining 内恰 1 处）→ lastInjection；否则整框。

recent 域构建条件（`voice_stick_coordinator_tail.cc:1360-1387`）：

- `caret_abs` 可读（UIA 目标）；
- `last_injection_` 存在且 `remaining_text` 非空；
- `elapsed <= voice_edit.scope_seconds`（**默认 120 秒**；本机若为 30 秒则是
  `config.toml` 手改过）；
- 同前台窗口/进程 + `LocateVoiceEditAnchor` 锚定成功。

→ ZCode / 记事本等 UIA 目标上：刚说完的话在时间窗内 → 只改「刚说的」；超时 → 整框。

**结论：只缺两件事**——① 允许「始终整框」的域优先级开关；② 该模式下不构建 recent，
`scope_seconds` 自然不参与（整框域本身无时间窗）。

## 3. 方案

### 3.1 新增配置项

`[voice_edit].domain_mode`（枚举字符串）：

| 值 | 行为 |
|---|---|
| `whole_field`（**建议新默认**） | 不构建 recent 域，始终在光标所在文本框全文检索；**无时间限制** |
| `recent_first`（原行为） | Tier 1 优先「最近一次语音输入」，`scope_seconds` 时间窗生效 |

- 解析/序列化：`app_config.cc`（`TomlString` 解析 + `output <<` 回写）；
- **启动归一化**：`domain_mode == whole_field` 时强制 `field_scope = true`（整框能力
  依赖批次 2；`field_scope=false` 是旧路径回归开关，与整框语义冲突）；
- 配置模板：`desktop/macos/Config/config.example.toml`；
- 文档：`Doc/Ref/desktop-config.md`。

### 3.2 协调器改动（Windows）

`TakeOverTargetedCommand`（`voice_stick_coordinator_tail.cc`）：

```cpp
const bool whole_field_only =
    voice_edit.domain_mode == VoiceEditDomainMode::kWholeField;
std::optional<VoiceTargetedRecentDomain> recent;
if (!whole_field_only && !caret_missing && last_injection_.has_value() &&
    !last_injection_->remaining_text.empty()) {
    // 原 recent 构建逻辑（含 scope_seconds 检查）保持不变
}
const auto domain_outcome =
    ResolveVoiceTargetedDomain(command, recent ? &*recent : nullptr, field_full);
```

- `whole_field_only` → `recent` 恒 `nullopt` → 规则表走整框分支；`scope_seconds`
  对 targeted 不再生效；
- **不受影响的路径**（有意保留）：
  - 普通「删最后 N 个字」（`voice_stick_coordinator_tail.cc:963`）的 L0 时间窗——
    那是「刚说的话」语义，不属于本次需求；
  - 批次 1（`field_scope=false`）路径；
  - undo 路径（`:2050`）。

### 3.3 macOS 同步

`desktop/macos` 的 `VoiceTargetedDomain` 与 Windows 同构（同一张规则表、同一份测试
向量）。同步新增同名配置与分支，保证双端行为一致；默认值同 `whole_field`。

## 4. 影响面与风险

| 场景 | 变化 |
|---|---|
| 刚说完一句，改其中的词（整框内该词唯一） | 直接改（与旧行为结果通常相同） |
| 整框内同一词多处 | **追问「第几个」**（现有逻辑必须保留，否则无法安全定位） |
| 输入框里有与「刚说过的话」无关的目标词 | 会被检索并修改（符合新需求；旧行为不会） |
| 超长文档（>200k UTF-16） | `field_too_long` 拒绝（不变） |
| 密码框 / 读不到全文 | 拒绝（不变） |
| 微信等无 UIA 窗口 | 行为不变（本来就走整框） |
| 时间限制 | targeted 取消；普通删除指令不变 |

**安全边界零放宽**：读全文 → 定位 → 改 → 全文终验；任何环节失败即拒绝，不盲改。

**性能**：整框域每次都要读全文（微信已是此路径）；ZCode/记事本由 UIA 全文读承担，
200k 上限内可接受。

## 5. 实施步骤（TDD）

1. **配置层**：`AppConfig::VoiceEdit::domain_mode`（枚举 + 解析 + 序列化 + 默认值）
   单测：默认值、`whole_field`/`recent_first`/非法值回退、回写格式；
2. **协调器**：`TakeOverTargetedCommand` 的 recent 构建加开关
   单测（fake reader/`VoiceTargetedHarness`）：
   - `whole_field` + recent 本可构建 → 仍整框（全文字段含两处目标 → clarify）；
   - `whole_field` + 注入时间超过 `scope_seconds` → 仍能编辑；
   - `recent_first` → 现有用例全部回归不变；
3. **macOS 同步**（同配置 + 同规则 + 同测试向量）；
4. **文档**：`Doc/Ref/desktop-config.md`、配置模板、`Doc/Plan/voice-edit-commands.md`
   §2.1 规则表补一节；
5. **真机验收**：
   - ZCode：输入含「北京」的文本 → 等 >2 分钟 → 说「把北京改成上海」→ 成功；
   - ZCode：文本两处「北京」→ 说「把北京改成上海」→ 追问「第几个」→ 答「第二个」→ 成功；
   - 记事本：同流程；
   - 微信：无回归（仍 2 次闪烁、`result=ok`）。

## 6. 决策（用户未即时回复，按 best judgment 定稿）

1. **默认值 = `whole_field`**（符合本次需求；`recent_first` 保留，可随时在
   `config.toml` 切回原行为）；
2. **先只改 Windows**：macOS 端列入下一轮同步（§3.3 方案保留，避免双端行为长期
   不一致——下一轮须补齐）。

> 实现前置：用户确认本方案（或直接说「开工」）后按 §5 的 TDD 步骤执行。
