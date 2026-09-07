# P1 第二迭代设计：热词库两个入口（框选添加 / 改写确认）

> 路线图 §四 P1「热词库 v1」收尾项：SQLite 加密存储与纠正管线已于第一迭代交付，
> 本迭代补齐「框选添加 / 改写确认两个入口」。前置文档：[design-p1-mvp.md](design-p1-mvp.md)。

## 1. 目标

- **框选添加**：任意应用选中文本 → `Ctrl+Alt+H` → 确认小窗（可编辑词条与读音变体）→ 入库（source=manual）。
- **改写确认**：口述结束后按 `Ctrl+Alt+S` → 弹出最近一句的三段对照（识别原文 / 热词纠正后 / 改写后）
  → 勾选纠正对「记入热词库」→ 错读形式作为读音变体入库（source=correction），
  下次同样发音直接纠正——飞轮闭环的关键一步。

## 2. 关键设计决策

### 2.1 框选读取：剪贴板往返（暂存→Ctrl+C→读→还原）
Windows 无跨应用"取选中文本"通用 API；业界通行剪贴板复制法。第一迭代"不做剪贴板恢复"
针对注入路径；本入口必须暂存还原，否则框选一次就冲掉用户剪贴板。
取舍：pyperclip 仅支持文本——**非文本剪贴板（图片/文件）无法还原，会被覆盖**（P1 已知限制，
README 记录；P2 用 win32 完整格式暂存）。

### 2.2 错读形式进 pron 变体（改写确认的核心价值）
纠正对 `kuernetes → Kubernetes` 确认入库时：`add(surface="Kubernetes", source="correction",
pron=["kuernetes"])`。纠正器的别名统一模型（第一迭代已建）会把 pron 变体作为候选别名，
下次听到"kuernetes"立即替换——用户确认一次，终身受益。

### 2.3 最近口述缓存：controller 持有，仅一条
`VoiceController` 缓存最近一次 `PipelineResult`（含三段文本与纠正事件），新会话覆盖；
确认入口只对最近一条有效（无最近结果时弹提示）。不落盘、不做历史列表（P2 热词管理界面的事）。

### 2.4 热键扩展：HotkeyListener 附加动作键
现监听器为 PTT 三键专用；增加 `action_keys: dict[key, callback]` 附加注册（本迭代两个）。
默认键位 `ctrl+alt+h` / `ctrl+alt+s`，config `[hotkey] add_selection / confirm_recent` 可配。

### 2.5 弹窗走 overlay 事件队列（不破坏线程模型）
钩子线程只 post 事件；tkinter 主线程 `_render` 收到 `kind="dialog"` 时在主线程创建
Toplevel（合法），回调经闭包回 controller。悬浮条隐藏期间 root 处于 withdrawn，
Toplevel 挂在 root 上照常显示。

## 3. 模块变更

```text
src/p1/
├── config.py                    # +add_selection / confirm_recent 两键解析
├── controller.py                # +last_result 缓存；+on_add_selection/on_confirm_recent
├── interaction/
│   ├── hotkey.py                # +action_keys 附加注册
│   ├── overlay.py               # +OverlayEvent.kind="dialog"（带 dialog 类型与 payload）
│   ├── selection.py             # 新：SelectionReader 剪贴板往返读选中文本
│   └── dialogs.py               # 新：HotwordDialog / ConfirmRecentDialog（Toplevel）
└── orchestration/pipeline.py    # 不动（Result 已带三段文本与纠正事件）
```

## 4. 测试策略（TDD）

| 模块 | 策略 |
|---|---|
| config 两新键 | 纯单测（默认值/覆盖/非法） |
| SelectionReader | send_copy=False 剪贴板往返单测；空选择 ValueError；完整 Ctrl+C 路径真机验收 |
| HotkeyListener action_keys | 注册/注销不抛单测 |
| controller 缓存与回调 | fake result 纯单测（确认后 store.add 参数断言：pron 含错读形式） |
| dialogs | 真 Toplevel 创建+模拟点击回调（pytest 可建 Tk，不进 mainloop）；视觉真机验收 |
| 全链路 | 真机：记事本框选 → 热键 → 小窗 → 入库 → grep 加密库验证 + 下一句口述纠正生效 |

## 5. 明确不做（本迭代）

- 热词管理列表界面/删除/导出（P2 热词管理）
- 非文本剪贴板的完整格式暂存还原（P2 win32 API）
- 历史口述列表、多选批量确认（P2）
