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


# ---------- 暂停/恢复（托盘开关的后端） ----------

def _make_listener(calls):
    return HotkeyListener(
        push_key="f9", cancel_key="esc",
        on_press=lambda: calls.append("press"),
        on_release=lambda: calls.append("release"),
        on_cancel=lambda: calls.append("cancel"),
        action_keys={"ctrl+alt+k": lambda: calls.append("action")})


def test_暂停后所有回调短路():
    calls = []
    listener = _make_listener(calls)
    listener.start()
    listener.pause()
    assert listener.is_paused()
    # 白盒直调注册的 handler：暂停态下任何键事件都不得穿透
    listener._push_handler(_fake_event("down"))
    listener._push_handler(_fake_event("up"))
    listener._cancel_handler(None)
    listener._action_handlers["ctrl+alt+k"]()
    assert calls == []
    listener.stop()


def test_恢复后回调重新生效():
    calls = []
    listener = _make_listener(calls)
    listener.start()
    listener.pause()
    listener.resume()
    assert not listener.is_paused()
    listener._push_handler(_fake_event("down"))
    assert calls == ["press"]
    listener.stop()


def test_暂停期间按住说话_松手也不触发():
    """按住中途暂停的边界：press 已被吞，resume 后孤立 up 不应凭空出字。"""
    calls = []
    listener = _make_listener(calls)
    listener.start()
    listener._push_handler(_fake_event("down"))   # 按住
    calls.clear()
    listener.pause()
    listener._push_handler(_fake_event("up"))     # 暂停中松手
    assert calls == []                            # 不许触发 release 出字
    listener.resume()
    listener.stop()


class _fake_event:
    """keyboard 库的 event_type 是字符串常量 KEY_DOWN='down' / KEY_UP='up'。"""

    def __init__(self, kind: str):
        self.event_type = kind
