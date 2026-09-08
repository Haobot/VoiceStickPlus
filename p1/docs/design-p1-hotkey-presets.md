# P1 第五迭代设计：多方案热键映射（预设 + 托盘切换 + 持久化）

路线图 §6 候选「多方案热键映射」的落地。目标：用户不再手改 config.toml 换键——
托盘菜单选预设方案，即时生效（重绑钩子）+ 重启保持（写回 config）。

## 需求

1. 内置热键预设方案集（PTT 键 / 取消键 / 框选键 / 确认键 四键一组）
2. config `[hotkey] preset = "..."` 选预设；无 preset 时显式四键 = 自定义（现状兼容）
3. 托盘「热键方案」子菜单：列出预设、当前项勾选、点击即切换
4. 切换 = 重绑全部钩子 + 写回 config.toml（保留注释与其他配置行）

## 预设集

| key | 显示名 | push_to_talk | cancel | add_selection | confirm_recent |
|---|---|---|---|---|---|
| `right_ctrl` | 右Ctrl 长按 | right ctrl | esc | ctrl+alt+h | ctrl+alt+s |
| `f_key` | F8 长按 | f8 | esc | ctrl+alt+h | ctrl+alt+s |

自定义（config 显式四键且无 preset）在托盘显示为「自定义」状态项，不可点击切换——
回预设 = 删 preset 行重启，托盘编辑键位不做（P2 候选）。

## 模块设计

### `interaction/hotkey_presets.py`（新，纯数据）

```python
@dataclass(frozen=True)
class HotkeyPreset:
    key: str; display: str
    push_to_talk: str; cancel: str; add_selection: str; confirm_recent: str

PRESETS: dict[str, HotkeyPreset]        # 上面两套

def resolve_preset(hotkey_section: dict) -> HotkeyPreset:
    """preset 字段在 → 预设整组（显式键忽略，文档写明优先级）；
    preset 无效值 → ValueError（启动即失败，不静默）；
    无 preset → custom 预设（取显式键或内置默认）。"""
```

### `HotkeyListener.rebind(push_key, cancel_key, action_keys)`（扩展）

stop() → 换键 → start()。暂停态保持不变（rebind 不触碰 `_paused`）。
换键即键不同，无同键双钩子 KeyError 风险（同键重绑也安全：先 stop 清句柄再 start）。

### `config.py`（扩展）

- `load_config` 增加 `hotkey_preset` 字段（preset 标识，"custom" 表示显式键）
- `save_preset(path, preset_key)`：行级 TOML 回写——已有 `preset =` 行替换、
  `[hotkey]` 段内无则段头后插入、无段则文件尾追加；其余行原样保留；
  重复调用幂等。Python 3.12 tomllib 只读，手写行级回写 ~30 行，零新依赖。

### `TrayMenu.add_popup(label, submenu)`（扩展）

- 渲染递归：MF_POPUP(0x0010) + 子菜单 HMENU；子菜单 cmd_id 在 add_popup 时
  整树重编为父菜单 id 空间之后的连续段（win32 菜单 cmd 全树唯一）
- `action_for` 递归穿透子菜单；`find` 同理
- 现有扁平 API 不变

### `main.py` 集成

- 启动：resolve_preset → 四键装配（现状逻辑不变，键值来自预设）
- 托盘：`preset:<key>` action → `after(0)` 主线程：rebind + save_preset + 日志
- menu_factory 动态重建，勾选态自动反映当前方案

## 边界与异常

- preset 值不在 PRESETS：启动 ValueError（不静默降级——静默会导致用户以为切了实际没切）
- save_preset 写失败（权限/占用）：日志告警，运行时切换仍生效，下次启动回旧方案（如实降级）
- rebind 期间钩子间隙（stop→start 毫秒级窗口）：可接受，切换本身用户主动操作

## 不做

- 托盘编辑任意键位（按键捕获 UI）——P2
- 多预设自定义存储（用户预设库）——config 显式四键已覆盖
- 冲突检测（与其他应用热键撞车）——keyboard 库本身无此能力，不伪造

## 测试

- `test_hotkey_presets.py`：预设解析四分支（预设/自定义/无效值/字段完整）
- `test_config.py` 增：save_preset 三形态 + 幂等 + 保留注释
- `test_hotkey.py` 增：rebind 后新键生效（白盒 handler 换绑）+ 暂停态穿越 rebind
- `test_tray.py` 增：子菜单 cmd_id 树唯一 + action_for/find 递归 + 渲染序列
- 真机：托盘切 F8 → F8 hold 出字 → 重启仍 F8 → 切回右Ctrl
