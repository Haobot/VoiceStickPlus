"""全局热键监听（keyboard 库钩子线程回调，不做任何阻塞操作）。

按住说话：push_to_talk 键 down = 开始录音，up = 停止并出字；
cancel 键按下 = 放弃当前会话。回调里只调控制器方法（自身很快返回）。
"""
from __future__ import annotations


class HotkeyListener:
    def __init__(self, push_key: str, cancel_key: str,
                 on_press, on_release, on_cancel):
        self._push_key = push_key
        self._cancel_key = cancel_key
        self._on_press = on_press
        self._on_release = on_release
        self._on_cancel = on_cancel
        self._handles = []

    def start(self) -> None:
        import keyboard

        self._handles.append(keyboard.on_press_key(
            self._push_key, lambda e: self._on_press(), suppress=False))
        self._handles.append(keyboard.on_release_key(
            self._push_key, lambda e: self._on_release(), suppress=False))
        self._handles.append(keyboard.on_press_key(
            self._cancel_key, lambda e: self._on_cancel(), suppress=False))

    def stop(self) -> None:
        import keyboard

        for handle in self._handles:
            keyboard.unhook(handle)
        self._handles.clear()
