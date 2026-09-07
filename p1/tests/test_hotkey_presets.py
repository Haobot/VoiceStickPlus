"""热键预设方案解析单测：preset 字段优先级、自定义回退、无效值启动即失败。"""
import pytest

from p1.interaction.hotkey_presets import PRESETS, resolve_preset


def test_内置预设至少两套且四键齐全():
    assert set(PRESETS) >= {"right_ctrl", "f_key"}
    for preset in PRESETS.values():
        assert preset.display
        for key in (preset.push_to_talk, preset.cancel,
                    preset.add_selection, preset.confirm_recent):
            assert key and isinstance(key, str)


def test_preset字段命中_整组采用预设_显式键被忽略():
    preset = resolve_preset({
        "preset": "f_key",
        "push_to_talk": "right ctrl",   # 显式键与 preset 并存：preset 优先
    })
    assert preset.key == "f_key"
    assert preset.push_to_talk == PRESETS["f_key"].push_to_talk


def test_无preset_取显式键_为custom():
    preset = resolve_preset({
        "push_to_talk": "f7", "cancel": "esc",
        "add_selection": "ctrl+alt+j", "confirm_recent": "ctrl+alt+d"})
    assert preset.key == "custom"
    assert preset.push_to_talk == "f7"
    assert preset.add_selection == "ctrl+alt+j"


def test_无preset_无显式键_内置默认():
    preset = resolve_preset({})
    assert preset.key == "custom"
    assert preset.push_to_talk == "right ctrl"
    assert preset.cancel == "esc"


def test_preset无效值_启动即失败不静默():
    with pytest.raises(ValueError, match="unknown_scheme"):
        resolve_preset({"preset": "unknown_scheme"})
