# -*- coding: utf-8 -*-
"""生成 desktop/windows/src/pinyin_data.cc——GB2312 一级字库拼音表。

跨轮纠错守卫（PinyinSameOrNear）的字符拼音数据：每字给出全部读音的
声母集合与韵母集合（逗号分隔，零声母为空串），按码点升序排列。

数据源：pypinyin（m0/.venv，与 M0 spike same_or_near 完全同参数，保证
判定口径一致）。生成物提交仓库（~3755 字、约 120KB 源码），无运行时依赖。

用法（仓库根目录）：
    m0/.venv/Scripts/python.exe scripts/gen_pinyin_table.py
"""
import sys
from pathlib import Path

from pypinyin import lazy_pinyin, Style

OUT = Path(__file__).resolve().parent.parent / "desktop" / "windows" / "src" / "pinyin_data.cc"


def gb2312_level1_chars():
    """GB2312 一级字库（16~55 区，3755 常用字），按区位序枚举。"""
    chars = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append(ch)
            if len(chars) == 3755:
                return chars
    return chars


def main() -> int:
    rows = []
    for ch in gb2312_level1_chars():
        # 与 spike run_cross_turn_spike.py pinyin_pair 同参数（strict 默认、errors=ignore）
        initials = sorted({x for x in lazy_pinyin(ch, style=Style.INITIALS, errors="ignore")})
        finals = sorted({x for x in lazy_pinyin(ch, style=Style.FINALS, errors="ignore")})
        if not finals:
            continue  # 无读音数据（pypinyin 缺失），查不到=保守拒绝
        cp = ord(ch)
        rows.append((cp, ",".join(initials), ",".join(finals)))
    rows.sort(key=lambda r: r[0])  # 码点升序（GB2312 区位序≠码点序，二分查找前提）

    lines = [
        "// 本文件由 scripts/gen_pinyin_table.py 生成，勿手改。",
        "// 数据源 pypinyin（与 m0 spike same_or_near 同参数）；GB2312 一级字库，",
        f"// 共 {len(rows)} 字（无读音数据的字已剔除，查不到按不同音处理）。",
        "#include \"pinyin_data.h\"",
        "",
        "namespace voicestick {",
        "namespace pinyin_data {",
        "",
        "const Entry kTable[] = {",
    ]
    for cp, ini, fin in rows:
        lines.append(f"    {{0x{cp:04x}, \"{ini}\", \"{fin}\"}},")
    lines += [
        "};",
        f"const std::size_t kTableSize = {len(rows)};",
        "",
        "} // namespace pinyin_data",
        "} // namespace voicestick",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"{len(rows)} 条 -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
