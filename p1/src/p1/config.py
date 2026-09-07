"""P1 配置加载：p1/config.toml（不存在时用内置默认值）。

凭据与密钥安全规则与 m0 一致：真实 config.toml gitignored，
仓库内只保留 config.example.toml 占位。
"""
from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

P1_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_CONFIG = P1_ROOT / "config.toml"
# 模型权重复用 M0 下载产物，避免重复占用 ~3GB
DEFAULT_MODELS_DIR = P1_ROOT.parent / "m0" / "models"


@dataclass(frozen=True)
class P1Config:
    """P1 运行配置（非敏感部分；凭据另行解析，见 rewrite/engines 层）。"""

    push_to_talk: str = "right ctrl"
    cancel_key: str = "esc"
    engine_adapter: str = "sense_voice"
    models_dir: str = str(DEFAULT_MODELS_DIR)
    rewrite_enabled: bool = True
    rewrite_provider: str = "rules"
    hotword_db_path: str = ""
    hotword_decay_days: int = 30


def load_config(path: Path | None = None) -> P1Config:
    """加载配置文件；缺文件用默认值，坏文件抛 ValueError（启动即失败，不静默）。"""
    path = path or DEFAULT_CONFIG
    data: dict = {}
    if path.exists():
        try:
            with open(path, "rb") as fh:
                data = tomllib.load(fh)
        except tomllib.TOMLDecodeError as exc:
            raise ValueError(f"config.toml 解析失败: {exc}") from exc

    hotkey = data.get("hotkey", {})
    engine = data.get("engine", {})
    rewrite = data.get("rewrite", {})
    hotword = data.get("hotword", {})

    models_dir = str(engine.get("models_dir", DEFAULT_MODELS_DIR))
    resolved = Path(models_dir)
    if not resolved.is_absolute():
        # 相对路径统一基于 p1/ 根，避免受启动目录影响
        resolved = P1_ROOT / resolved
    return P1Config(
        push_to_talk=str(hotkey.get("push_to_talk", "right ctrl")),
        cancel_key=str(hotkey.get("cancel", "esc")),
        engine_adapter=str(engine.get("adapter", "sense_voice")),
        models_dir=str(resolved),
        rewrite_enabled=bool(rewrite.get("enabled", True)),
        rewrite_provider=str(rewrite.get("provider", "rules")),
        hotword_db_path=str(hotword.get("db_path", "")),
        hotword_decay_days=int(hotword.get("decay_days", 30)),
    )
