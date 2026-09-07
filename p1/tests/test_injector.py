"""注入器单测：剪贴板往返真实测量（Windows 剪贴板是本机真实边界）。

Ctrl+V 发键路径不在单测触发（会作用于当前焦点窗口），真机验收覆盖。
"""
import pytest

from p1.interaction.injector import ClipboardInjector


def test_写剪贴板_往返一致():
    injector = ClipboardInjector()
    elapsed = injector.inject("你好VoiceStick P1测试文本", send_paste=False)
    import pyperclip
    assert pyperclip.paste() == "你好VoiceStick P1测试文本"
    assert 0.0 <= elapsed < 0.5  # 剪贴板写入本身应在毫秒级


def test_空文本拒绝注入():
    with pytest.raises(ValueError, match="空文本"):
        ClipboardInjector().inject("", send_paste=False)


def test_多行文本往返():
    text = "第一行\n第二行"
    ClipboardInjector().inject(text, send_paste=False)
    import pyperclip
    assert pyperclip.paste() == text
