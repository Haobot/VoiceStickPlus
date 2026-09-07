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

    hotkey_preset: str = "custom"     # 预设标识；四键为该预设的解析结果
    push_to_talk: str = "right ctrl"
    cancel_key: str = "esc"
    add_selection_key: str = "ctrl+alt+h"
    confirm_recent_key: str = "ctrl+alt+s"
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

    engine = data.get("engine", {})
    rewrite = data.get("rewrite", {})
    hotword = data.get("hotword", {})

    models_dir = str(engine.get("models_dir", DEFAULT_MODELS_DIR))
    resolved = Path(models_dir)
    if not resolved.is_absolute():
        # 相对路径统一基于 p1/ 根，避免受启动目录影响
        resolved = P1_ROOT / resolved

    from p1.interaction.hotkey_presets import resolve_preset
    preset = resolve_preset(data.get("hotkey", {}))
    return P1Config(
        hotkey_preset=preset.key,
        push_to_talk=preset.push_to_talk,
        cancel_key=preset.cancel,
        add_selection_key=preset.add_selection,
        confirm_recent_key=preset.confirm_recent,
        engine_adapter=str(engine.get("adapter", "sense_voice")),
        models_dir=str(resolved),
        rewrite_enabled=bool(rewrite.get("enabled", True)),
        rewrite_provider=str(rewrite.get("provider", "rules")),
        hotword_db_path=str(hotword.get("db_path", "")),
        hotword_decay_days=int(hotword.get("decay_days", 30)),
    )


def save_preset(path: Path, preset_key: str) -> None:
    """把 preset 写回 config.toml（行级处理，保留注释与其他配置行）。

    preset_key="custom" 表示删掉 preset 行（回到显式键位方案）。
    tomllib 只读，config.toml 结构简单（段落头+键值行），行级回写即可，零新依赖。
    """
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True) \
        if path.exists() else []
    new_line = f'preset = "{preset_key}"\n' if preset_key != "custom" else None

    # 定位 [hotkey] 段范围与既有 preset 行
    hotkey_start = None
    preset_idx = None
    for idx, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("["):
            if hotkey_start is not None:
                break            # 进入下一段
            if stripped == "[hotkey]":
                hotkey_start = idx
        elif hotkey_start is not None and stripped.startswith("preset"):
            preset_idx = idx

    if preset_idx is not None:                       # 已有行：替换或删除
        if new_line is None:
            del lines[preset_idx]
        else:
            lines[preset_idx] = new_line
    elif new_line is not None:
        if hotkey_start is None:                     # 无段：文件尾追加
            if lines and not lines[-1].endswith("\n"):
                lines[-1] += "\n"
            lines.append("\n[hotkey]\n" if lines else "[hotkey]\n")
            lines.append(new_line)
        else:                                        # 段内无行：段头后插入
            lines.insert(hotkey_start + 1, new_line)
    path.write_text("".join(lines), encoding="utf-8")
