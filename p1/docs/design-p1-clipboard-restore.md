# P1 第四迭代设计：剪贴板内容恢复

> 路线图 §四 P1 后续迭代项（design-p1-mvp §6 与 design-p1-hotword-entries §2.1
> 均标注「P1 接受覆盖，后续做恢复」——本迭代兑现）。

## 1. 目标

P1 两条路径破坏用户剪贴板：**文本注入**（copy + Ctrl+V 出字）与**框选读取**
（Ctrl+C 读选区）。当前 pyperclip 只能暂存/还原文本——图片、文件列表、HTML 等
非文本格式被永久覆盖。本迭代交付完整格式快照/恢复，两条路径用完即还原。

## 2. 关键设计决策

### 2.1 ClipboardVault：枚举全部格式逐项字节快照（纯 win32 ctypes）
`OpenClipboard → EnumClipboardFormats → GetClipboardData → GlobalSize/GlobalLock`
把每种格式拷成 bytes；恢复时 `GlobalAlloc(GMEM_MOVEABLE) + 拷贝 + SetClipboardData`。
- **跳过句柄类格式**（CF_BITMAP=2 / CF_METAFILEPICT=3 / CF_PALETTE=9 /
  CF_ENHMETAFILE=14）：它们不是 HGLOBAL，位图场景应用几乎都会同时提供
  CF_DIB/CF_DIBV5 内存版，快照不丢主流内容；矢量/metafile 丢失记入已知限制。
- **注册格式（HTML Format、FileGroupDescriptorW 等）**：同进程内 format id 稳定，
  按 id 原样写回；名字仅作诊断。
- **尽力恢复**：单格式 SetClipboardData 失败跳过（CF_LOCALE 等系统格式偶发拒绝），
  不让个别格式毁掉整体恢复。
- **OpenClipboard 带重试**（5×10ms）：本机剪贴板并发占用是反复出现的现实
  （第二迭代「复制无响应」同源）。
- 空剪贴板快照 = 空列表，恢复 = EmptyClipboard。

### 2.2 注入路径的恢复时机：Ctrl+V 后延迟 150ms
粘贴是目标应用异步读剪贴板——立即恢复会粘贴出旧内容。默认 `restore_delay=0.15s`
（回填延迟预算 500ms，第一迭代实测 total 60-80ms，恢复后仍余量充足）。
`send_paste=False`（测试/e2e 通道验证）不走延迟立即恢复；
e2e_selftest 改传 `restore_clipboard=False` 保持「验文本落板」语义。

### 2.3 框选读取：vault 替换文本暂存
save 在 Ctrl+C 前、restore 在读到文本后无条件执行（原「text != saved 才还原」
逻辑废除——快照恢复幂等且覆盖非文本）。复制无响应（序列号不动）时**不恢复**
直接报错——剪贴板未被破坏，恢复是多余动作。

### 2.4 不做
- 剪贴板历史/多槽位（P2）
- 跨进程 vault（快照仅在当前进程生命周期内有效）
- metafile/调色板句柄格式转换

## 3. 模块变更

```text
src/p1/interaction/
├── clipboard_vault.py     # 新：ClipboardVault（save/restore 完整格式快照）
├── selection.py           # 文本暂存→vault 快照
└── injector.py            # 注入后恢复（restore_clipboard 参数 + restore_delay）
```

## 4. 测试策略（TDD）

| 模块 | 策略 |
|---|---|
| ClipboardVault | 真剪贴板：文本+HTML Format 双格式快照恢复 / 空剪贴板 / 纯文本 / CF_DIB 位图字节往返 |
| selection | 现有文本用例不动；升级「读取后剪贴板不被破坏」为多格式断言 |
| injector | 默认恢复原内容；restore_clipboard=False 保持旧语义 |

## 5. 真机验收

截图进剪贴板 → F8 口述出字 → 图片仍在（画图可粘贴）；同场景走 ctrl+alt+h
框选读取；PTT 回归；e2e 自检。
