#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
按 core/src/chip_db.c 中 edl_chip_name() 的等价逻辑，核对 Firehose「库外」MSM：
  - exact / alt_e1 / core_mask 命中时，实机 UI 已非 Unknown，仅需决定是否补精确表项
  - 真 Unknown 为建议补表候选
  - Firehose 标签与模糊命中名对比，标出「表述是否一致」

用法:
  python tools/verify_firehose_chip_db.py --chip-db core/src/chip_db.c \\
      --new-only-c build/firehose_new_only.c -o build/firehose_verify_report.txt
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parse_firehose_finder_filter import _msm_table_block  # noqa: E402


def load_msm_table_ordered(chip_db_path: Path) -> list[tuple[int, str]]:
    text = chip_db_path.read_text(encoding="utf-8", errors="replace")
    block = _msm_table_block(text)
    pat = re.compile(r"\{0x([0-9A-Fa-f]+),\"([^\"]*)\"" )
    return [(int(m.group(1), 16), m.group(2)) for m in pat.finditer(block)]


def edl_chip_name_resolve(msm_id: int, table: list[tuple[int, str]]) -> tuple[str, str]:
    """返回 (名称, 命中方式: exact|alt_e1|core_mask|none)。"""
    for uid, name in table:
        if uid == msm_id:
            return name, "exact"
    if (msm_id & 0xFF) != 0xE1:
        alt = (msm_id & 0xFFFFFF00) | 0xE1
        for uid, name in table:
            if uid == alt:
                return name, "alt_e1"
    core = msm_id & 0x00FFFFF0
    for uid, name in table:
        if (uid & 0x00FFFFF0) == core:
            return name, "core_mask"
    return "Unknown", "none"


def parse_new_only_c(path: Path) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    pat = re.compile(r"\{0x([0-9A-Fa-f]+),\"([^\"]*)\"" )
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.search(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2)))
    return out


def norm_hint(s: str) -> str:
    return "".join(c.lower() for c in s if c.isalnum())


