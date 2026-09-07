"""Provider 注册表与工厂：编排层（评测管线/Web UI）只经由本入口拿引擎实例。"""
from __future__ import annotations

from pathlib import Path

from m0_asr.platform_config import CloudTencentCredentials

from .base import AsrProvider, HealthStatus, TranscribeOutcome
from .cloud_tencent import CloudTencentProvider
from .local_seaco import LocalSeacoProvider
from .local_sense_voice import LocalSenseVoiceProvider

M0_ROOT = Path(__file__).resolve().parent.parent.parent.parent
HOTWORDS_TXT = M0_ROOT / "data" / "texts" / "hotwords.txt"

_REGISTRY: dict[str, type[AsrProvider]] = {
    LocalSenseVoiceProvider.name: LocalSenseVoiceProvider,
    LocalSeacoProvider.name: LocalSeacoProvider,
    CloudTencentProvider.name: CloudTencentProvider,
    "cloud_tencent_hotword": CloudTencentProvider,
}


def read_hotwords(path: Path = HOTWORDS_TXT) -> list[str]:
    """读热词文件（与 SeACo 双层管线同源，跨引擎公平对比）。"""
    if not path.exists():
        return []
    return [line.strip() for line in
            path.read_text(encoding="utf-8").splitlines() if line.strip()]


def registered_provider_names() -> list[str]:
    return sorted(_REGISTRY)


def get_provider(name: str, credentials: CloudTencentCredentials | None = None) -> AsrProvider:
    if name not in _REGISTRY:
        raise KeyError(f"未注册的 ASR 引擎: {name!r}，可选: {registered_provider_names()}")
    cls = _REGISTRY[name]
    if cls is CloudTencentProvider:
        hotwords = read_hotwords() if name == "cloud_tencent_hotword" else None
        return cls(credentials=credentials, hotword_list=hotwords, name=name)
    if cls is LocalSeacoProvider:
        # 双层热词管线与云端热词变体同源（hotwords.txt），对比才公平
        return cls(hotwords_file=HOTWORDS_TXT)
    return cls()
