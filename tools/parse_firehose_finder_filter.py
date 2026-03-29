#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 hoplik/Firehose-Finder 的 ForFilter.xml / ForFound.xml 批量解析 MSM → 芯片说明行。

支持两种格式：
1) Access 导出的真 XML（<ForFilter> 块内 HWID / FullName / OEMID 等）——当前上游仓库默认格式。
2) 旧版「按行」文本（虽后缀 .xml，实为行导向数据库），典型块结构：
  <8位十六进制 MSM>
  <芯片说明文字>
  <OEM 字段1，常为 4 位十六进制或 0000>
  <OEM 字段2>
  <64 位十六进制 PK Hash>
  [00000000 / 00000001 等标志]
  <厂商/品牌>
  <型号>
  <机型说明…>
  [# 可选 GitHub 链接]

用法:
  python tools/parse_firehose_finder_filter.py ForFilter.xml -o build/firehose_msm.csv
  python tools/parse_firehose_finder_filter.py ForFilter.xml --json -o build/firehose_msm.json
  python tools/parse_firehose_finder_filter.py ForFilter.txt --c-snippets -o build/firehose_msm_snippets.c
  python tools/parse_firehose_finder_filter.py ForFilter.xml --compare ..\core\src\chip_db.c

数据源: https://github.com/hoplik/Firehose-Finder (MIT)
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

RE_MSM8 = re.compile(r"^[0-9A-Fa-f]{8}$")
RE_PK64 = re.compile(r"^[0-9A-Fa-f]{64}$")
RE_HEX4 = re.compile(r"^[0-9A-Fa-f]{1,4}$")
RE_FLAG8 = re.compile(r"^[0-9A-Fa-f]{8}$")  # 00000000 / 00000001


def _norm_lines(raw: str) -> list[str]:
    out: list[str] = []
    for line in raw.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        out.append(s)
    return out


def parse_records(lines: list[str]) -> list[dict[str, Any]]:
    """扫描所有 8 位 MSM 起点，尽力解析紧随其后的字段。"""
    recs: list[dict[str, Any]] = []
    n = len(lines)
    i = 0
    while i < n:
        tok = lines[i]
        if not RE_MSM8.match(tok):
            i += 1
            continue
        msm = tok.upper()
        j = i + 1
        if j >= n:
            break
        chip = lines[j]
        j += 1
        # 若「芯片行」实际是下一条 MSM，回退
        if not chip or RE_MSM8.match(chip):
            i += 1
            continue
        if RE_PK64.match(chip):
            i += 1
            continue

        oem1 = oem2 = ""
        if j < n and RE_HEX4.match(lines[j]):
            oem1 = lines[j].upper()
            j += 1
        if j < n and RE_HEX4.match(lines[j]):
            oem2 = lines[j].upper()
            j += 1

        pk = ""
        if j < n and RE_PK64.match(lines[j]):
            pk = lines[j].upper()
            j += 1

        if j < n and RE_FLAG8.match(lines[j]) and lines[j].upper() not in (msm,):
            # 常见 00000000 / 00000001
            j += 1

        brand = model = note = ""
        if j < n and lines[j] and not RE_MSM8.match(lines[j]) and not RE_PK64.match(lines[j]):
            brand = lines[j]
            j += 1
        if j < n and lines[j] and not RE_MSM8.match(lines[j]) and not RE_PK64.match(lines[j]):
            model = lines[j]
            j += 1
        if j < n and lines[j] and not RE_MSM8.match(lines[j]) and not RE_PK64.match(lines[j]):
            note = lines[j]
            j += 1

        recs.append(
            {
                "msm_hex": msm,
                "msm_uint": int(msm, 16),
                "chip_label": chip,
                "oem1": oem1,
                "oem2": oem2,
                "pk_hash": pk,
                "brand": brand,
                "model": model,
                "note": note,
            }
        )
        i += 1
    return recs


def _ff_child_text(parent: ET.Element, tag: str) -> str:
    el = parent.find(tag)
    if el is None or el.text is None:
        return ""
    return el.text.strip()


def _clean_hash_url(s: str) -> str:
    s = s.strip()
    while s.startswith("#"):
        s = s[1:].strip()
    while s.endswith("#"):
        s = s[:-1].strip()
    return s


def parse_forfilter_xml_records(raw: str) -> list[dict[str, Any]]:
    """解析 Firehose-Finder 当前使用的 Access/XML 导出（dataroot 下多条 ForFilter）。"""
    try:
        root = ET.fromstring(raw)
    except ET.ParseError:
        return []

    recs: list[dict[str, Any]] = []
    for ff in root.iter("ForFilter"):
        hw = _ff_child_text(ff, "HWID").upper().replace(" ", "")
        if not RE_MSM8.match(hw):
            continue
        chip = _ff_child_text(ff, "FullName")
        oem1 = _ff_child_text(ff, "OEMID").upper()
        oem2 = _ff_child_text(ff, "MODELID").upper()
        pk = _ff_child_text(ff, "HASHID").upper().replace(" ", "")
        if pk and not RE_PK64.match(pk):
            pk = ""
        brand = _ff_child_text(ff, "Trademark")
        model = _ff_child_text(ff, "Model")
        alt = _ff_child_text(ff, "AltName")
        url = _clean_hash_url(_ff_child_text(ff, "Url"))
        note = alt
        if url:
            note = f"{alt} | {url}" if alt else url

        recs.append(
            {
                "msm_hex": hw,
                "msm_uint": int(hw, 16),
                "chip_label": chip,
                "oem1": oem1,
                "oem2": oem2,
                "pk_hash": pk,
                "brand": brand,
                "model": model,
                "note": note,
            }
        )
    return recs


def load_records(path: Path, raw: str) -> list[dict[str, Any]]:
    """优先识别真 XML ForFilter；否则按行解析。"""
    head = raw[:12000] if len(raw) > 12000 else raw
    if "<?xml" in head[:800] and "<ForFilter>" in head:
        xml_recs = parse_forfilter_xml_records(raw)
        if xml_recs:
            return xml_recs
    lines = _norm_lines(raw)
    return parse_records(lines)


def _msm_table_block(chip_db_text: str) -> str:
    """仅截取 msm_table[] 初始化段，避免误扫 vendor_table 里的 {0x0000,...} 等 OEM 行。"""
    keys = (
        "static const msm_entry_t msm_table[] = {",
        "msm_entry_t msm_table[] = {",
    )
    i = -1
    for k in keys:
        i = chip_db_text.find(k)
        if i >= 0:
            break
    if i < 0:
        return ""
    j = chip_db_text.find("{", i)
    if j < 0:
        return ""
    j += 1
    k = chip_db_text.find("#define MSM_TABLE_SIZE", j)
    if k < 0:
        k = chip_db_text.find("static const vendor_entry_t", j)
    if k < 0:
        k = chip_db_text.find("typedef struct", j)
    return chip_db_text[j:k] if k > j else ""


def load_chip_db_ids(chip_db_path: Path) -> dict[int, str]:
    """从 chip_db.c 的 msm_table 提取 {0x....,"..."} 的 ID 与名称（完整 32 位 HWID，含 0x0005xxxx 等）。"""
    text = chip_db_path.read_text(encoding="utf-8", errors="replace")
    block = _msm_table_block(text)
    if not block:
        return {}
    pat = re.compile(r"\{0x([0-9A-Fa-f]+),\"([^\"]*)\"" )
    out: dict[int, str] = {}
    for m in pat.finditer(block):
        try:
            uid = int(m.group(1), 16)
        except ValueError:
            continue
        out[uid] = m.group(2)
    return out


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit_c_snippets(
    rows: list[dict[str, Any]],
    out_fp,
    only_new: dict[int, str] | None,
    *,
    dedupe_msm: bool,
):
    out_fp.write(
        "/* 由 tools/parse_firehose_finder_filter.py 自 Firehose-Finder 生成；合并前请人工校对 */\n"
    )
    seen: set[int] = set()
    # 同一 MSM 多行时：dedupe 只保留首次出现（或可先 CSV 里按频次选手工挑）
    for r in rows:
        uid = r["msm_uint"]
        if only_new is not None and uid in only_new:
            continue
        if dedupe_msm and uid in seen:
            continue
        seen.add(uid)
        name = c_escape(r["chip_label"])
        out_fp.write(f'    {{0x{r["msm_hex"]},"{name}"}},\n')


def main() -> int:
    ap = argparse.ArgumentParser(description="解析 Firehose-Finder ForFilter/ForFound 行格式 MSM 表")
    ap.add_argument("input", type=Path, help="ForFilter.xml 或 ForFound.xml 本地路径")
    ap.add_argument("-o", "--output", type=Path, help="输出文件（默认可 stdout）")
    ap.add_argument("--json", action="store_true", help="输出 JSON 数组")
    ap.add_argument("--c-snippets", action="store_true", help="输出 C 表项片段（需 -o）")
    ap.add_argument(
        "--compare",
        type=Path,
        metavar="chip_db.c",
        help="与 core/src/chip_db.c 对比，在 stderr 打印已有/冲突统计",
    )
    ap.add_argument(
        "--summary",
        action="store_true",
        help="stderr 打印 MSM 唯一数、记录条数、各 MSM 名称种类数",
    )
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"文件不存在: {args.input}", file=sys.stderr)
        return 2

    raw = args.input.read_text(encoding="utf-8", errors="replace")
    recs = load_records(args.input, raw)

    if args.summary or args.compare:
        by_msm: defaultdict[str, Counter] = defaultdict(Counter)
        for r in recs:
            by_msm[r["msm_hex"]][r["chip_label"]] += 1
        print(f"解析记录数: {len(recs)}", file=sys.stderr)
        print(f"唯一 MSM 数: {len(by_msm)}", file=sys.stderr)
        multi = sum(1 for c in by_msm.values() if len(c) > 1)
        print(f"同一 MSM 对应多种 chip_label 的个数: {multi}", file=sys.stderr)

    existing: dict[int, str] = {}
    if args.compare:
        existing = load_chip_db_ids(args.compare)
        rows_new = 0
        rows_in_db = 0
        msm_in_file = {r["msm_uint"] for r in recs}
        msm_only_in_file = msm_in_file - set(existing.keys())
        msm_in_both = msm_in_file & set(existing.keys())
        label_mismatch_msm: set[int] = set()
        for r in recs:
            uid = r["msm_uint"]
            if uid not in existing:
                rows_new += 1
            else:
                rows_in_db += 1
                if existing[uid] != r["chip_label"]:
                    label_mismatch_msm.add(uid)
        print(
            f"对比 {args.compare}: chip_db msm 条目 {len(existing)}；"
            f"解析记录中 MSM 已在库 {rows_in_db} 行 / 库中无此 MSM {rows_new} 行；"
            f"文件中曾出现的唯一 MSM {len(msm_in_file)} 个，其中库中完全没有的 MSM {len(msm_only_in_file)} 个；"
            f"库与文件 chip_label 字符串不完全一致的 MSM（去重）约 {len(label_mismatch_msm)} 个",
            file=sys.stderr,
        )

    out = args.output
    if args.json:
        payload = json.dumps(recs, ensure_ascii=False, indent=2)
        if out:
            out.write_text(payload, encoding="utf-8")
        else:
            print(payload)
        return 0

    if args.c_snippets:
        if not out:
            print("使用 --c-snippets 时请指定 -o", file=sys.stderr)
            return 2
        only_new = existing if args.compare else None
        with out.open("w", encoding="utf-8", newline="\n") as fp:
            emit_c_snippets(recs, fp, only_new, dedupe_msm=True)
        print(f"已写入 C 片段: {out}", file=sys.stderr)
        return 0

    # 默认 CSV
    fieldnames = [
        "msm_hex",
        "msm_uint",
        "chip_label",
        "oem1",
        "oem2",
        "pk_hash",
        "brand",
        "model",
        "note",
    ]
    if out:
        fp = out.open("w", encoding="utf-8", newline="", errors="replace")
        close_fp = True
    else:
        fp = sys.stdout
        close_fp = False
    try:
        w = csv.DictWriter(fp, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        for r in recs:
            row = {k: r.get(k, "") for k in fieldnames}
            row["msm_uint"] = str(row["msm_uint"])
            w.writerow(row)
    finally:
        if close_fp:
            fp.close()
        elif out:
            pass

    if out:
        print(f"已写入 CSV: {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
