"""config 单测：默认值 / 文件加载覆盖 / 坏文件报错 / 路径解析 / preset 持久化。"""
from pathlib import Path

import pytest

from p1.config import P1Config, load_config, save_preset


# ---------- 默认值 ----------

def test_默认配置_无文件时全用内置默认():
    cfg = load_config(Path("Z:/不存在/config.toml"))
    assert isinstance(cfg, P1Config)
    assert cfg.push_to_talk == "right ctrl"
    assert cfg.cancel_key == "esc"
    assert cfg.engine_adapter == "sense_voice"
    assert cfg.rewrite_enabled is True
    assert cfg.rewrite_provider == "rules"


def test_默认配置_模型目录解析为绝对路径():
    cfg = load_config(Path("Z:/不存在/config.toml"))
    # 默认共享 m0/models，必须是可用的绝对路径
    assert Path(cfg.models_dir).is_absolute()
    assert Path(cfg.models_dir).name == "models"


# ---------- 文件加载 ----------

def test_文件加载_覆盖默认值(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text(
        "[hotkey]\n"
        'push_to_talk = "f7"\n'
        "[rewrite]\n"
        "enabled = false\n"
        'provider = "tencent_hunyuan"\n',
        encoding="utf-8",
    )
    cfg = load_config(cfg_file)
    assert cfg.push_to_talk == "f7"
    assert cfg.cancel_key == "esc"  # 未覆盖的字段保持默认
    assert cfg.rewrite_enabled is False
    assert cfg.rewrite_provider == "tencent_hunyuan"


def test_文件加载_坏toml报清晰错误(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text("[hotkey 这是坏格式", encoding="utf-8")
    with pytest.raises(ValueError, match="config.toml 解析失败"):
        load_config(cfg_file)


def test_文件加载_自定义models_dir相对路径基于p1根(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text('[engine]\nmodels_dir = "assets/models"\n', encoding="utf-8")
    cfg = load_config(cfg_file)
    assert Path(cfg.models_dir).is_absolute()
    assert cfg.models_dir.endswith(str(Path("assets") / "models"))


# ---------- 热词入口键位（第二迭代） ----------

def test_默认配置_热词入口两键为ctrl_alt组合():
    cfg = load_config(Path("Z:/不存在/config.toml"))
    assert cfg.add_selection_key == "ctrl+alt+h"
    assert cfg.confirm_recent_key == "ctrl+alt+s"


def test_文件加载_可覆盖热词入口键位(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text(
        "[hotkey]\n"
        'add_selection = "ctrl+alt+k"\n'
        'confirm_recent = "ctrl+alt+j"\n',
        encoding="utf-8",
    )
    cfg = load_config(cfg_file)
    assert cfg.add_selection_key == "ctrl+alt+k"
    assert cfg.confirm_recent_key == "ctrl+alt+j"


# ---------- 热键预设（第五迭代） ----------

def test_加载_preset字段_四键整组取预设(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text('[hotkey]\npreset = "f_key"\n', encoding="utf-8")
    cfg = load_config(cfg_file)
    assert cfg.hotkey_preset == "f_key"
    assert cfg.push_to_talk == "f8"


def test_加载_无preset_标识custom且显式键生效(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text('[hotkey]\npush_to_talk = "f7"\n', encoding="utf-8")
    cfg = load_config(cfg_file)
    assert cfg.hotkey_preset == "custom"
    assert cfg.push_to_talk == "f7"


# ---------- save_preset 行级回写 ----------

def test_保存预设_段内无preset行_段头后插入(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text(
        "# 顶部注释\n[hotkey]\npush_to_talk = \"f7\"\n[rewrite]\nenabled = false\n",
        encoding="utf-8")
    save_preset(cfg_file, "f_key")
    text = cfg_file.read_text(encoding="utf-8")
    assert '[hotkey]\npreset = "f_key"' in text
    # 其他行原样保留
    assert "# 顶部注释" in text
    assert 'push_to_talk = "f7"' in text
    assert "[rewrite]" in text


def test_保存预设_已有preset行_原位替换且幂等(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text(
        '[hotkey]\npreset = "f_key"\ncancel = "esc"\n', encoding="utf-8")
    save_preset(cfg_file, "right_ctrl")
    save_preset(cfg_file, "right_ctrl")   # 幂等：重复写不重复插行
    text = cfg_file.read_text(encoding="utf-8")
    assert text.count('preset =') == 1
    assert 'preset = "right_ctrl"' in text
    assert 'cancel = "esc"' in text


def test_保存预设_无hotkey段_文件尾追加(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text("[rewrite]\nenabled = false\n", encoding="utf-8")
    save_preset(cfg_file, "f_key")
    cfg = load_config(cfg_file)
    assert cfg.hotkey_preset == "f_key"
    assert cfg.rewrite_enabled is False


def test_保存预设_文件不存在_新建最小配置(tmp_path):
    cfg_file = tmp_path / "config.toml"
    save_preset(cfg_file, "f_key")
    cfg = load_config(cfg_file)
    assert cfg.hotkey_preset == "f_key"
    assert cfg.push_to_talk == "f8"


def test_保存自定义_preset行整行删除(tmp_path):
    cfg_file = tmp_path / "config.toml"
    cfg_file.write_text('[hotkey]\npreset = "f_key"\ncancel = "esc"\n',
                        encoding="utf-8")
    save_preset(cfg_file, "custom")
    text = cfg_file.read_text(encoding="utf-8")
    assert "preset" not in text
    assert 'cancel = "esc"' in text
