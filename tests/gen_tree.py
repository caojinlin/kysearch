#!/usr/bin/env python3
"""生成确定性的合成测试树（约 3 万文件，含中文/空格/大小写/深层目录用例）。

输出期望值（shell 可 source），供 integration.sh 断言使用。
"""
import argparse
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    args = ap.parse_args()
    root = Path(args.root)

    def batch(rel_dir, name_fmt, n):
        d = root / rel_dir
        d.mkdir(parents=True, exist_ok=True)
        for i in range(n):
            (d / name_fmt.format(i=i)).touch()
        return n

    phone = batch("手机照片", "手机{i:05d}.jpg", 5000)
    bg24 = batch("工作文档", "报告_2024_{i:04d}.docx", 3000)
    bg25 = batch("工作文档", "报告_2025_{i:04d}.docx", 2000)
    rep = batch("工作文档", "report_{i:05d}.pdf", 3000)
    reps = batch("工作文档", "reports_{i:04d}.txt", 1000)
    weekly = batch("工作文档", "weekly report {i:03d}.md", 500)
    img = batch("图片", "IMG_2025{i:04d}.jpg", 5000)
    ui = batch("assets", "ui_{i:04d}.qss", 2000)
    icons = batch("assets/icons", "icon_{i:04d}.png", 4000)
    deep = batch("深层/deep/lvl1/lvl2", "深层数据_{i:04d}.dat", 5000)

    dirs = 9  # 手机照片 工作文档 图片 assets assets/icons 深层 deep lvl1 lvl2
    total_files = phone + bg24 + bg25 + rep + reps + weekly + img + ui + icons + deep

    print(f"EXPECT_TOTAL={total_files + dirs}")
    print(f"EXPECT_PHONE={phone + 1}")                # 查询: 手机（5000 文件 + 手机照片目录）
    print(f"EXPECT_BAOGAO_DOCX={bg24 + bg25}")        # 查询: 报告 docx
    print(f"EXPECT_IMG_CASE={img}")                   # 查询: --case IMG_2025
    print(f"EXPECT_IMG_CASE_LOWER=0")                 # 查询: --case img_2025
    print(f"EXPECT_WORD_REPORT={weekly}")             # 查询: --word report
    print(f"EXPECT_REPORT_ANY={rep + reps + weekly}")  # 查询: report
    print(f"EXPECT_PATH_ASSETS_ICONS={icons + 1}")    # 查询: --path assets/icons
    print(f"EXPECT_NAME_ASSETS=1")                    # 查询: assets（文件名模式）
    print(f"EXPECT_ICON={icons}")                     # 查询: icon_
    print(f"EXPECT_DEEP={deep + 1}")                  # 查询: 深层（文件 深层数据_* + 目录 深层）
    print(f"EXPECT_PY_SHOUJI={phone + 1}")             # 拼音: shouji（含目录 手机照片）
    # 首字母子串语义：sj 命中 手机(sj) 与 深层数据(scsj 内含 sj)
    print(f"EXPECT_PY_SJ={phone + 1 + deep}")
    print(f"EXPECT_PY_BAOGAO={bg24 + bg25}")           # 拼音: baogao
    print(f"EXPECT_PY_BG={bg24 + bg25}")               # 拼音首字母: bg
    print(f"EXPECT_PY_SCSJ={deep}")                    # 拼音首字母: scsj（深层数据）


if __name__ == "__main__":
    main()
