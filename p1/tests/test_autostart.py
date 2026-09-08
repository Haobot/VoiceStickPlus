"""开机自启开关单测：真读写 HKCU Run（仅本应用的键名，测试后还原）。"""
import pytest
from pathlib import Path

from p1.interaction import autostart


@pytest.fixture
def clean_run_entry():
    """保存在场值 → 清空 → 测试 → 还原，不碰其它应用的键。"""
    saved = autostart._read_value()
    autostart.disable()
    yield
    autostart.disable()
    if saved is not None:
        autostart._write_value(saved)


def test_默认未启用时_is_enabled为假(clean_run_entry):
    assert autostart.is_enabled() is False


def test_启用后_注册表含main入口绝对路径(clean_run_entry):
    assert autostart.enable() is True
    assert autostart.is_enabled() is True
    value = autostart._read_value()
    assert value is not None
    # main.py 路径必须真实存在（曾错指 src/main.py——层级数错，子串断言拦不住）
    main_path = value.split('"')[3]
    assert main_path.endswith("main.py")
    assert Path(main_path).exists()


def test_禁用后_键被移除(clean_run_entry):
    autostart.enable()
    autostart.disable()
    assert autostart.is_enabled() is False


def test_命令行优先用pythonw无窗口解释器():
    cmd = autostart._command()
    # 本机 venv 存在 pythonw 则必须用它；否则回退 python.exe
    import pathlib
    pyw = pathlib.Path(cmd.split('"')[1] if cmd.startswith('"') else cmd.split()[0])
    assert pyw.name in ("pythonw.exe", "python.exe")


def test_重复启用_幂等不抛(clean_run_entry):
    autostart.enable()
    autostart.enable()
    assert autostart.is_enabled() is True
