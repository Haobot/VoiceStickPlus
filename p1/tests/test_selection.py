"""SelectionReader 单测：剪贴板往返（暂存→读→还原）契约。

send_copy=False 路径直接驱动剪贴板（pyperclip 真实往返，不 mock）；
Ctrl+C 发送路径留真机验收。
"""
import pytest

from p1.interaction.selection import SelectionReader

# 动真实剪贴板：每个用例自保存自还原现场


def test_读选中文本_读取后剪贴板不被破坏():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    # 预置"选中"内容：send_copy=False 时直接读当前剪贴板
    pyperclip.copy("Kubernetes")
    assert reader.read_selection(send_copy=False) == "Kubernetes"
    # 读取不应改动剪贴板（saved==text 时无还原动作；Ctrl+C 路径的
    # "暂存→还原"契约由真机验收覆盖，单测发 Ctrl+C 会干扰终端焦点）
    assert pyperclip.paste() == "Kubernetes"
    pyperclip.copy(saved)  # 恢复测试现场


def test_读选中文本_两侧空白被去除():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    pyperclip.copy("  Voicestick P1 \n")
    assert reader.read_selection(send_copy=False) == "Voicestick P1"
    pyperclip.copy(saved)


def test_空选择_报ValueError():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    pyperclip.copy("   ")
    with pytest.raises(ValueError, match="未选中"):
        reader.read_selection(send_copy=False)
    pyperclip.copy(saved)


def test_超长选择_报ValueError并说明上限():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    pyperclip.copy("词" * 65)
    with pytest.raises(ValueError, match="过长"):
        reader.read_selection(send_copy=False)
    pyperclip.copy(saved)


def test_多行选择_内部换行归一为空格():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    pyperclip.copy("第一行\n第二行")
    assert reader.read_selection(send_copy=False) == "第一行 第二行"
    pyperclip.copy(saved)


def test_多行选择_归一后超长仍拒绝():
    import pyperclip
    saved = pyperclip.paste()
    reader = SelectionReader(send_delay=0.0)
    pyperclip.copy("\n".join(["词" * 30, "词" * 30, "词" * 30]))
    with pytest.raises(ValueError, match="过长"):
        reader.read_selection(send_copy=False)
    pyperclip.copy(saved)


def test_复制无响应_重试耗尽后报ValueError(monkeypatch):
    """Ctrl+C 后剪贴板毫无变化（序列号不动）——真机实况：
    剪贴板被并发占用导致复制失败，必须报错而非把旧剪贴板当选区。"""
    import pyperclip
    saved = pyperclip.paste()
    monkeypatch.setattr("keyboard.send", lambda *_: None)  # 模拟复制失败
    pyperclip.copy("OLDBOARD")
    reader = SelectionReader(send_delay=0.0)
    with pytest.raises(ValueError, match="无响应"):
        reader.read_selection(send_copy=True)
    pyperclip.copy(saved)


def test_复制无响应_会重试到上限次(monkeypatch):
    import pyperclip
    saved = pyperclip.paste()
    calls = []
    monkeypatch.setattr("keyboard.send", lambda *_: calls.append(1))
    pyperclip.copy("OLDBOARD")
    reader = SelectionReader(send_delay=0.0)
    with pytest.raises(ValueError, match="无响应"):
        reader.read_selection(send_copy=True)
    assert len(calls) == SelectionReader.COPY_RETRIES + 1
    pyperclip.copy(saved)


def test_复制成功且内容恰与旧剪贴板相同_不误报(monkeypatch):
    """选中内容 == 剪贴板旧值的合法巧合：序列号已变化，不得当失败。"""
    import pyperclip
    saved = pyperclip.paste()
    pyperclip.copy("Kubernetes")
    monkeypatch.setattr("keyboard.send",
                        lambda *_: pyperclip.copy("Kubernetes"))
    reader = SelectionReader(send_delay=0.0)
    assert reader.read_selection(send_copy=True) == "Kubernetes"
    pyperclip.copy(saved)


def test_复制成功改写剪贴板_读取并还原(monkeypatch):
    import pyperclip
    saved = pyperclip.paste()
    pyperclip.copy("OLDBOARD")
    monkeypatch.setattr("keyboard.send",
                        lambda *_: pyperclip.copy("  Kubernetes  "))
    reader = SelectionReader(send_delay=0.0)
    assert reader.read_selection(send_copy=True) == "Kubernetes"
    assert pyperclip.paste() == "OLDBOARD"  # 进入时的剪贴板被还原
    pyperclip.copy(saved)
