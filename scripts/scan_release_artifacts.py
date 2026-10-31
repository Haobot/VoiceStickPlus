#!/usr/bin/env python3
"""公开发布产物凭据扫描（P0-4 发布门禁）。

在上传 GitHub Release / 镜像 COS 之前扫描"将公开"的产物，命中真实凭据即非零退出，
阻断发布。两类检测：

1) 精确值匹配：从本机 config.toml（或 --config 指定）读取真实凭据值，在所有产物中
   按 raw ASCII 与 UTF-16LE 两种编码精确搜索（覆盖编译进 exe 的内置凭据）。
2) 形态匹配：无论是否提供 config，都按已知凭据形态扫描（AKID… 腾讯 SecretId、
   sk-… 风格 LLM key、以及 key/secret/token = "长随机串" 赋值形态）。

支持二进制（exe/msi/dmg/zip 原始字节）与 zip 解包逐条目扫描（deflate 压缩内容）。

用法::

    python scripts/scan_release_artifacts.py <file> [<file> ...]
    python scripts/scan_release_artifacts.py --config <config.toml> <file> ...
    python scripts/scan_release_artifacts.py --allow-builtin <file> ...   # 内测包，慎用

退出码：0 = 未命中；1 = 命中凭据；2 = 参数/IO 错误。
限制：MSI/CAB 等二次压缩容器无法解包，仅能扫到未压缩区段；发布前仍应确保
build-msi.bat 的 VOICESTICK_EMBED_BUILTIN_KEYS / VOICESTICK_MSI_EMBED_REAL_KEYS
未被显式打开（本脚本会以 --config 精确值匹配兜底 exe 与 zip）。
"""

from __future__ import annotations

import argparse
import re
import sys
import zipfile
from pathlib import Path
from typing import Iterable, NamedTuple

# 只从 config 中提取真正敏感的字段；base_url/model/appid 不作为秘密值。
SECRET_CONFIG_KEYS = (
    "volcengine_api_key",
    "tencent_secret_id",
    "tencent_secret_key",
    "llm_api_key",
    "voicestick_api_key",
)

# 形态规则：(规则名, 正则)。value 组可选，用于掩码展示。
PATTERN_RULES = (
    ("tencent_secret_id", re.compile(rb"AKID[A-Za-z0-9]{13,}")),
    ("llm_api_key", re.compile(rb"sk-[A-Za-z0-9_\-]{16,}")),
    (
        "credential_assignment",
        re.compile(
            rb"""(?:api[_-]?key|secret[_-]?(?:key|id)|access[_-]?key|access[_-]?token|app[_-]?secret)"""
            rb"""\s*[:=]\s*["']([A-Za-z0-9/+_\-]{16,})["']""",
            re.IGNORECASE,
        ),
    ),
)

MAX_ZIP_ENTRY_BYTES = 512 * 1024 * 1024      # 单条目上限，防 zip 炸弹
MAX_ZIP_TOTAL_BYTES = 4 * 1024 * 1024 * 1024  # 单包解包总量上限



class Finding(NamedTuple):
    path: str
    location: str
    rule: str
    preview: str


def mask(value: bytes) -> str:
    text = value.decode("utf-8", errors="replace")
    if len(text) <= 6:
        return "*" * len(text)
    return text[:4] + "*" * max(4, len(text) - 6) + text[-2:]


def load_secret_values(config_path: Path | None) -> list[tuple[str, bytes]]:
    """从 config.toml 读取真实凭据值，返回 [(key, value_bytes)]；缺失/解析失败返回空。"""
    if config_path is None or not config_path.exists():
        return []
    try:
        text = config_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - 由调用方打印
        print(f"WARN: 无法读取 config {config_path}: {exc}", file=sys.stderr)
        return []
    values: list[tuple[str, bytes]] = []
    for key in SECRET_CONFIG_KEYS:
        for match in re.finditer(rf'^\s*{re.escape(key)}\s*=\s*"([^"]{{8,}})"', text, re.MULTILINE):
            value = match.group(1).encode("utf-8")
            values.append((key, value))
    return values


