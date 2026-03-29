#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对 parse_firehose_finder_filter.py 产出的 CSV 做「有用数据」归纳：
  - 与 chip_db.c msm_table 对比后的真正缺失 MSM
  - 名称不一致（可人工对齐）
  - 空 FullName、基带/穿戴等关键词分类
  - 各 MSM 在 ForFilter 中的记录条数（机型/哈希多样性代理）

用法:
  python tools/analyze_firehose_data.py build/firehose_msm.csv --chip-db core/src/chip_db.c \\
      -o build/firehose_useful_report.txt
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

# 与 parse 脚本一致
sys.path.insert(0, str(Path(__file__).resolve().parent))
from parse_firehose_finder_filter import load_chip_db_ids  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="分析 Firehose CSV 相对 chip_db 的有用数据")
    ap.add_argument("csv_path", type=Path, help="firehose_msm.csv")
    ap.add_argument("--chip-db", type=Path, required=True, help="core/src/chip_db.c")
    ap.add_argument("-o", "--output", type=Path, help="UTF-8 报告路径（默认可 stdout）")
    args = ap.parse_args()

    if not args.csv_path.is_file():
        print(f"找不到 CSV: {args.csv_path}", file=sys.stderr)
        return 2
    if not args.chip_db.is_file():
        print(f"找不到 chip_db: {args.chip_db}", file=sys.stderr)
        return 2

    existing = load_chip_db_ids(args.chip_db)
    rows: list[dict[str, str]] = []
    with args.csv_path.open(encoding="utf-8", errors="replace", newline="") as fp:
        for r in csv.DictReader(fp):
            rows.append(r)

    by_msm: dict[str, list[dict[str, str]]] = defaultdict(list)
    for r in rows:
        by_msm[r["msm_hex"]].append(r)

    def cat_label(s: str) -> str:
        t = (s or "").lower()
        if not (s or "").strip():
            return "空名称"
        if any(k in t for k in ("modem", "lte", "5g", "x55", "x24", "x20", "x16", "x12", "x7", "x5")):
            return "蜂窝/基带类命名"
        if "wear" in t:
            return "穿戴"
        if any(k in t for k in ("networking", "ipq", "汽车", "8155", "8295")):
            return "网络/汽车等"
        if "snapdragon" in t or "msm" in t or "sdm" in t or "sm" in t or "apq" in t:
            return "应用处理器/骁龙营销名"
        return "其他"

    lines: list[str] = []
    lines.append("=== Firehose ForFilter × chip_db 有用数据分析 ===\n")
    lines.append(f"CSV 行数: {len(rows)}  唯一 MSM: {len(by_msm)}  chip_db.msm_table 条目: {len(existing)}\n")

    in_both: list[tuple[int, str, str]] = []
    missing: list[tuple[int, str]] = []
    for msm_hex, group in sorted(by_msm.items(), key=lambda x: int(x[0], 16)):
        uid = int(msm_hex, 16)
        label = group[0].get("chip_label", "").strip()
        if uid in existing:
            if existing[uid] != label:
                in_both.append((uid, existing[uid], label))
        else:
            missing.append((uid, label))

    lines.append(f"【1】库中尚无的 MSM（可优先考虑补表）: {len(missing)} 个\n")
    for uid, lab in missing:
        h = f"{uid:08X}"
        n = len(by_msm[h])
        c = cat_label(lab)
        lab_disp = lab if lab else "（FullName 空）"
        lines.append(f"  0x{h}  |  {c}  |  ForFilter 行数={n}  |  {lab_disp}\n")

    lines.append(f"\n【2】库中已有但 Firehose FullName 与 chip_db 字符串不一致: {len(in_both)} 个\n")
    lines.append("  （多为营销名/代号差异，合并时择优或保留 chip_db 风格）\n")
    for uid, dbn, fh in sorted(in_both, key=lambda x: x[0]):
        lines.append(f"  0x{uid:08X}\n    chip_db: {dbn}\n    Firehose: {fh}\n")

    empty_name_msms = [h for h, g in by_msm.items() if not (g[0].get("chip_label") or "").strip()]
    lines.append(f"\n【3】FullName 为空的 MSM: {len(empty_name_msms)} 个 → 不宜直接写入 chip_db，需别的源补全\n")
    for h in sorted(empty_name_msms, key=lambda x: int(x, 16)):
        lines.append(f"  0x{h.upper()}  (行数 {len(by_msm[h])})\n")

    # 记录条数 Top（反映数据丰富度）
    top = sorted(((h, len(g)) for h, g in by_msm.items()), key=lambda x: -x[1])[:25]
    lines.append("\n【4】ForFilter 中行数最多的 MSM（机型/PK 组合多）Top 25\n")
    for h, cnt in top:
        lab = by_msm[h][0].get("chip_label", "")[:60]
        lines.append(f"  {cnt:4d}  0x{h.upper()}  {lab}\n")

    brands = Counter(r.get("brand", "").strip() for r in rows if r.get("brand", "").strip())
    lines.append("\n【5】品牌字段 Trademark 出现次数 Top 15（侧面反映样本覆盖）\n")
    for name, cnt in brands.most_common(15):
        lines.append(f"  {cnt:4d}  {name}\n")

    text = "".join(lines)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"已写入: {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
