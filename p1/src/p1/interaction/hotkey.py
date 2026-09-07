"""全局热键监听（keyboard 库钩子线程回调，不做任何阻塞操作）。

按住说话：push_to_talk 键 down = 开始录音，up = 停止并出字；
cancel 键按下 = 放弃当前会话；action_keys 为附加组合键（如 ctrl+alt+h）。

实现注意：push 键的 down/up 必须合并为单个 hook_key 钩子——keyboard 库对
同键注册两个钩子时 _hooks[key] 条目互相覆盖，注销时必然 KeyError 且留残钩。
组合键 on_press_key 不支持（"ctrl+alt+h is not mapped"），须走 add_hotkey。
"""
from __future__ import annotations


class HotkeyListener:
    def __init__(self, push_key: str, cancel_key: str,
                 on_press, on_release, on_cancel,
                 action_keys: dict[str, callable] | None = None):
        self._push_key = push_key
        self._cancel_key = cancel_key
        self._on_press = on_press
        self._on_release = on_release
        self._on_cancel = on_cancel
        self._action_keys = action_keys or {}
        self._handles = []   # hook_key 返回的注销函数
        self._hotkey_handles = []  # add_hotkey 返回句柄

    def start(self) -> None:
        import keyboard

        def push_handler(event):
            if event.event_type == keyboard.KEY_DOWN:
                self._on_press()
            else:
                self._on_release()

        self._handles.append(keyboard.hook_key(
            self._push_key, push_handler, suppress=False))
        self._handles.append(keyboard.on_press_key(
            self._cancel_key, lambda e: self._on_cancel(), suppress=False))
        for key, callback in self._action_keys.items():
            self._hotkey_handles.append(keyboard.add_hotkey(key, callback))

    def stop(self) -> None:
        import keyboard

        for handle in self._handles:
            keyboard.unhook(handle)
        self._handles.clear()
        for handle in self._hotkey_handles:
            keyboard.remove_hotkey(handle)
        self._hotkey_handles.clear()
