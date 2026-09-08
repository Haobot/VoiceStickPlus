"""评测平台配置：激活引擎 / 评测引擎集 / 云端凭据三级解析。

凭据解析优先级（安全红线：任何层级都不写入 git）：
  环境变量 TENCENT_SECRET_ID/KEY/APPID > m0/config.toml [cloud.tencent]
  > 复用产品配置 %APPDATA%\\VoiceStick\\config.toml 的 tencent_* 字段。
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore

M0_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_M0_CONFIG = M0_ROOT / "config.toml"
DEFAULT_VOICESTICK_CONFIG = Path(os.environ.get("APPDATA", "")) / "VoiceStick" / "config.toml"

DEFAULT_ACTIVE_ENGINE = "local_sense_voice"
DEFAULT_EVAL_ENGINES = ["local_sense_voice", "cloud_tencent"]


@dataclass(frozen=True)
class PlatformConfig:
    """评测平台配置（非敏感部分）。"""

    active_engine: str = DEFAULT_ACTIVE_ENGINE
    eval_engines: list[str] = field(default_factory=lambda: list(DEFAULT_EVAL_ENGINES))


@dataclass(frozen=True)
class CloudTencentCredentials:
    """腾讯云 ASR 凭据（仅内存/本机文件，绝进仓库）。"""

    secret_id: str
    secret_key: str
    appid: str
    engine_model_type: str = "16k_zh"
    region: str = "ap-shanghai"

    def masked_summary(self) -> str:
        """脱敏摘要，用于日志与界面展示。"""
        return (f"tencent[{self.engine_model_type}@{self.region}] "
                f"id={self.secret_id[:6]}*** appid={self.appid}")


def _read_toml(path: Path) -> dict:
    if not path.exists():
        return {}
    with open(path, "rb") as fh:
        return tomllib.load(fh)


def load_platform_config(path: Path | None = None) -> PlatformConfig:
    """加载 m0/config.toml（缺文件/缺字段用默认值）。"""
    data = _read_toml(path or DEFAULT_M0_CONFIG)
    engine = data.get("engine", {})
    evaluation = data.get("evaluation", {})
    return PlatformConfig(
        active_engine=engine.get("active", DEFAULT_ACTIVE_ENGINE),
        eval_engines=evaluation.get("engines", list(DEFAULT_EVAL_ENGINES)),
    )


def _cred_from_mapping(mapping: dict) -> CloudTencentCredentials | None:
    """从字典构造凭据；字段不全视为无效返回 None。"""
    secret_id = str(mapping.get("secret_id", "")).strip()
    secret_key = str(mapping.get("secret_key", "")).strip()
    appid = str(mapping.get("appid", "")).strip()
    if not (secret_id and secret_key and appid):
        return None
    return CloudTencentCredentials(
        secret_id=secret_id,
        secret_key=secret_key,
        appid=appid,
        engine_model_type=str(mapping.get("engine_model_type", "16k_zh")),
        region=str(mapping.get("region", "ap-shanghai")),
    )


def resolve_tencent_credentials(
    m0_config: Path | None = None,
    voicestick_config: Path | None = None,
) -> CloudTencentCredentials | None:
    """三级解析腾讯凭据：env > m0/config.toml > VoiceStick config.toml。"""
    env = {
        "secret_id": os.environ.get("TENCENT_SECRET_ID", ""),
        "secret_key": os.environ.get("TENCENT_SECRET_KEY", ""),
        "appid": os.environ.get("TENCENT_APPID", ""),
    }
    cred = _cred_from_mapping(env)
    if cred:
        return cred

    m0_data = _read_toml(m0_config or DEFAULT_M0_CONFIG)
    cred = _cred_from_mapping(m0_data.get("cloud", {}).get("tencent", {}))
    if cred:
        return cred

    vs_data = _read_toml(voicestick_config or DEFAULT_VOICESTICK_CONFIG)
    cred = _cred_from_mapping({
        "secret_id": vs_data.get("tencent_secret_id", ""),
        "secret_key": vs_data.get("tencent_secret_key", ""),
        "appid": vs_data.get("tencent_appid", ""),
        "engine_model_type": vs_data.get("tencent_engine_model_type", "16k_zh"),
    })
    return cred
