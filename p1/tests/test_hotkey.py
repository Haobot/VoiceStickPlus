"""HotkeyListener 单测：PTT 三键契约 + 第二迭代附加动作键。

keyboard 库钩子真注册真注销（Windows 本机有显示环境），
不发物理键——只验证注册/注销不抛、回调句柄被清理。
"""
from p1.interaction.hotkey import HotkeyListener


def test_无附加键_注册注销不抛():
    listener = HotkeyListener(
        push_key="f9", cancel_key="esc",
        on_press=lambda: None, on_release=lambda: None, on_cancel=lambda: None)
    listener.start()
    listener.stop()


def test_附加动作键_注册注销不抛():
    calls = []
    listener = HotkeyListener(
        push_key="f9", cancel_key="esc",
        on_press=lambda: None, on_release=lambda: None, on_cancel=lambda: None,
        action_keys={"ctrl+alt+h": lambda: calls.append("add")})
    listener.start()
    listener.stop()
    # 未发键，回调不应被触发
    assert calls == []


def test_停止后句柄清空_重复停止安全():
    listener = HotkeyListener(
        push_key="f9", cancel_key="esc",
        on_press=lambda: None, on_release=lambda: None, on_cancel=lambda: None,
        action_keys={"ctrl+alt+h": lambda: None})
    listener.start()
    listener.stop()
    listener.stop()
    assert listener._handles == []
    assert listener._hotkey_handles == []