def scan_bytes(data: bytes, path: str, location: str,
               secret_values: Iterable[tuple[str, bytes]]) -> list[Finding]:
    findings: list[Finding] = []
    for rule, pattern in PATTERN_RULES:
        for match in pattern.finditer(data):
            findings.append(Finding(path, location, rule,
                                    mask(match.group(1) if match.groups() else match.group(0))))
    for key, value in secret_values:
        if value in data:
            findings.append(Finding(path, location, f"config_value:{key}", mask(value)))
        wide = value.decode("utf-8", errors="ignore").encode("utf-16-le")
        if wide and wide in data:
            findings.append(Finding(path, location, f"config_value_utf16:{key}", mask(value)))
    return findings


def scan_zip(path: Path, secret_values: Iterable[tuple[str, bytes]]) -> list[Finding]:
    findings: list[Finding] = []
    total = 0
    try:
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if info.is_dir():
                    continue
                if info.file_size > MAX_ZIP_ENTRY_BYTES or total + info.file_size > MAX_ZIP_TOTAL_BYTES:
                    print(f"WARN: 跳过超大条目 {path}!{info.filename} ({info.file_size} B)",
                          file=sys.stderr)
                    continue
                with archive.open(info) as handle:
                    data = handle.read()
                total += len(data)
                findings.extend(scan_bytes(data, str(path), f"!{info.filename}", secret_values))
    except (zipfile.BadZipFile, OSError) as exc:
        print(f"WARN: 无法作为 zip 读取 {path}: {exc}", file=sys.stderr)
    return findings


def scan_file(path: Path, secret_values: Iterable[tuple[str, bytes]]) -> list[Finding]:
    if not path.is_file():
        print(f"WARN: 不是文件，跳过 {path}", file=sys.stderr)
        return []
    findings: list[Finding] = []
    if zipfile.is_zipfile(path):
        findings.extend(scan_zip(path, secret_values))
    data = path.read_bytes()
    findings.extend(scan_bytes(data, str(path), "@raw", secret_values))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="扫描公开发布产物中的真实凭据")
    parser.add_argument("files", nargs="+", help="待扫描的产物路径")
    parser.add_argument("--config", help="本机 config.toml（默认 %%APPDATA%%\\\\VoiceStick\\\\config.toml）")
    parser.add_argument("--allow-builtin", action="store_true",
                        help="命中仅告警不阻断（内测包分发用，公开发布不要加）")
    args = parser.parse_args(argv)

    config_path: Path | None
    if args.config:
        config_path = Path(args.config)
    else:
        import os
        appdata = os.environ.get("APPDATA")
        config_path = Path(appdata) / "VoiceStick" / "config.toml" if appdata else None

    secret_values = load_secret_values(config_path)
    if secret_values:
        print(f"INFO: 已从 {config_path} 载入 {len(secret_values)} 个凭据值用于精确匹配")
    else:
        print("INFO: 未提供/未读到 config，仅做凭据形态扫描")

    findings: list[Finding] = []
    for name in args.files:
        findings.extend(scan_file(Path(name), secret_values))

    if not findings:
        print(f"PASS: {len(args.files)} 个产物未发现真实凭据")
        return 0

    print(f"\n{'WARN' if args.allow_builtin else 'FAIL'}: 命中 {len(findings)} 处凭据特征：")
    for finding in findings:
        print(f"  [{finding.rule}] {finding.path} {finding.location} -> {finding.preview}")
    if args.allow_builtin:
        print("（--allow-builtin：仅告警，不阻断）")
        return 0
    print("拒绝发布：请使用不含真实 key 的构建（默认 VOICESTICK_EMBED_BUILTIN_KEYS=0）。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
