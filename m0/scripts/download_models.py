#!/usr/bin/env python3
"""模型下载脚本：多源回退 + 断点续传 + 完整性校验 + 清单生成。

用法（在 m0/ 目录下）:
    .venv/Scripts/python.exe scripts/download_models.py --all
    .venv/Scripts/python.exe scripts/download_models.py --only sense_voice

下载源优先级: GitHub Releases（主源）。全部模型来自 sherpa-onnx 官方发布。
模型清单与许可证见 model_registry.py（与引擎加载共用，单一事实源）。
"""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from m0_asr.model_registry import GITHUB_BASE, HF_MIRROR_BASE, MODELS, MODELS_DIR  # noqa: E402

DOWNLOAD_DIR = MODELS_DIR / "_download"


def curl_download(url: str, dest, max_retries: int = 3) -> bool:
    """用系统 curl 下载（-C - 断点续传，-L 跟随重定向）。"""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(1, max_retries + 1):
        print(f"  下载尝试 {attempt}/{max_retries}: {url}")
        proc = subprocess.run(
            ["curl", "-L", "-C", "-", "--fail", "--retry", "3",
             "--connect-timeout", "20", "-o", str(dest), url],
            capture_output=False,
        )
        if proc.returncode == 0 and dest.exists() and dest.stat().st_size > 0:
            return True
        print(f"  失败（returncode={proc.returncode}）")
    return False


def download_from_mirror(spec) -> bool:
    """hf-mirror 逐文件下载（国内推荐源；只下必备文件，无需整包含 fp32）。"""
    if not spec.mirror_repo:
        return False
    print(f"[{spec.key}] 走 hf-mirror 文件级源: {spec.mirror_repo}")
    for rel in spec.mirror_files:
        url = f"{HF_MIRROR_BASE}/{spec.mirror_repo}/resolve/main/{rel}"
        if not curl_download(url, spec.dir / rel):
            print(f"  镜像文件失败: {rel}")
            return False
        print(f"  [ok] {rel}")
    return True


def download_from_modelscope(spec) -> bool:
    """ModelScope snapshot_download（FunASR 生态权重，国内直连快）。"""
    if not spec.ms_model_id:
        return False
    print(f"[{spec.key}] 走 ModelScope snapshot: {spec.ms_model_id}")
    try:
        from modelscope import snapshot_download
        snapshot_download(spec.ms_model_id, local_dir=str(spec.dir))
    except Exception as exc:
        print(f"  ModelScope 下载失败: {exc}")
        return False
    return True


def download_one(spec) -> bool:
    """下载单个模型：ModelScope（如有）→ hf-mirror 文件级 → GitHub tar。"""
    if spec.is_ready():
        print(f"[{spec.key}] 已存在且完整，跳过: {spec.dir}")
        return True

    if download_from_modelscope(spec) and spec.is_ready():
        print(f"[{spec.key}] 完成（ModelScope）: {spec.dir}")
        return True
    if download_from_mirror(spec) and spec.is_ready():
        print(f"[{spec.key}] 完成（hf-mirror）: {spec.dir}")
        return True
    print(f"[{spec.key}] 镜像源未完成，回退 GitHub Releases")

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOAD_DIR / f"{spec.package}.tar.bz2"
    url = f"{GITHUB_BASE}/{spec.package}.tar.bz2"
    if not curl_download(url, archive):
        print(f"[{spec.key}] 下载失败，请手动下载 {url}")
        print(f"       放置到 {archive} 后重新运行本脚本（支持断点续传）")
        return False

    print(f"  解压到 {MODELS_DIR}/ ...")
    with tarfile.open(archive, "r:bz2") as tar:
        tar.extractall(MODELS_DIR, filter="data")
    missing = [f for f in spec.required_files if not (spec.dir / f).exists()]
    if missing:
        print(f"[{spec.key}] 解压后缺文件: {missing}")
        return False
    print(f"[{spec.key}] 完成（GitHub）: {spec.dir}")
    return True


def sha256_of(path, chunk: int = 1 << 20) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        while block := fh.read(chunk):
            digest.update(block)
    return digest.hexdigest()


def write_manifest() -> None:
    """生成 models/MANIFEST.md：文件清单 + 大小 + SHA256 + 许可证。"""
    lines = [
        "# M0 模型清单",
        "",
        "> 由 scripts/download_models.py 自动生成；提交进 git，权重本体被 gitignore。",
        "",
    ]
    for spec in MODELS.values():
        lines += [f"## {spec.key} — `{spec.package}`", ""]
        lines += [f"- 许可证: **{spec.license_name}**（{spec.license_note}）"]
        if not spec.dir.exists():
            lines += ["- 状态: 未下载", ""]
            continue
        lines += ["- 状态: 已下载", "- 文件:"]
        for f in sorted(spec.dir.rglob("*")):
            if f.is_file():
                size_mb = f.stat().st_size / 1e6
                rel = f.relative_to(spec.dir).as_posix()
                # 只对权重级大文件算哈希（tokenizer 小文件跳过以省时）
                digest = sha256_of(f) if f.suffix in (".onnx", ".bin") else "-"
                lines.append(f"  - `{rel}` ({size_mb:.1f} MB) sha256=`{digest}`")
        lines.append("")
    (MODELS_DIR / "MANIFEST.md").write_text("\n".join(lines), encoding="utf-8")
    print("清单已写入 models/MANIFEST.md")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--all", action="store_true", help="下载全部模型")
    group.add_argument("--only", choices=list(MODELS), help="只下载指定模型")
    parser.add_argument("--manifest-only", action="store_true", help="只重写清单不下载")
    args = parser.parse_args()

    if args.manifest_only:
        write_manifest()
        return 0

    targets = list(MODELS) if args.all else [args.only]
    failed = [k for k in targets if not download_one(MODELS[k])]
    write_manifest()
    if failed:
        print(f"失败: {failed}")
        return 1
    print("全部模型就绪")
    return 0


if __name__ == "__main__":
    sys.exit(main())
