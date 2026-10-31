#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""打包本地模型离线分发 zip（voicestick-models-full-v1.zip）。

供内网/手动分发场景：用户解压到 %LOCALAPPDATA%\\VoiceStick\\models\\ 即可被
桌面端识别（目录布局与 model_manifest.cc 清单一致，见
Doc/Ref/cos-distribution.md）。上传到 COS models/ 顶层或 GitHub Release
由人工/发布脚本完成，本脚本只负责本地打包与校验。

用法：
    python scripts/pack_local_models.py                 # 打包到 dist/
    python scripts/pack_local_models.py --check-only    # 只校验源文件
    python scripts/pack_local_models.py --flat-dir out/ # 另导出散文件布局

默认源为桌面端下载缓存（%LOCALAPPDATA%\\VoiceStick\\models\\
sense-voice-int8-2024-07-17），可用 --source 指定其他权威副本。
完整性硬校验：任一文件字节数或 SHA-256 与清单不符立即失败，不产出半成品。
"""

import argparse
import hashlib
import os
import sys
import zipfile
from pathlib import Path

# 与 desktop/windows/src/model_manifest.cc 保持同步（修改任一侧须同步另一侧）。
# (zip 内相对路径, 字节数, sha256)
MODEL_FILES = [
    (
        "sense-voice-int8-2024-07-17/model.int8.onnx",
        239233841,
        "c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51",
    ),
    (
        "sense-voice-int8-2024-07-17/tokens.txt",
        315894,
        "f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc",
    ),
    (
        "sense-voice-int8-2024-07-17/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf",
        1107409472,
        "b139949c5bd74937ad8ed8c8cf3d9ffb1e99c866c823204dc42c0d91fa181897",
    ),
]

# 模型均为 Apache-2.0 再分发；原文见 apache.org（上游未随包提供独立 LICENSE 文件）。
NOTICE_TEXT = """VoiceStick 本地模型离线包
==========================

内容：
  - SenseVoice int8（sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17）
    语音识别模型，源自 https://modelscope.cn/models/pengzhendong/
    sherpa-onnx-sense-voice-zh-en-ja-ko-yue
  - Qwen3-1.7B-Q4_K_M（GGUF 量化）文本精修模型，源自
    https://modelscope.cn/models/unsloth/Qwen3-1.7B-GGUF

许可证：两个模型均以 Apache License 2.0 提供，原文见
https://www.apache.org/licenses/LICENSE-2.0.txt

安装：解压到 %LOCALAPPDATA%\\VoiceStick\\models\\（macOS 为
~/Library/Application Support/VoiceStick/models/），保持
sense-voice-int8-2024-07-17 目录层级，然后在 VoiceStick 设置中选择
本地语音识别。各文件的字节数与 SHA-256 见 MANIFEST.txt。
"""

ZIP_NAME = "voicestick-models-full-v1.zip"


def default_source() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        return Path(local_app_data) / "VoiceStick" / "models"
    return Path.home() / "Library" / "Application Support" / "VoiceStick" / "models"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_source(source_root: Path) -> list[tuple[Path, str]]:
    """逐文件硬校验；返回 (绝对路径, zip 内相对路径) 列表。任一不符即退出。"""
    resolved = []
    for rel_path, expected_bytes, expected_sha256 in MODEL_FILES:
        # 源根为 models/ 父目录，zip 相对路径即缓存目录内的真实层级。
        path = source_root / rel_path
        if not path.is_file():
            sys.exit(f"错误：缺少文件 {path}")
        actual_bytes = path.stat().st_size
        if actual_bytes != expected_bytes:
            sys.exit(f"错误：{path} 字节数 {actual_bytes} != 清单 {expected_bytes}")
        print(f"校验 SHA-256 {path.name} ...", flush=True)
        actual_sha256 = sha256_of(path)
        if actual_sha256 != expected_sha256:
            sys.exit(f"错误：{path} SHA-256 不符\n  实际 {actual_sha256}\n  清单 {expected_sha256}")
        resolved.append((path, rel_path))
    return resolved


def build_manifest_text() -> str:
    lines = ["# 字节数与 SHA-256（与桌面端内置清单一致，可用于解压后自查）", ""]
    for rel_path, expected_bytes, expected_sha256 in MODEL_FILES:
        lines.append(f"{rel_path}")
        lines.append(f"  bytes:  {expected_bytes}")
        lines.append(f"  sha256: {expected_sha256}")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=None,
                        help="模型目录（默认取桌面端下载缓存）")
    parser.add_argument("--out", type=Path, default=None,
                        help=f"输出 zip 路径（默认 dist/{ZIP_NAME}）")
    parser.add_argument("--flat-dir", type=Path, default=None,
                        help="另导出散文件布局（GitHub Release 上传用）")
    parser.add_argument("--check-only", action="store_true",
                        help="只校验源文件，不打包")
    args = parser.parse_args()

    source_root = args.source or default_source()
    if not source_root.is_dir():
        sys.exit(f"错误：源目录不存在 {source_root}")
    print(f"源目录：{source_root}")

    resolved = verify_source(source_root)
    print("源文件校验全部通过")
    if args.check_only:
        return

    if args.flat_dir:
        for path, rel_path in resolved:
            target = args.flat_dir / rel_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
        (args.flat_dir / "NOTICE.txt").write_text(NOTICE_TEXT, encoding="utf-8")
        print(f"散文件布局已导出到 {args.flat_dir}")

    out_path = args.out or (Path(__file__).resolve().parent.parent / "dist" / ZIP_NAME)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # STORED：onnx/gguf 已是高熵数据，DEFLATE 收益可忽略且打包耗时数倍。
    print(f"打包 {out_path}（STORED，约 1.3 GB）...")
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_STORED,
                         allowZip64=True) as archive:
        for path, rel_path in resolved:
            archive.write(path, rel_path)
        archive.writestr("NOTICE.txt", NOTICE_TEXT)
        archive.writestr("MANIFEST.txt", build_manifest_text())

    print(f"计算 zip SHA-256 ...", flush=True)
    zip_sha256 = sha256_of(out_path)
    (out_path.with_suffix(out_path.suffix + ".sha256")).write_text(
        f"{zip_sha256}  {out_path.name}\n", encoding="ascii")
    print(f"完成：{out_path}")
    print(f"SHA-256：{zip_sha256}")


if __name__ == "__main__":
    main()
