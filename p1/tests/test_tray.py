"""托盘菜单模型单测：纯数据逻辑，win32 细节留 TrayIcon 手动验收。"""
import time

import pytest

from p1.interaction.tray import TrayIcon, TrayMenu


def test_菜单项与分隔线的标签序列():
    menu = TrayMenu()
    menu.add("VoiceStick P1 运行中", action="", enabled=False)
    menu.add_separator()
    menu.add("退出", action="quit")
    assert menu.labels() == ["VoiceStick P1 运行中", "-", "退出"]


def test_命令id从1连续分配_分隔线不占id():
    menu = TrayMenu()
    menu.add("状态", action="", enabled=False)
    menu.add_separator()
    menu.add("退出", action="quit")
    # 可点击项才拿命令 id（win32 TrackPopupMenu 需要）；禁用状态项也占 id 但查不到动作
    assert menu.action_for(1) == ""      # 状态项无动作
    assert menu.action_for(2) == "quit"


def test_按动作标识查找菜单项():
    menu = TrayMenu()
    menu.add("退出", action="quit")
    item = menu.find("quit")
    assert item is not None and item.label == "退出"
    assert menu.find("不存在") is None


def test_重复动作标识_首个命中():
    menu = TrayMenu()
    menu.add("退出", action="quit")
    menu.add("再退出", action="quit")
    assert menu.find("quit").label == "退出"


def test_未知命令id返回None动作():
    menu = TrayMenu()
    menu.add("退出", action="quit")
    assert menu.action_for(999) is None


def test_勾选态菜单项_默认不勾():
    menu = TrayMenu()
    item = menu.add("监听热键", action="toggle_listen")
    assert item.checked is False


def test_勾选态项_通过items序列读取():
    menu = TrayMenu()
    menu.add("监听热键", action="toggle_listen", checked=True)
    menu.add("开机自启", action="toggle_autostart", checked=False)
    items = menu.items()
    assert items[0].checked is True
    assert items[1].checked is False


def test_热词库条数刷新_重建菜单保持动作绑定():
    """状态行条数动态变化：以同 action 重建，回调绑定不丢。"""
    menu = TrayMenu()
    menu.add("热词库 3 条", action="", enabled=False)
    menu.add("退出", action="quit")
    menu2 = TrayMenu()
    menu2.add("热词库 5 条", action="", enabled=False)
    menu2.add("退出", action="quit")
    assert menu2.find("quit") is not None


# ---------- 子菜单（第五迭代：热键方案切换） ----------

def _preset_menu() -> TrayMenu:
    sub = TrayMenu()
    sub.add("右Ctrl 长按", action="preset:right_ctrl", checked=True)
    sub.add("F8 长按", action="preset:f_key", checked=False)
    menu = TrayMenu()
    menu.add("监听热键", action="toggle_listen")
    menu.add_popup("热键方案", sub)
    menu.add("退出", action="quit")
    return menu


def test_子菜单_命令id全树唯一():
    menu = _preset_menu()
    parent_ids = {item.cmd_id for item in menu.items() if item.label != "-"}
    sub = menu.find_popup("热键方案")
    sub_ids = {item.cmd_id for item in sub.items() if item.label != "-"}
    assert len(parent_ids) == 3            # 监听热键 / 热键方案(弹出也占id) / 退出
    assert len(sub_ids) == 2
    # 父与子的 id 不重叠（win32 TrackPopupMenu 无法区分命令来源）
    assert parent_ids & sub_ids == set()
    # 全树扁平化后无重复
    assert len(parent_ids | sub_ids) == 5


def test_子菜单_action_for递归穿透():
    menu = _preset_menu()
    # 子菜单项的 cmd_id 能从根菜单解析出 action
    target = menu.find_popup("热键方案").find("preset:f_key")
    assert menu.action_for(target.cmd_id) == "preset:f_key"
    # 父级项不受影响
    assert menu.action_for(menu.find("quit").cmd_id) == "quit"


def test_子菜单_find递归穿透():
    menu = _preset_menu()
    assert menu.find("preset:f_key") is not None
    assert menu.find("preset:f_key").label == "F8 长按"


def test_子菜单_渲染序列含弹出项与勾选态():
    menu = _preset_menu()
    assert menu.labels() == ["监听热键", "热键方案", "退出"]
    popup = menu.find_popup("热键方案")
    assert [i.label for i in popup.items()] == ["右Ctrl 长按", "F8 长按"]
    assert popup.items()[0].checked is True


# ---------- TrayIcon 生命周期（真窗口，不点菜单；点击留真机验收） ----------

@pytest.fixture
def tray():
    icon = TrayIcon(menu_factory=lambda: TrayMenu(), on_action=lambda a: None)
    assert icon.start(), "托盘图标添加失败"
    yield icon
    icon.stop()


def test_托盘启动成功并干净退出(tray):
    tray.stop()                      # 显式退出
    tray._thread.join(timeout=3.0)   # 消息循环应随 WM_QUIT 结束
    assert not tray._thread.is_alive()


def test_托盘重复stop不炸(tray):
    tray.stop()
    tray.stop()                      # 幂等
    tray._thread.join(timeout=3.0)
    assert not tray._thread.is_alive()
