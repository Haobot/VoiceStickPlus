#!/usr/bin/env python3
"""release_guard.py — 仓库一致性硬门禁（静态检查，不联网）。

用法：python3 scripts/release_guard.py [--root PATH]
退出码：0 = 全部通过；1 = 存在失败项（逐项打印后汇总）。

检查项（来源：Doc/Plan/architecture-review-followup-2026-10-07.md §5.1 动作 0.2）：
  versions   VERSION == firmware/version.txt == CHANGELOG 最新 ## vX.Y.Z
  tag        最新 tag <= VERSION；HEAD 带 vX.Y.Z tag 时必须 == VERSION（无 tag 环境 SKIP）
  appcast    website/public/appcast.xml 最新 sparkle:version 不得高于 VERSION
  hub        AGENTS/CLAUDE/CODEBUDDY 归一化后逐行一致（改一漏二即红）
  i18n       网站 zh-CN.json 与 en-US.json 键集合一致
  uuid       service UUID 四端一致：protocol.md 文本 / 固件 BLE_UUID128_INIT 字节序 /
             macOS BleProtocol.swift / Windows ble_protocol.h
  frame-ms   固件 AUDIO_FRAME_MS 与 protocol.md 表述一致，且两端 Ogg granule 步进
             由帧采样数推导出 48kHz 步进 == 48000*AUDIO_FRAME_MS/1000

设计约束：单项失败不中断，全部跑完再汇总；检查逻辑以函数暴露，供 test_release_guard.py 复用。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SERVICE_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100"
# 产品全线固定 16kHz 单声道（固件 audio_pipeline / 两端编码器一致）
SAMPLE_RATE = 16000
HUB_TOKEN = "HUB-COPIES：三者内容一致，修改整体性内容时同步更新三份，避免漂移。"
VER_RE = re.compile(r"^\d+\.\d+\.\d+$")


def _text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _version_tuple(ver: str) -> tuple[int, ...]:
    return tuple(int(p) for p in ver.split("."))


# ---------------------------------------------------------------- checks

def check_versions(root: Path) -> str | None:
    errors = []
    version = _text(root / "VERSION").strip()
    if not VER_RE.match(version):
        errors.append(f"VERSION 格式非法: {version!r}")
    fw = _text(root / "firmware" / "version.txt").strip()
    if fw != version:
        errors.append(f"firmware/version.txt={fw} != VERSION={version}")
    changelog = _text(root / "CHANGELOG.md")
    m = re.search(r"^## v(\d+\.\d+\.\d+)\s*$", changelog, re.MULTILINE)
    if not m:
        errors.append("CHANGELOG.md 无 '## vX.Y.Z' 标题")
    elif m.group(1) != version:
        errors.append(f"CHANGELOG 最新版本={m.group(1)} != VERSION={version}")
    return "\n".join(errors) if errors else None


def _git(root: Path, *args: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, timeout=30
        )
    except Exception:
        return None
    if out.returncode != 0:
        return None
    return out.stdout


def check_tag(root: Path) -> str | None | tuple[str, str]:
    raw = _git(root, "tag", "--list", "v*", "--sort=-v:refname")
    if raw is None:
        return ("skip", "git tag 不可用（浅克隆/非 git 环境）")
    tags = []
    for line in raw.splitlines():
        m = re.fullmatch(r"v(\d+\.\d+\.\d+)", line.strip())
        if m:
            tags.append(m.group(1))
    version = _text(root / "VERSION").strip()
    errors = []
    if tags:
        latest = max(tags, key=_version_tuple)
        if _version_tuple(latest) > _version_tuple(version):
            errors.append(f"最新 tag v{latest} 高于 VERSION={version}")
    heads = _git(root, "tag", "--points-at", "HEAD")
    if heads:
        for line in heads.splitlines():
            m = re.fullmatch(r"v(\d+\.\d+\.\d+)", line.strip())
            if m and m.group(1) != version:
                errors.append(
                    f"HEAD 已打 tag v{m.group(1)} 但 VERSION={version}（打 tag 必须与版本同步）"
                )
    return "\n".join(errors) if errors else None


def check_appcast(root: Path) -> str | None | tuple[str, str]:
    path = root / "website" / "public" / "appcast.xml"
    if not path.exists():
        return ("skip", "website/public/appcast.xml 不存在")
    versions = re.findall(r'sparkle:version="(\d+\.\d+\.\d+)"', _text(path))
    if not versions:
        return ("skip", "appcast 无 sparkle:version")
    version = _text(root / "VERSION").strip()
    newest = max(versions, key=_version_tuple)
    if _version_tuple(newest) > _version_tuple(version):
        return f"appcast 最新版本 {newest} 高于 VERSION={version}"
    return None


def normalize_hub(text: str) -> list[str]:
    """去掉标题行、英文引导行、同源副本句（各文件自指不同），并折叠连续空行。"""
    out: list[str] = []
    for i, line in enumerate(text.splitlines()):
        if i == 0 and line.startswith("# "):
            continue
        if line.startswith("This file provides guidance"):
            continue
        if "是本文的同源副本" in line:
            line = HUB_TOKEN
        out.append(line)
    collapsed: list[str] = []
    for line in out:
        if line == "" and collapsed and collapsed[-1] == "":
            continue
        collapsed.append(line)
    return collapsed


def check_hub(root: Path) -> str | None:
    files = ["AGENTS.md", "CLAUDE.md", "CODEBUDDY.md"]
    bodies = {}
    for name in files:
        path = root / name
        if not path.exists():
            return f"{name} 不存在"
        bodies[name] = normalize_hub(_text(path))
    errors = []
    base = bodies["AGENTS.md"]
    for name in files[1:]:
        if bodies[name] != base:
            a, b = base, bodies[name]
            n = min(len(a), len(b))
            diff_at = next((i for i in range(n) if a[i] != b[i]), n)
            errors.append(
                f"{name} 与 AGENTS.md 归一化后不一致（首个差异行 {diff_at + 1}: "
                f"{(b[diff_at] if diff_at < len(b) else '<EOF>')[:60]!r}）"
            )
    return "\n".join(errors) if errors else None


def _flatten_keys(obj, prefix: str = "") -> set[str]:
    keys: set[str] = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            keys.add(prefix + k)
            keys |= _flatten_keys(v, prefix + k + ".")
    return keys


def check_i18n(root: Path) -> str | None:
    base = root / "website" / "src" / "i18n"
    try:
        zh = json.loads(_text(base / "zh-CN.json"))
        en = json.loads(_text(base / "en-US.json"))
    except Exception as exc:
        return f"i18n JSON 解析失败: {exc}"
    zh_keys, en_keys = _flatten_keys(zh), _flatten_keys(en)
    only_zh = sorted(zh_keys - en_keys)
    only_en = sorted(en_keys - zh_keys)
    errors = []
    if only_zh:
        errors.append(f"仅 zh-CN 有: {only_zh[:8]}")
    if only_en:
        errors.append(f"仅 en-US 有: {only_en[:8]}")
    return "\n".join(errors) if errors else None


def _firmware_service_uuid_bytes(firmware_src: str) -> str | None:
    m = re.search(
        r"s_service_uuid\s*=\s*\n?\s*BLE_UUID128_INIT\(([^;]+?)\);",
        firmware_src,
        re.DOTALL,
    )
    if not m:
        return None
    nums = re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))
    if len(nums) != 16:
        return None
    b = [int(x, 16) for x in nums][::-1]  # NimBLE 小端存储 → 大端字节序
    h = "".join(f"{x:02x}" for x in b)
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


def check_uuid(root: Path) -> str | None:
    errors = []
    doc = _text(root / "Doc" / "Ref" / "protocol.md").lower()
    if SERVICE_UUID not in doc:
        errors.append("protocol.md 缺 service UUID")
    fw_src = _text(root / "firmware" / "components" / "voice_ble" / "voice_ble.c")
    fw_uuid = _firmware_service_uuid_bytes(fw_src)
    if fw_uuid is None:
        errors.append("固件 s_service_uuid/BLE_UUID128_INIT 解析失败")
    elif fw_uuid.lower() != SERVICE_UUID:
        errors.append(f"固件字节序解出 {fw_uuid} != {SERVICE_UUID}")
    swift = _text(
        root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "BleProtocol.swift"
    )
    m = re.search(r'serviceUUID\s*=\s*"([0-9A-Fa-f-]+)"', swift)
    if not m:
        errors.append("BleProtocol.swift 缺 serviceUUID 定义")
    elif m.group(1).lower() != SERVICE_UUID:
        errors.append(f"Swift serviceUUID={m.group(1)} != {SERVICE_UUID}")
    win = _text(root / "desktop" / "windows" / "src" / "ble_protocol.h")
    m = re.search(r'service_uuid\s*=\s*L"([0-9A-Fa-f-]+)"', win)
    if not m:
        errors.append("ble_protocol.h 缺 service_uuid 定义")
    elif m.group(1).lower() != SERVICE_UUID:
        errors.append(f"Windows service_uuid={m.group(1)} != {SERVICE_UUID}")
    return "\n".join(errors) if errors else None


def _resolve_samples(token: str, win_header: str, mac_swift: str) -> int | None:
    if token.isdigit():
        return int(token)
    if token == "kFrameSamples":
        m = re.search(r"kFrameSamples\s*=\s*(\d+)", win_header)
        return int(m.group(1)) if m else None
    if token == "frameSamples":
        m = re.search(r"frameSamples\s*=\s*(\d+)", mac_swift)
        return int(m.group(1)) if m else None
    return None


def check_frame_ms_and_granule(root: Path) -> str | None:
    errors = []
    fw = _text(root / "firmware" / "components" / "audio_pipeline" / "audio_pipeline.c")
    m = re.search(r"#define\s+AUDIO_FRAME_MS\s+(\d+)", fw)
    if not m:
        return "固件缺 #define AUDIO_FRAME_MS"
    frame_ms = int(m.group(1))
    doc = _text(root / "Doc" / "Ref" / "protocol.md")
    if f"encodes {frame_ms} ms of 16 kHz mono" not in doc:
        errors.append(
            f"protocol.md 帧时长表述与固件 AUDIO_FRAME_MS={frame_ms} 不一致"
            "（应含 'encodes {0} ms of 16 kHz mono'）".format(frame_ms)
        )
    expected_ticks = 48000 * frame_ms // 1000
    win_header = _text(root / "desktop" / "windows" / "src" / "audio_opus_encoder.h")
    mac_swift = _text(
        root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "AudioOpusEncoder.swift"
    )
    targets = [
        (
            "Windows ogg_opus_muxer.cc",
            _text(root / "desktop" / "windows" / "src" / "ogg_opus_muxer.cc"),
            r"(kFrameSamples|frameSamples|\d+)\)*\s*\*\s*48000\s*/\s*sample_rate_",
        ),
        (
            "macOS OggOpusMuxer.swift",
            _text(
                root
                / "desktop"
                / "macos"
                / "Sources"
                / "VoiceStickApp"
                / "OggOpusMuxer.swift"
            ),
            r"(kFrameSamples|frameSamples|\d+)\)*\s*\*\s*48_000\s*/\s*sampleRate",
        ),
    ]
    for name, src, pattern in targets:
        m = re.search(pattern, src)
        if not m:
            errors.append(f"{name}: 未找到 granule 步进表达式（帧采样数×48000/sample_rate）")
            continue
        samples = _resolve_samples(m.group(1), win_header, mac_swift)
        if samples is None:
            errors.append(f"{name}: 帧采样常量 {m.group(1)} 解析失败")
            continue
        ticks = samples * 48000 // SAMPLE_RATE
        if ticks != expected_ticks:
            errors.append(
                f"{name}: granule 步进 {ticks} ≠ 期望 {expected_ticks}"
                f"（帧采样 {samples}@{SAMPLE_RATE}Hz，AUDIO_FRAME_MS={frame_ms}）"
            )
    return "\n".join(errors) if errors else None


CHECKS = [
    ("versions", check_versions),
    ("tag", check_tag),
    ("appcast", check_appcast),
    ("hub", check_hub),
    ("i18n", check_i18n),
    ("uuid", check_uuid),
    ("frame-ms", check_frame_ms_and_granule),
]


def run(root: Path, verbose: bool = True) -> list[str]:
    failures: list[str] = []
    for name, fn in CHECKS:
        try:
            result = fn(root)
        except Exception as exc:  # 检查本身崩溃也按失败上报，绝不静默
            result = f"检查异常: {type(exc).__name__}: {exc}"
        if isinstance(result, tuple) and result and result[0] == "skip":
            if verbose:
                print(f"[SKIP] {name}: {result[1]}")
            continue
        if result is None:
            if verbose:
                print(f"[PASS] {name}")
        else:
            failures.append(f"{name}: {result}")
            if verbose:
                print(f"[FAIL] {name}:")
                for line in str(result).splitlines():
                    print(f"       {line}")
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parent.parent
    )
    args = parser.parse_args(argv)
    failures = run(args.root)
    if failures:
        print(f"\nrelease_guard: {len(failures)} 项失败")
        return 1
    print("\nrelease_guard: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
