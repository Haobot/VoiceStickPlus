# -*- coding: utf-8 -*-
"""从 desktop/windows/src/pinyin_data.cc 转换生成 macOS 端拼音表。

desktop/macos/Sources/VoiceStickCore/PinyinData.swift——与 Windows 端
PinyinSameOrNear 同表同口径（1:1 移植约定，见 HotwordSelector.swift 先例）。
数据源头仍是 scripts/gen_pinyin_table.py（pypinyin）；本脚本只做既有 C 表
→ Swift 表的机械转换，保证两侧字节同源，勿手改生成物。

用法（仓库根目录，任意 Python 3）：
    python3 scripts/gen_pinyin_swift.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "desktop" / "windows" / "src" / "pinyin_data.cc"
OUT = ROOT / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "PinyinData.swift"

ROW = re.compile(r"\{0x([0-9a-fA-F]+), \"([^\"]*)\", \"([^\"]*)\"\},")


def main() -> int:
    rows = [(int(m.group(1), 16), m.group(2), m.group(3))
            for m in ROW.finditer(SRC.read_text(encoding="utf-8"))]
    if not rows:
        print("pinyin_data.cc 未解析到条目", file=sys.stderr)
        return 1
    lines = [
        "// 本文件由 scripts/gen_pinyin_swift.py 从 desktop/windows/src/pinyin_data.cc",
        "// 机械转换生成，勿手改；数据源头 scripts/gen_pinyin_table.py（pypinyin，",
        "// GB2312 一级字库），与 Windows 端 PinyinSameOrNear 同表同口径。",
        f"// 共 {len(rows)} 字，按码点升序（二分查找前提）。",
        "",
        "/// 单字拼音条目：声母/韵母为逗号分隔的多读音并集（零声母为空串）。",
        "public struct PinyinEntry {",
        "    public let cp: UInt32",
        "    public let initials: String",
        "    public let finals: String",
        "",
        "    public init(cp: UInt32, initials: String, finals: String) {",
        "        self.cp = cp",
        "        self.initials = initials",
        "        self.finals = finals",
        "    }",
        "}",
        "",
        "public enum PinyinData {",
        "    /// 码点升序拼音表（二分查找）。",
        "    public static let table: [PinyinEntry] = [",
    ]
    for cp, ini, fin in rows:
        lines.append(f"        PinyinEntry(cp: 0x{cp:04x}, initials: \"{ini}\", finals: \"{fin}\"),")
    lines += [
        "    ]",
        "",
        "    /// 二分查找（表按码点升序；查不到返回 nil，调用方按不同音保守拒绝）。",
        "    public static func find(_ cp: UInt32) -> PinyinEntry? {",
        "        var low = 0",
        "        var high = table.count",
        "        while low < high {",
        "            let mid = (low + high) / 2",
        "            if table[mid].cp < cp {",
        "                low = mid + 1",
        "            } else {",
        "                high = mid",
        "            }",
        "        }",
        "        guard low < table.count, table[low].cp == cp else { return nil }",
        "        return table[low]",
        "    }",
        "}",
        "",
    ]
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows)} 条 -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