def marketing_overlap(chip_db_name: str, fh_label: str) -> bool:
    """粗判 Firehose 营销名是否与 chip_db 字符串「可能指同一系」。"""
    if not fh_label.strip():
        return False
    a, b = norm_hint(chip_db_name), norm_hint(fh_label)
    if len(b) < 3:
        return False
    # 数字系列：625、730、855、8gen1 等子串
    toks = ["625", "636", "630", "660", "670", "675", "710", "712", "730", "845", "855", "865", "888", "778", "750", "765", "8gen1", "8gen2", "8gen3", "8elite", "7gen1", "7gen3", "6gen1", "6gen3", "4gen", "835", "821", "820", "410", "400", "425", "430", "617", "808", "801", "439", "205", "x55", "x24", "x20", "x16", "x12", "x7", "x5", "modem", "lte"]
    for t in toks:
        if t in b and t in a:
            return True
    # 括号内骁龙名
    if "snapdragon" in b:
        for part in ("Snapdragon", "SDM", "SM", "MSM", "APQ"):
            if part.lower() in a:
                return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="核对 Firehose 库外 MSM 与 edl_chip_name 行为")
    ap.add_argument("--chip-db", type=Path, required=True)
    ap.add_argument("--new-only-c", type=Path, required=True, help="parse 脚本生成的 firehose_new_only.c")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    table = load_msm_table_ordered(args.chip_db)
    rows = parse_new_only_c(args.new_only_c)

    lines: list[str] = []
    lines.append("=== Firehose「库外」MSM × edl_chip_name 核对 ===\n")
    lines.append(
        "说明：load_chip_db_ids 仅统计「精确表项」。下列 ID 无精确行，但 edl_chip_name 可能通过 "
        "末字节 E1 或 core_mask 已显示别名。\n\n"
    )

    buckets: dict[str, list[str]] = {
        "A_真缺失_当前Unknown": [],
        "B_已解析_与Firehose表述可认为一致": [],
        "C_已解析_与Firehose表述不一致或需人工": [],
        "D_FullName空_但chip_db已可解析": [],
        "E_FullName空_当前Unknown": [],
        "F_特殊ID需第二来源": [],
    }

    for uid, fh in rows:
        h = f"{uid:08X}"
        name, how = edl_chip_name_resolve(uid, table)
        if not fh.strip():
            if how == "none":
                buckets["E_FullName空_当前Unknown"].append(f"  0x{h}  edl_chip_name→「{name}」({how})\n")
            else:
                buckets["D_FullName空_但chip_db已可解析"].append(
                    f"  0x{h}  edl_chip_name→「{name}」({how})\n"
                )
            continue
        if how == "none":
            if uid == 0x30020000 or uid > 0xF0000000:
                buckets["F_特殊ID需第二来源"].append(
                    f"  0x{h}  Firehose:「{fh}」  edl: Unknown  （非常规 HWID 形态，合并前必查 Sahara 日志）\n"
                )
            else:
                buckets["A_真缺失_当前Unknown"].append(f"  0x{h}  Firehose:「{fh}」\n")
            continue
        # 已能解析
        agree = fh.strip() in name or name in fh or marketing_overlap(name, fh)
        line = f"  0x{h}  [{how}] edl:「{name}」  Firehose:「{fh}」\n"
        if agree:
            buckets["B_已解析_与Firehose表述可认为一致"].append(line)
        else:
            buckets["C_已解析_与Firehose表述不一致或需人工"].append(line)

    for title, items in buckets.items():
        lines.append(f"【{title}】 {len(items)} 条\n")
        lines.extend(items if items else ["  （无）\n"])
        lines.append("\n")

    lines.append("【建议】\n")
    lines.append("  · A：优先补 msm_table 精确行（名称建议沿用 chip_db 代号风格：MSM/SM/SDM + 骁龙括号）。\n")
    lines.append("  · B：可选补精确行以固定显示；不补则 UI 已通过模糊匹配可读。\n")
    lines.append("  · C：核对是否同硅不同 HWID 或 Firehose 误标；再决定表项或注释。\n")
    lines.append("  · D：可保持现状；chip_db 已能正确显示，仅缺 Firehose FullName 佐证。\n")
    lines.append("  · E：禁止仅用 ForFilter 补表；需 msmids / 实机 / 其它库。\n")
    lines.append("  · F：确认是否为 OEM 自定义字段或导出错误。\n")

    lines.append("\n【人工重点核对（与 chip_db 已有条目关系）】\n")
    manual = [
        ("0x30020000", "非典型 HWID；若已按特定样本补精确行，请保留“非标准导出 HWID”注释，勿参与常规 HWID 推断。"),
        ("0x0009C0E1", "营销名 660；表中已有 0x0008C0E1 SDM660。可能为另一 HWID 变体，补表时勿覆盖原行。"),
        ("0x000E60E1", "营销名 730；表为 0x000E70E1 SM7150(730)。核对 bkerler msmids 是否同系不同 ID。"),
        ("0x000E90E1", "营销名 855；表为 0x000A50E1 SM8150(855)。常见并列 HWID，补精确行即可。"),
        ("0x001870E1", "营销名 8 Gen 1；表为 0x001620E1 SM8450。并列 ID，补行。"),
        ("0x001D90E1", "营销名 8+ Gen 1；表为 0x001900E1 SM8475。并列 ID，补行。"),
        ("0x001CB0E1", "营销名 8 Gen 2；表为 0x001CA0E1 SM8550。并列 ID，补行。"),
        ("0x002270E1", "营销名 8 Gen 3；表为 0x0022A0E1/0x002280E1 SM8650。并列 ID，补行。"),
        ("0x0028D0E1", "营销名 8 Elite；表为 0x0028C0E1 SM8750。HWID 差 0x10，确认非 OCR/导出错误后再补。"),
        ("0x0007E0E1", "营销名 630；表有 0x000AC0E1 SDM630 等。可能变体，查 msmids。"),
        ("0x001080E1", "营销名 712；表为 0x000DD0E1 SDM712。并列 ID。"),
        ("0x000CF0E1", "X55 Modem；表有 0x0009E0E1 SDX55(X55)。名称统一风格即可。"),
        ("0x001AD0E1", "Networking Pro 620（网卡）；与手机 SoC 并列存在，单独一行合理。"),
        ("0x0004A0E1 / 0x007F10E1 / 0x007F40E1", "X5 Modem；表有 0x000480E1 MDM9207(蜂窝IoT) 等，勿把不同基带 HWID 合并为一行。"),
    ]
    for hid, note in manual:
        lines.append(f"  {hid}: {note}\n")

    args.output.write_text("".join(lines), encoding="utf-8")
    print(f"已写入: {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
