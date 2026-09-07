"""热键预设方案集：四键一组的键位方案，托盘切换 + config 持久化的数据源。

preset 字段存在时整组采用预设（显式四键被忽略——避免半预设半自定义的
歧义状态）；无 preset 时显式四键即自定义方案（key="custom"）。
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class HotkeyPreset:
    key: str          # 方案标识（config 持久化 & 托盘 action 用）
    display: str      # 托盘菜单显示名
    push_to_talk: str
    cancel: str
    add_selection: str
    confirm_recent: str


PRESETS: dict[str, HotkeyPreset] = {
    "right_ctrl": HotkeyPreset(
        key="right_ctrl", display="右Ctrl 长按",
        push_to_talk="right ctrl", cancel="esc",
        add_selection="ctrl+alt+h", confirm_recent="ctrl+alt+s"),
    "f_key": HotkeyPreset(
        key="f_key", display="F8 长按",
        push_to_talk="f8", cancel="esc",
        add_selection="ctrl+alt+h", confirm_recent="ctrl+alt+s"),
}

# 自定义方案的兜底键（config 无任何键位配置时）
_DEFAULT = PRESETS["right_ctrl"]


def resolve_preset(hotkey_section: dict) -> HotkeyPreset:
    """从 config [hotkey] 段解析生效方案。

    preset 命中 → 预设整组；preset 无效值 → ValueError（启动即失败，
    静默降级会让用户以为切了实际没切）；无 preset → custom（显式键或缺省）。
    """
    preset_key = hotkey_section.get("preset")
    if preset_key is not None:
        key = str(preset_key)
        if key not in PRESETS:
            raise ValueError(
                f"未知的热键预设 preset = {key!r}，可选: "
                + "/".join(PRESETS) + "（或删除该行用自定义键位）")
        return PRESETS[key]
    return HotkeyPreset(
        key="custom", display="自定义",
        push_to_talk=str(hotkey_section.get("push_to_talk",
                                            _DEFAULT.push_to_talk)),
        cancel=str(hotkey_section.get("cancel", _DEFAULT.cancel)),
        add_selection=str(hotkey_section.get("add_selection",
                                             _DEFAULT.add_selection)),
        confirm_recent=str(hotkey_section.get("confirm_recent",
                                              _DEFAULT.confirm_recent)))
