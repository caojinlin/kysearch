#!/usr/bin/env python3
"""生成 src/core/pinyin_data.h：常用汉字（U+4E00..U+9FA5）→ 无声调拼音表。

数据源 pypinyin（含多音字），默认读音在前。重新生成：
    pip3 install --user pypinyin && python3 tools/gen_pinyin.py
"""
from pathlib import Path

from pypinyin import pinyin, Style

OUT = Path(__file__).resolve().parent.parent / "src" / "core" / "pinyin_data.h"


def main() -> None:
    rows = []
    for cp in range(0x4E00, 0x9FA6):
        ch = chr(cp)
        pys = pinyin(ch, style=Style.NORMAL, heteronym=True)
        if not pys or not pys[0]:
            continue
        all_readings = []
        for group in pys:
            for r in group:
                r = r.strip()
                if r and r not in all_readings:
                    all_readings.append(r)
        if not all_readings:
            continue
        default = all_readings[0]
        rows.append((cp, default, ",".join(all_readings)))

    with OUT.open("w", encoding="utf-8") as f:
        f.write("// 自动生成：tools/gen_pinyin.py（数据源 pypinyin）—— 请勿手工编辑\n")
        f.write("#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\n")
        f.write("namespace kysearch::pinyin_data {\n\n")
        f.write("struct Entry {\n  std::uint32_t cp;\n  const char* def;       // 默认读音\n  const char* all;       // 全部读音，逗号分隔（默认在前）\n};\n\n")
        f.write(f"inline constexpr Entry kTable[] = {{\n")
        for cp, default, allr in rows:
            f.write(f"  {{0x{cp:X}, \"{default}\", \"{allr}\"}},\n")
        f.write("};\n\n")
        f.write("inline constexpr std::size_t kSize = sizeof(kTable) / sizeof(kTable[0]);\n\n")
        f.write("}  // namespace kysearch::pinyin_data\n")
    print(f"生成 {OUT}: {len(rows)} 字")


if __name__ == "__main__":
    main()
