"""开机自启开关：HKCU Run 注册表键（标准库 winreg，零新依赖）。

命令行优先 venv 同目录 pythonw.exe（无控制台窗口）；不存在则回退 python.exe。
main.py 启动时 os.chdir 到脚本目录——Run 键启动的 cwd 是系统目录，
config 相对路径（../m0/models）依赖 cwd 正确。
"""
from __future__ import annotations

import sys
import winreg
from pathlib import Path

_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_APP_NAME = "VoiceStickP1"
# p1/src/p1/interaction/autostart.py → 上溯 4 层到 p1/main.py
_MAIN = Path(__file__).resolve().parents[3] / "main.py"


def _read_value() -> str | None:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as key:
            value, _ = winreg.QueryValueEx(key, _APP_NAME)
            return value
    except FileNotFoundError:
        return None


def _write_value(command: str) -> None:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY, 0,
                        winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, _APP_NAME, 0, winreg.REG_SZ, command)


def _command() -> str:
    exe = Path(sys.executable)
    pythonw = exe.with_name("pythonw.exe")
    interpreter = pythonw if pythonw.exists() else exe
    return f'"{interpreter}" "{_MAIN}"'


def is_enabled() -> bool:
    return _read_value() is not None


def enable() -> bool:
    """写入自启命令；返回是否新写入（已启用时幂等返回 True）。"""
    if is_enabled():
        return True
    _write_value(_command())
    return True


def disable() -> None:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, _APP_NAME)
    except FileNotFoundError:
        pass
