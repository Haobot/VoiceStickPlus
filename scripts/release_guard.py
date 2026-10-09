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
    # r136：占位符对拍（键对等之外，值内插值 token 集须一致——单侧加 {count} 类
    # token 会造成运行期显示错乱；实测 80 对键零不一致基线 + 负检单测守护）。
    ph = re.compile("\{[a-zA-Z0-9_]+\}")
    zv, ev = _flatten_vals(zh), _flatten_vals(en)
    for key in sorted(set(zv) & set(ev)):
        if not isinstance(zv[key], str) or not isinstance(ev[key], str):
            continue
        if set(ph.findall(zv[key])) != set(ph.findall(ev[key])):
            errors.append("{key} 占位符不一致" + f": zh={sorted(set(ph.findall(zv[key])))} en={sorted(set(ph.findall(ev[key])))}")
    return "\n".join(errors) if errors else None


def _flatten_vals(d, prefix="") -> dict:
    out = {}
    for k, v in d.items():
        key = prefix + k
        if isinstance(v, dict):
            out.update(_flatten_vals(v, key + '.'))
        else:
            out[key] = v
    return out

def _swift_enum_names(body: str) -> set[str]:
    """Swift 枚举体（case a, b / case c = \"x\"）。"""
    names: set[str] = set()
    for line in re.findall(r"^\s*case\s+(.+)$", body, re.MULTILINE):
        for part in line.split(","):
            ident = part.split("=")[0].strip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", ident):
                names.add(ident)
    return names


def _cpp_enum_names(body: str) -> set[str]:
    """C++ enum class 体：成员为裸标识符（全仓 StringId 均 k 前缀），兼容尾逗号
    与 = 初值；跳过 // 注释行。"""
    names: set[str] = set()
    for line in body.splitlines():
        code = line.split("//", 1)[0].strip().rstrip(",")
        if not code or "=" in code:
            code = code.split("=", 1)[0].strip()
        if re.fullmatch(r"k[A-Za-z0-9_]*", code or ""):
            names.add(code)
    return names


def _strip_strings(text: str) -> str:
    """去掉字符串字面量（防译文里的 .foo: 或 case 被误当结构）。"""
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


