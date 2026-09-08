"""platform_config 单测：配置加载与云端凭据三级解析（env > m0 > VoiceStick）。"""
import os
from pathlib import Path

import pytest

from m0_asr.platform_config import (
    CloudTencentCredentials,
    load_platform_config,
    resolve_tencent_credentials,
)


def _write_toml(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


# ---------- load_platform_config ----------

def test_加载配置_激活引擎与默认评测集(tmp_path):
    cfg_file = tmp_path / "config.toml"
    _write_toml(cfg_file, """
[engine]
active = "cloud_tencent"

[evaluation]
engines = ["local_sense_voice", "cloud_tencent"]
""")
    cfg = load_platform_config(cfg_file)
    assert cfg.active_engine == "cloud_tencent"
    assert cfg.eval_engines == ["local_sense_voice", "cloud_tencent"]


def test_加载配置_缺省值(tmp_path):
    cfg_file = tmp_path / "config.toml"
    _write_toml(cfg_file, "")
    cfg = load_platform_config(cfg_file)
    assert cfg.active_engine == "local_sense_voice"  # 默认本地
    assert cfg.eval_engines == ["local_sense_voice", "cloud_tencent"]


def test_加载配置_文件不存在_用缺省(tmp_path):
    cfg = load_platform_config(tmp_path / "不存在.toml")
    assert cfg.active_engine == "local_sense_voice"


# ---------- 凭据三级解析 ----------

def test_凭据解析_环境变量优先(monkeypatch, tmp_path):
    monkeypatch.setenv("TENCENT_SECRET_ID", "env-id")
    monkeypatch.setenv("TENCENT_SECRET_KEY", "env-key")
    monkeypatch.setenv("TENCENT_APPID", "env-appid")
    m0_cfg = tmp_path / "m0.toml"
    _write_toml(m0_cfg, """
[cloud.tencent]
secret_id = "m0-id"
secret_key = "m0-key"
appid = "m0-appid"
""")
    vs_cfg = tmp_path / "vs.toml"
    _write_toml(vs_cfg, """
tencent_secret_id = "vs-id"
tencent_secret_key = "vs-key"
tencent_appid = "vs-appid"
""")
    cred = resolve_tencent_credentials(m0_config=m0_cfg, voicestick_config=vs_cfg)
    assert cred is not None
    assert (cred.secret_id, cred.secret_key, cred.appid) == ("env-id", "env-key", "env-appid")


def test_凭据解析_m0配置次之(monkeypatch, tmp_path):
    for var in ("TENCENT_SECRET_ID", "TENCENT_SECRET_KEY", "TENCENT_APPID"):
        monkeypatch.delenv(var, raising=False)
    m0_cfg = tmp_path / "m0.toml"
    _write_toml(m0_cfg, """
[cloud.tencent]
secret_id = "m0-id"
secret_key = "m0-key"
appid = "m0-appid"
""")
    vs_cfg = tmp_path / "vs.toml"
    _write_toml(vs_cfg, 'tencent_secret_id = "vs-id"\ntencent_secret_key = "vs-key"\ntencent_appid = "vs-appid"\n')
    cred = resolve_tencent_credentials(m0_config=m0_cfg, voicestick_config=vs_cfg)
    assert cred.secret_id == "m0-id"


def test_凭据解析_回退VoiceStick配置(monkeypatch, tmp_path):
    for var in ("TENCENT_SECRET_ID", "TENCENT_SECRET_KEY", "TENCENT_APPID"):
        monkeypatch.delenv(var, raising=False)
    m0_cfg = tmp_path / "m0.toml"
    _write_toml(m0_cfg, "")
    vs_cfg = tmp_path / "vs.toml"
    _write_toml(vs_cfg, """
asr_provider = "tencent"
tencent_secret_id = "vs-id"
tencent_secret_key = "vs-key"
tencent_appid = "vs-appid"
tencent_engine_model_type = "16k_zh"
""")
    cred = resolve_tencent_credentials(m0_config=m0_cfg, voicestick_config=vs_cfg)
    assert cred is not None
    assert cred.secret_id == "vs-id"
    assert cred.engine_model_type == "16k_zh"


def test_凭据解析_全部缺失返回None(monkeypatch, tmp_path):
    for var in ("TENCENT_SECRET_ID", "TENCENT_SECRET_KEY", "TENCENT_APPID"):
        monkeypatch.delenv(var, raising=False)
    cred = resolve_tencent_credentials(
        m0_config=tmp_path / "无.toml", voicestick_config=tmp_path / "无.toml")
    assert cred is None


def test_凭据解析_部分字段缺失视为无效(monkeypatch, tmp_path):
    for var in ("TENCENT_SECRET_ID", "TENCENT_SECRET_KEY", "TENCENT_APPID"):
        monkeypatch.delenv(var, raising=False)
    vs_cfg = tmp_path / "vs.toml"
    _write_toml(vs_cfg, 'tencent_secret_id = "only-id"\n')  # 缺 key 与 appid
    cred = resolve_tencent_credentials(
        m0_config=tmp_path / "无.toml", voicestick_config=vs_cfg)
    assert cred is None


def test_凭据对象可脱敏展示():
    cred = CloudTencentCredentials(secret_id="AKID1234567890", secret_key="abcdef", appid="1",
                                   engine_model_type="16k_zh")
    assert "AKID1234567890" not in cred.masked_summary()
    assert "16k_zh" in cred.masked_summary()
