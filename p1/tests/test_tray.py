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


def test_热词库条数刷新_重建菜单保持动作绑定():
    """状态行条数动态变化：以同 action 重建，回调绑定不丢。"""
    menu = TrayMenu()
    menu.add("热词库 3 条", action="", enabled=False)
    menu.add("退出", action="quit")
    menu2 = TrayMenu()
    menu2.add("热词库 5 条", action="", enabled=False)
    menu2.add("退出", action="quit")
    assert menu2.find("quit") is not None


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
