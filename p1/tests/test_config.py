"""config 单测：默认值 / 文件加载覆盖 / 坏文件报错 / 路径解析。"""
from pathlib import Path

import pytest

from p1.config import P1Config, load_config


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