def check_desktop_i18n(root: Path) -> str | None:
    """N9：桌面两端本地化奇偶守护（静态、CI 强制）——

    两端已有运行期自检（Windows LocalizationTablesAreComplete / macOS
    tablesAreComplete），但只有跑到才暴露；此检查在发布门禁静态比对：
    StringId/L10nKey 枚举 ⇄ EN 表 ⇄ ZH 表 三集合双向一致（网页 zh/en 已由
    check_i18n 覆盖）。数组表缺项不会编译报错（值初始化为空串），必须靠本检查。
    """
    errors: list[str] = []

    # ---- Windows：localization.h 枚举 vs localization.cc 两张表 ----
    wh = _text(root / "desktop" / "windows" / "src" / "localization.h")
    wc = _text(root / "desktop" / "windows" / "src" / "localization.cc")
    enum_m = re.search(r"enum\s+class\s+StringId\s*\{(.*?)\n\}", wh, re.DOTALL)
    if not enum_m:
        errors.append("windows: 未找到 enum class StringId")
    elif "EnglishStrings()" not in wc or "ChineseStrings()" not in wc:
        errors.append("windows: localization.cc 缺 EnglishStrings/ChineseStrings")
    else:
        enum_names = _cpp_enum_names(enum_m.group(1))
        en_seg = wc.split("EnglishStrings()", 1)[1].split("ChineseStrings()", 1)[0]
        zh_part = wc.split("ChineseStrings()", 1)[1]
        zh_seg = zh_part.split("kEnglish =", 1)[0] if "kEnglish =" in zh_part else zh_part
        en_names = set(re.findall(r"Index\(StringId::([A-Za-z0-9_]+)\)", en_seg))
        zh_names = set(re.findall(r"Index\(StringId::([A-Za-z0-9_]+)\)", zh_seg))
        for label, table in (("EN", en_names), ("ZH", zh_names)):
            missing = sorted(enum_names - table)
            extra = sorted(table - enum_names)
            if missing:
                errors.append(f"windows {label} 表缺项: {missing[:10]}")
            if extra:
                errors.append(f"windows {label} 表幽灵项(枚举已无): {extra[:10]}")

    # ---- macOS：Localization.swift 的 L10nKey vs english/chinese 两字典 ----
    ms = _text(root / "desktop" / "macos" / "Sources" / "VoiceStickApp" / "Localization.swift")
    menum = re.search(r"enum\s+L10nKey[^{]*\{(.*?)\n\}", ms, re.DOTALL)
    en_decl = "static let english: [L10nKey: String] = ["
    zh_decl = "static let chinese: [L10nKey: String] = ["
    if not menum:
        errors.append("macos: 未找到 enum L10nKey")
    elif en_decl not in ms or zh_decl not in ms:
        errors.append("macos: Localization.swift 缺 english/chinese 字典")
    else:
        m_enums = _swift_enum_names(menum.group(1))
        en_body = _strip_strings(ms.split(en_decl, 1)[1].split("\n    ]", 1)[0])
        zh_body = _strip_strings(ms.split(zh_decl, 1)[1].split("\n    ]", 1)[0])
        en_keys = set(re.findall(r"^\s*\.([A-Za-z0-9_]+)\s*:", en_body, re.MULTILINE))
        zh_keys = set(re.findall(r"^\s*\.([A-Za-z0-9_]+)\s*:", zh_body, re.MULTILINE))
        for label, table in (("english", en_keys), ("chinese", zh_keys)):
            missing = sorted(m_enums - table)
            extra = sorted(table - m_enums)
            if missing:
                errors.append(f"macos {label} 表缺项: {missing[:10]}")
            if extra:
                errors.append(f"macos {label} 表幽灵项(枚举已无): {extra[:10]}")

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
            "macOS OggOpusMuxer.swift",  # N1 第二刀：文件已下沉 VoiceStickCore
            _text(
                root
                / "desktop"
                / "macos"
                / "Sources"
                / "VoiceStickCore"
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


def check_test_ns_balance(root: Path) -> str | None:
    """N8 cut13（六轮 CI 教训）：desktop/windows/tests 的匿名 namespace 配对审计。

    span 切割曾把 ns 闭合线随段迁走（C1075/C2059 级联、修点又落进 main 的 try 围栏）。
    此检查按仓库约定（col0 'namespace {' 开、col0 '} // namespace' 闭，容忍多空格）
    做**位置交替**扫描：深度不得为负、文件尾必须归零，且 'int main' 必须处于全局域
    （深度 0）。tests 目录缺失时 SKIP（最小夹具不含测试目录）。
    """
    d = root / "desktop" / "windows" / "tests"
    if not d.is_dir():
        return ("skip", "desktop/windows/tests 不存在（最小夹具）")
    errors = []
    open_re = re.compile(r"^namespace\b.*\{$")  # 匿名与带名 ns 皆计
    close_re = re.compile(r"^\}[ \t]*//[ \t]*namespace")
    main_re = re.compile(r"^int main\s*\(")
    files = sorted(list(d.glob("*.cc")) + list(d.glob("*.h")))
    if not files:
        return ("skip", "tests 目录为空")
    for f in files:
        depth = 0
        saw_main = False
        for i, raw in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            if close_re.match(raw):
                depth -= 1
                if depth < 0:
                    errors.append(f"{f.name}:{i} 闭合早于任何开（深度为负）")
                    depth = 0
            elif open_re.match(raw):
                depth += 1
            elif not saw_main and main_re.match(raw):
                saw_main = True
                if depth != 0:
                    errors.append(
                        f"{f.name}:{i} int main 不在全局域（namespace 深度 {depth}）"
                    )
        if depth != 0:
            errors.append(f"{f.name} 文件尾 namespace 深度 {depth}（开闭未配平）")
    return "\n".join(errors) if errors else None


def check_doc_plan_status(root: Path) -> str | None:
    """N10：Doc/Plan 每篇必须带引用式状态行（`> 状态：<值>`）。

    目标（跟进评审 N10）：89+ 篇方案文档需状态标注 + 可观测性计数。本检查
    （1）强制新文档不缺载体（防新增无标注）；（2）目录缺失时 SKIP（最小夹具）。
    语义核销（把占位改写为真实状态）按篇另行推进——占位文案本身即
    「未核销」的诚实表达，不伪造进度。
    """
    d = root / "Doc" / "Plan"
    if not d.is_dir():
        return ("skip", "Doc/Plan 不存在（最小夹具）")
    files = sorted(d.glob("*.md"))
    if not files:
        return ("skip", "Doc/Plan 无 md 文件")
    errors = []
    marked = 0
    for f in files:
        text = f.read_text(encoding="utf-8")
        if re.search(r"^> 状态[:：]\s*\S", text, re.MULTILINE):
            marked += 1
        else:
            errors.append(f"{f.name}: 缺引用式状态行（> 状态：<值>）")
    return "\n".join(errors) if errors else None



def _walk_text(root: Path, rel: str, suffixes) -> str:
    base = root / rel
    if not base.exists():
        return ""
    parts = []
    for f in base.rglob("*"):
        if f.suffix in suffixes and "/build/" not in str(f):
            parts.append(f.read_text(encoding="utf-8", errors="ignore"))
    return "".join(parts)


def _present_event(name: str, text: str) -> bool:
    # 明引号形（case/==）与反斜杠嵌入形（C 字符串、win 构造器）双覆盖。
    q = chr(34)
    e = chr(92) + q
    return (q + name + q) in text or (e + name + e) in text


def _protocol_event_sets(root: Path):
    """返回 (state_set, ctrl_set)：protocol.md 按 State/Control 章节切向解析事件名。"""
    doc_path = root / "Doc" / "Ref" / "protocol.md"
    state_set = set()
    ctrl_set = set()
    sec = None
    for line in doc_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            sec = line[3:].strip()
        # 仅收 JSON 示例行（{ 开头）：表格/散文里的内联提及（如 Control 章引用 state 侧报告）不算
        if not line.lstrip().startswith("{"):
            continue
        for m in re.finditer(
            chr(34) + "event" + chr(34) + ":" + chr(34) + "([a-z_]+)" + chr(34),
            line,
        ):
            if sec == "State Event":
                state_set.add(m.group(1))
            elif sec == "Control Event":
                ctrl_set.add(m.group(1))
    return state_set, ctrl_set


def check_protocol_events(root: Path) -> str | None:
    """D11-①：protocol.md 事件名三端存在性（State/Control 切向 + 分端要求 + 显式豁免）。"""
    doc_path = root / "Doc" / "Ref" / "protocol.md"
    if not doc_path.exists():
        return "protocol.md 缺失"
    state_set, ctrl_set = _protocol_event_sets(root)
    if not state_set or not ctrl_set:
        return "protocol.md State/Control Event 章节切向失败（缺示例 JSON？）"
    sides = {
        "firmware": _walk_text(root, "firmware", (".c", ".h")),
        "mac": _walk_text(root, "desktop/macos/Sources", (".swift",)),
        "win": _walk_text(root, "desktop/windows/src", (".cc", ".h")),
        "scripts": _walk_text(root, "scripts", (".py",)),
    }
    # 显式豁免/范围表（每条带理由；r126 五名溯源定谳产物）。
    overrides = {
        "test_playback": (
            {"firmware", "scripts"},
            "L3 工具下发（run_l3_firmware.py），桌面端不发送",
        ),
        "gateway_keymap_get": (
            {"firmware", "win"},
            "win 连接期主动查询；mac 走 gateway_status 报告回读",
        ),
        "usb_auto_off_get": (
            {"firmware", "win"},
            "win 电池窗查询；mac 依赖 power_mgmt 主动推送回推",
        ),
    }
    dynamic_prefix = {
        "remote_button_down": "remote_button_",
        "remote_button_up": "remote_button_",
    }
    errors = []
    for name in sorted(state_set | ctrl_set):
        required, _reason = overrides.get(name, ({"firmware", "mac", "win"}, ""))
        for side in required:
            text = sides[side]
            if _present_event(name, text):
                continue
            prefix = dynamic_prefix.get(name)
            if prefix and prefix in text:
                continue
            scope = "（豁免表范围内）" if name in overrides else ""
            errors.append(f"事件 {name} 在 {side} 缺字面量{scope}")
    return chr(10).join(errors) if errors else None

def check_control_frame_schema(root: Path) -> str | None:
    """D11-②：控制帧 JSON schema 与 protocol.md Control 章节双向对拍（键/型/必填）。"""
    import json as _json

    schema_path = root / "Doc" / "Ref" / "control-frame-schema.json"
    if not schema_path.exists():
        return "Doc/Ref/control-frame-schema.json 缺失"
    try:
        schema = _json.loads(schema_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return "control-frame-schema.json 解析失败"
    _state, ctrl = _protocol_event_sets(root)
    events = schema.get(
        "events", {})
    if not isinstance(events, dict) or not events:
        return "schema 缺 events 表"
    errors = []
    doc_names, sch_names = set(ctrl), set(events)
    for n in sorted(doc_names - sch_names):
        errors.append(f"doc Control 事件 {n} 缺 schema 条目")
    for n in sorted(sch_names - doc_names):
        errors.append(f"schema 条目 {n} 不在 doc Control 章节（陈旧）")
    # 逐事件：doc 示例键/型 与 schema properties/required 对拍（重写于 r128 调试后，原生成段存隐伤）
    doc_fields = {}
    sec = None
    text = (root / "Doc" / "Ref" / "protocol.md").read_text(encoding="utf-8")
    for raw in text.splitlines():
        if raw.startswith("## "):
            sec = raw[3:].strip()
            continue
        if sec != "Control Event":
            continue
        if not raw.startswith("{"):
            continue
        try:
            obj = _json.loads(raw)
        except Exception:
            continue
        if not isinstance(obj, dict) or "event" not in obj:
            continue
        name = obj.pop("event")
        bucket = doc_fields.setdefault(name, {})
        for key, val in obj.items():
            tn = {
                "str": "string", "bool": "boolean",
                "int": "integer", "float": "number",
                "list": "array",
            }.get(type(val).__name__, type(val).__name__)
            bucket[key] = tn
    for name, props in events.items():
        if name not in doc_fields:
            continue
        want = doc_fields[name]
        got = props.get(
            "properties", {})
        if got != want:
            errors.append(f"{name} properties 与 doc 示例不一致: schema={got} doc={want}")
        if props.get(
            "required") != list(want.keys()):
            errors.append(f"{name} required 非全键或顺序不齐")
    return chr(10).join(errors) if errors else None


def check_backlog_structure(root: Path):
    """backlog 行结构与状态词汇核（r130；N10 语义核销的结构层）。

    1) 每个数据行必须以 | 收尾——抓跨物理行断行（r126 D11 行内嵌换行实案）；
    2) 状态列首词须在既定词汇（closed/open/partial/blocked/待核查）——抓错列与新造词。
    """
    path = root / 'Doc' / 'Plan' / 'backlog.md'
    if not path.exists():
        return ("skip", "backlog.md not in minimal fixture")
    allow = {'closed', 'open', 'partial', 'blocked', '待核查'}
    all_lines = path.read_text(encoding='utf-8').splitlines()
    errors = []
    for idx, raw in enumerate(all_lines, 1):
        if not raw.startswith('| '):
            continue
        if not raw.rstrip().endswith("|"):
            errors.append(f"第{idx}行行尾缺竖线（表格行断裂/跨物理行）")
            continue
        parts = [x.strip() for x in raw.split('|')]
        if len(parts) < 4:
            continue
        ident = parts[1]
        # 表头行恒后随分隔线——动态跳过（ID/编号/日期 等任意命名）
        if idx < len(all_lines) and all_lines[idx].lstrip().startswith('|---'):
            continue
        if len(ident) >= 4 and ident[:4].isdigit() and ident[4:5] == "-":
            continue  # 闭环记录日期行（列语义不同）
        status = parts[3].replace("*", "").strip()
        first = status.split(chr(0xFF08))[0].split('(')[0].strip()
        if first not in allow:
            errors.append(f"{ident} 状态词汇未知: {status[:24]}")
    return chr(10).join(errors) if errors else None

CHECKS = [
    ("versions", check_versions),
    ("tag", check_tag),
    ("appcast", check_appcast),
    ("hub", check_hub),
    ("i18n", check_i18n),
    ("i18n-desktop", check_desktop_i18n),  # N9：桌面两端枚举⇄EN⇄ZH 静态奇偶
    ("uuid", check_uuid),
    ("frame-ms", check_frame_ms_and_granule),
    # N8 cut13 教训：tests 匿名 ns 配对 + main 全局域（防 span 撕裂重演）
    ("test-ns", check_test_ns_balance),
    # N10：Doc/Plan 状态标注载体（防新增无标注；语义核销另行推进）
    ("doc-plan-status", check_doc_plan_status),
    # D11-①：协议事件名三端存在性（r126 勘察的正式化）
    ("protocol-events", check_protocol_events),
    # D11-②：控制帧 JSON schema ⇄ doc Control 章节（r128）
    ("control-frame-schema", check_control_frame_schema),
    # r130：backlog 行结构 + 状态词汇（N10 结构层语义核销）
    ("backlog-structure", check_backlog_structure),
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
