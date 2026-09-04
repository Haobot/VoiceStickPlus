"""模型注册表：包名、必备文件、许可证——下载脚本与引擎加载共用，单一事实源。"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent.parent
MODELS_DIR = M0_ROOT / "models"

# sherpa-onnx 官方发布（k2-fsa/sherpa-onnx asr-models release）
GITHUB_BASE = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
# 国内镜像（hf-mirror.com，逐文件下载，实测速度显著快于 GitHub Releases）
HF_MIRROR_BASE = "https://hf-mirror.com"


@dataclass(frozen=True)
class ModelSpec:
    """一个待下载模型的完整描述。"""

    key: str  # 内部标识，同时是 MODELS 字典键
    package: str  # sherpa-onnx 发布包名（解压后目录名一致）
    required_files: tuple[str, ...]  # 解压后必须存在的文件（相对包目录）
    license_name: str
    license_note: str
    # hf-mirror 逐文件备选源：(仓库ID, 文件相对路径)；为空则只有 GitHub 源
    mirror_repo: str = ""
    mirror_files: tuple[str, ...] = ()
    # ModelScope 模型 ID（FunASR 生态权重，经 modelscope snapshot_download 下载）
    ms_model_id: str = ""

    @property
    def dir(self) -> Path:
        """解压后的模型目录绝对路径。"""
        return MODELS_DIR / self.package

    def is_ready(self) -> bool:
        """权重是否已就位且完整。"""
        return all((self.dir / f).exists() for f in self.required_files)


SENSE_VOICE = ModelSpec(
    key="sense_voice",
    package="sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17",
    required_files=("model.int8.onnx", "tokens.txt"),
    license_name="Apache-2.0",
    license_note="FunAudioLLM/SenseVoice-Small 权重，sherpa-onnx 官方 int8 转换",
    mirror_repo="csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17",
    mirror_files=("model.int8.onnx", "tokens.txt"),
)

SEACO_PARAFORMER = ModelSpec(
    key="seaco_paraformer",
    package="sherpa-onnx-paraformer-trilingual-zh-cantonese-en",
    required_files=("model.int8.onnx", "tokens.txt"),
    license_name="Model License Agreement (ModelScope)",
    license_note=(
        "转换自 ModelScope dengcunqin/speech_seaco_paraformer_large_asr_nat-"
        "zh-cantonese-en-16k-common-vocab11666-pytorch（SeACo-Paraformer，"
        "上游遵循 FunASR/ModelScope 模型协议，商用前须通读协议原文）"
    ),
    mirror_repo="csukuangfj/sherpa-onnx-paraformer-trilingual-zh-cantonese-en",
    mirror_files=("model.int8.onnx", "tokens.txt"),
)

QWEN3_ASR = ModelSpec(
    key="qwen3_asr",
    package="sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25",
    required_files=("conv_frontend.onnx", "encoder.int8.onnx", "decoder.int8.onnx", "tokenizer/vocab.json"),
    license_name="Apache-2.0",
    license_note="QwenLM/Qwen3-ASR 0.6B 权重，社区 int8 导出（zengshuishui/Qwen3-ASR-onnx）",
    mirror_repo="pantinor/sherpa-onnx-qwen3-asr-0.6b-int8",
    mirror_files=(
        "conv_frontend.onnx",
        "encoder.int8.onnx",
        "decoder.int8.onnx",
        "tokenizer/merges.txt",
        "tokenizer/tokenizer_config.json",
        "tokenizer/vocab.json",
    ),
)

# FunASR 生态的 SeACo-Paraformer（提示词 B 路线一指定路线）。
# 注意：sherpa-onnx 导出的 trilingual SeACo 包（SEACO_PARAFORMER）不支持热词偏置
# （sherpa-onnx 的 Aho-Corasick 热词仅实现于 transducer），SeACo 偏置必须走
# FunASR 运行时加载本权重。保留 SEACO_PARAFORMER 条目作为该结论的实证对照。
FUNASR_SEACO = ModelSpec(
    key="funasr_seaco",
    package="funasr-seaco-paraformer-zh",
    required_files=("model.pt", "configuration.json"),
    license_name="Model License Agreement (ModelScope)",
    license_note=(
        "阿里达摩院 FunASR SeACo-Paraformer-large（热词定制化非自回归 ASR），"
        "上游遵循 ModelScope 模型协议，商用前须通读协议原文"
    ),
    ms_model_id="iic/speech_seaco_paraformer_large_asr_nat-zh-cn-16k-common-vocab8404-pytorch",
)

MODELS: dict[str, ModelSpec] = {
    spec.key: spec
    for spec in (SENSE_VOICE, SEACO_PARAFORMER, QWEN3_ASR, FUNASR_SEACO)
}
