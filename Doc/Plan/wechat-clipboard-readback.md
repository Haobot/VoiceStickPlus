# 微信等自绘窗口的语音定点编辑：剪贴板回读通道（fallback）

> 日期：2026-11-04。前置排查见 `Doc/Expe/wechat-mmui-uia-ondemand-cold-tree-2026-11-04.md`。
> 路线 A（UIA 点火：`SPI_SETSCREENREADER` + 微信重启）已于真机证伪：标志在微信整个
> 启动过程中为 TRUE，新进程的 `Qt51514QWindowIcon` / `MMUIRenderSubWindowHW` 子树
> TextPattern 仍为 0、`WM_GETOBJECT` 仍返回 0。微信 4.2.0.8 的 MMUI 不暴露 UIA。

## 目标

微信聊天输入框内的语音编辑（替换/删除）可用，行为与 ZCode 一致；不伪造成功、
读不到就不改、失败有明确提示。

## 方案

新增一个**剪贴板回读通道**，在 UIA `TextPattern` 不可用时提供「读全文」能力：

```text
UIA 读全文失败（微信等自绘窗口）
  → 快照剪贴板（ClipboardVault，完整格式）
  → Ctrl+A（全选）→ Ctrl+C（复制）
  → 轮询 GetClipboardSequenceNumber 变化（≤600ms）
  → 读 CF_UNICODETEXT
  → 恢复剪贴板快照
  → 右方向键取消选区
  → 得到字段全文（UTF-16 → UTF-8）
```

后续 targeted 流程（域解析/计划/整框粘贴/终验）完全复用现有实现。

## 局限与安全边界（必须遵守）

1. **只用于整框域（`field_scope=true`）**：剪贴板通道拿不到光标位置与选区状态，
   故 `caret_abs_utf16 = kCaretOutsideTail`，跳过「最近注入域」，直接整框搜索。
2. **读取会改变目标选区**：Ctrl+A 全选 → 读后右方向键把光标放到末尾。这是本通道
   的已知副作用（用户可见），仅在没有 UIA 读通道时启用。
3. **剪贴板必须可快照**：`ClipboardVault::Save()` 失败（被占用）→ 直接返回失败，
   **不发任何按键**，避免污染用户剪贴板。读取后必须 `Restore` 原快照。
4. **终验语义不放松**：改动后再次用同一通道读全文，与预期**精确比对**；不符 →
   `result_unconfirmed`（不重试改、不伪造成功）。
5. **密码框防护**：UIA 路径已判 `is_password` 的直接拒绝（不走本通道）。UIA 连焦点
   元素都拿不到时无法判断密码框——已知残余风险，靠「只在整框域 + 非密码」与
   「读后立即恢复剪贴板」收敛。
6. **日志不输出字段全文**（沿用现有约束，只记长度/结果）。
7. **失败即拒绝**：剪贴板序列号未变（目标不响应 Ctrl+C）、读不到文本、快照/恢复
   失败 → `verify_unavailable`，绝不盲改。

## 接口

`src/clipboard_field_reader.h`：

```cpp
class IClipboardFieldReader {
public:
    virtual ~IClipboardFieldReader() = default;
    // 读当前焦点字段全文（UTF-8）。空字段 → 返回 true + 空串。
    // 剪贴板不可用/无更新/非文本 → false。须在 UI 线程调用（SendInput）。
    virtual bool ReadFieldFullText(std::string* out) = 0;
};
std::unique_ptr<IClipboardFieldReader> CreateClipboardFieldReader();
```

注入范式与 `SetFocusTextReader` / `SetTargetedFieldEditor` 一致：
`VoiceStickCoordinator::SetClipboardFieldReader(std::unique_ptr<IClipboardFieldReader>)`。
未注入 = 不启用（既有行为逐字节不变，含全部现有测试）。

## 接入点

1. `TakeOverTargetedCommand`（`field_scope` 分支）：`ReadFocusedFieldFullText` 返回
   false 且非密码框、非超长时，尝试剪贴板回读；成功则填充
   `VoiceEditFieldState`（`full_text` / `full_utf16_length` / `tail_*` /
   `selection_collapsed=true` / `caret_at_end=true` / 两个 caret 哨兵）继续原流程。
2. `FinalizeVoiceTargetedEdit`（`absolute_coords` 分支）：UIA 全文读失败时同样走
   剪贴板回读做终验比对。
3. `ExecuteVoiceTargetedEdit`：UIA 选区写（`SelectAbsoluteRange` / `SelectTailRange`）
   失败且已注入剪贴板通道时，降级 `TryWholeFieldPasteRewrite`（整框粘贴不依赖 UIA
   选区，失败无损）；未注入时保持原 `verify_unavailable` 拒绝。
4. `TryWholeFieldPasteRewrite`（整框域）：构造替换文本复用接管阶段已读到的全文
   （`exec.pre_action_text`），**不再重复读剪贴板**——每读一次输入框就全选闪一次，
   复用以把闪烁从 3 次降到 2 次。

## 测试（CTest，`core_tests_voice_edit.cc`）

Fake `VoiceTargetedFakeClipboardReader`（可编程文本/可用性/读取次数）：

1. UIA 全文读不可用 + 剪贴板回读成功 → 整框替换执行，终验（剪贴板读回新全文）→
   `result=ok`。
2. UIA 不可用 + 剪贴板不可用 → `verify_unavailable`，零注入操作。
3. UIA 不可用 + 剪贴板读到的全文不含目标 → 拒绝，零注入操作（防误改）。
4. 终验剪贴板读回与预期不符 → `result_unconfirmed`（不重试改）。

## 真机验证（2026-11-04 20:18，通过）

微信输入框聚焦 → 按键说「我要去北京吃烤鸭」→「把北京改成上海」：

- 日志：`clipboard field reader: updated=1 read_ok=1 text_len=9` →
  `clipboard fallback full_len=9` → `voice targeted action=replace
  domain=whole_field` → `voice targeted result=ok`；
- **剪贴板读 2 次**（接管初读 + 终验读回），输入框全选闪烁 2 次；
- 关键实现修正：**Ctrl+A 后留 120ms 再发 Ctrl+C**（30ms 时微信不产生复制，
  `updated=0`），Ctrl+C 再加一次重试（1000ms 等待窗口）；
- 对照：ZCode / 记事本无回归（UIA 路径优先生效，不触发剪贴板通道）。

## 后续

- 若微信后续版本恢复 UIA（或用户开读屏器），UIA 路径自动优先，剪贴板通道闲置。
- 通道可扩展到其他自绘编辑器（当前只在 UIA 失败时启用，天然通用）。
