# P1 第三迭代设计：系统托盘常驻

> 路线图 §四 P1 后续迭代项（design-p1-mvp §6「明确不做」标注 P1 后续）。
> 前置文档：[design-p1-mvp.md](design-p1-mvp.md)。

## 1. 目标

P1 客户端目前是隐藏控制台进程：**无优雅退出**（Hidden 启动后 Ctrl+C 不可达，只能杀进程）、
无运行状态入口。本迭代交付系统托盘常驻：

- 托盘图标（程序名 tooltip，热词库条数动态更新）
- 右键菜单：状态行（禁用）/ 热词库条数（禁用）/ 分隔线 / **退出**
- 退出 = 干净收尾：注销热键钩子、删除托盘图标、销毁 Tk、进程退出码 0

## 2. 关键设计决策

### 2.1 零新依赖：纯 ctypes win32（不用 pystray+PIL）
托盘 = Shell_NotifyIconW + 私有窗口类收回调。图标用 CreateIcon 单色点阵（16×16 "V" 字），
不引图像库。P1 依赖面保持最小（keyboard/pyperclip/onnxruntime）。

### 2.2 托盘线程独立于 Tk 主线程
私有窗口 + GetMessage 阻塞循环放独立线程（不占 Tk after 周期）；
TrackPopupMenu 在托盘线程弹出（菜单有线程亲和，同线程创建合法）。
菜单动作**不在托盘线程直接执行**——经回调闭包 post 给主线程
（Tk destroy / keyboard.unhook 都必须在主线程），复用 overlay 事件模型的思路。

经典坑（已内置防御）：弹菜单前必须 SetForegroundWindow(托盘hwnd)，
否则菜单点击外部不消失（KB135788）。

### 2.3 菜单模型纯逻辑可测，win32 薄封装手动验收
`TrayMenu`（纯数据：项列表/动作标识/命令 id 分配/按 id 查动作）全部单测；
`TrayIcon`（ctypes 注册/消息循环/清理）只测「创建-运行-销毁不炸不泄漏窗口」，
图标真实显示与菜单点击真机验收。

## 3. 模块变更

```text
src/p1/
├── interaction/tray.py    # 新：TrayMenu（纯） + TrayIcon（win32，独立线程）
└── main.py                # 组装：托盘随应用起停；退出动作 → 主线程收尾
```

## 4. 测试策略（TDD）

| 模块 | 策略 |
|---|---|
| TrayMenu | 纯单测：add/separator/find/id 映射、重复 action、空菜单 |
| TrayIcon | 单测：创建后窗口类可查、销毁后窗口不复存在、双创建不炸（幂等 teardown） |
| 退出链路 | main 组装后手动验收：菜单点退出 → 进程退出码 0、钩子注销、托盘消失 |

## 5. 明确不做（本迭代）

- 托盘左键双击开设置窗（设置 UI 是 P2）
- 开机自启、暂停/恢复监听开关（P2）
- 图标状态变化（录音中变色等，悬浮条已表达状态）
