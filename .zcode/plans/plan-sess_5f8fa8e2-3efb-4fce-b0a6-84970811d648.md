## 整框替换改走「Ctrl+A 全选 + Ctrl+V 粘贴」——UIA 写 API 在 ZCode 上全线 S_OK 空转

### Context（本轮日志钉死）

`whole-field rewrite applied full_len=27` → `verify unconfirmed still_old=1`——**SetValue 返回成功但字段一字未变**（still_old=1=动作前原样）。至此 ZCode 上 UIA 三条写路径（Select / SetValue）全部实证 S_OK 空转，死的还包括选区读（GetSelection 恒空）；而 **SendInput 键盘通道始终是活的**（退格删除在 ZCode 一直成功，方向键也有视觉反馈）。

唯一活着的写通道 = 键盘。方案：**Ctrl+A（语义固定=全选，无光标算术无错位）+ Ctrl+V（粘贴替换后全文）→ 全文终验**。失败模式全部无损：焦点错/粘贴被忽略 → 字段原样 → still_old 提示失败（安全模型不变：写回以全文终验确认，不符只提示不宣称成功）。

对照锚点：Notepad 同轮 `result=ok`——原生路径零回归。

### 设计

**层级重排**（`VerifyTargetedSelectionThenAct` 读回耗尽后）：
1. UIA 选区写 + 读回轮询（现状，原生目标）
2. **全选粘贴整框替换**（新）：`TryWholeFieldPasteRewrite`——读全文 → 目标绝对区间（复用现有换算）→ `BuildWholeFieldReplacement` 构造 F'（保留，簇边界匹配不变）→ `input_injector_->ReplaceAllWithPaste(F')` → 200ms 后复用 `FinalizeVoiceTargetedEdit` 全文/尾窗终验（现成轮询窗 2.7s 覆盖粘贴异步落地）
3. 兜底拒绝（Ctrl+End 恢复 + fieldMismatch，现有）

**砍掉**（实证死路，git 可追溯）：
- SetValue 层（`ITargetedFieldEditor::SetWholeText` 接口 + 生产实现 + TryWholeFieldRewriteFallback）
- 键盘选区层（`TryKeyboardSelectionFallback` / `PlanKeyboardSelection` 协调器接线——其依赖的选区读验证在唯一目标上已死；纯逻辑保留在 git 历史）

**注入器接口**（`InputInjector` + `input_injector_win.cc`）：新增 `ReplaceAllWithPaste(const std::string& text)`——复用现有 Paste 的剪贴板基建（ClipboardVault 快照 → 写 text → `Ctrl+A` → sleep 40ms → `Ctrl+V` → sleep 150ms → 还原快照，P1 restore_delay 同款时序）。F' 为空串（全文删除 edge）→ 拒绝该兜底。

### 测试（TDD 先红后绿）

- Fake：`FakeInputInjector` 加 `ReplaceAllWithPaste`（记录 `replace_all_text`）；`VoiceTargetedInputInjector` 落 fake 字段 doc=text + 光标末尾 + op `"paste_all"`；开关 `paste_all_fails`
- 用例改造：
  1. `WholeFieldRewriteSucceeds`：swallow_select → 断言 paste_all 路径（ops `select,read×5,paste_all,end`、doc 变更、成功 toast、无 SetValue）（现状必红）
  2. Exhausts（stale_reads=99）：整框替换成功语义
  3. `AllFallbacksFailRejects`：`paste_all_fails=true` → Ctrl+End 恢复 + fieldMismatch + 零改动 + 记录保留
  4. 删除 KeyboardFallback 两个用例（通道已砍）；原生路径用例回归不动
- 纯逻辑 `BuildWholeFieldReplacement` 向量不变

### 验证

1. build_win.bat 全量构建 → CTest 2/2
2. 重启 VoiceStick.exe，二进制 grep 特征串确认版本
3. 用户真机（ZCode）：语音正文 →「把北京改成上海」→ 预期：输入框内容整体替换成功；日志 `voice targeted result=ok target_len=2`
4. 失败判据（如仍失败）：`verify unconfirmed ... still_old=1` = Ctrl+A/Ctrl+V 也未落地（下一层查焦点 HWND）；`whole-field`/`paste_all` 通道日志看落点
5. 经验文档追加 + distilled 同步

### 风险（显式）

- Ctrl+A 焦点若不在输入框：web 页面全选 + 粘贴被忽略（不可编辑区不接受粘贴）→ 字段无损、终验失败提示
- 富文本 contenteditable 粘纯文本丢格式（聊天输入框纯文本，无感）
- 剪贴板还原竞态：复用 Paste 的 150ms restore_delay 先例
- Chromium 粘贴落地 >200ms：终验轮询窗 ≈2.7s 已覆盖