#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Derive conservative exact-brand tables from Firehose-Finder CSV samples.

Inputs:
- build/firehose_msm.csv
- core/src/chip_db.c

Outputs:
- a human-readable report
- optional C snippets for missing stable entries

Design goals:
- prefer low-risk exact display mappings
- only recommend exact PK-prefix entries when the prefix is stable
- separate strong candidates from manual-review items
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


GENERIC_BRANDS = {
    "",
    "Unknown",
    "QUALCOMM",
    "Qualcomm",
}


BRAND_NORMALIZE = {
    "motorola": "Motorola",
    "motorola solutions": "Motorola Solutions",
    "lenovo": "Lenovo",
    "xiaomi": "Xiaomi",
    "redmi": "Redmi",
    "poco": "POCO",
    "vivo": "Vivo",
    "iqoo": "iQOO",
    "huawei": "Huawei",
    "honor": "Honor",
    "oneplus": "OnePlus",
    "oppo": "OPPO",
    "realme": "Realme",
    "google": "Google",
    "nokia": "Nokia",
    "tcl": "TCL",
    "alcatel": "Alcatel",
    "asus": "Asus",
    "lg": "LG",
    "lge": "LG",
}

FAMILY_ALIASES = {
    "huawei": {"Huawei", "Honor"},
    "honor": {"Huawei", "Honor"},
    "motorola": {"Motorola", "Motorola Solutions", "Lenovo"},
    "lenovo": {"Motorola", "Motorola Solutions", "Lenovo"},
    "xiaomi": {"Xiaomi", "Redmi", "POCO"},
    "redmi": {"Xiaomi", "Redmi", "POCO"},
    "poco": {"Xiaomi", "Redmi", "POCO"},
    "vivo": {"Vivo", "iQOO"},
    "iqoo": {"Vivo", "iQOO"},
}


def normalize_brand(raw: str) -> str:
    brand = (raw or "").strip()
    if not brand:
        return ""
    return BRAND_NORMALIZE.get(brand.casefold(), brand)


def expand_brand_filters(values: list[str]) -> set[str]:
    out: set[str] = set()
    for value in values:
        for token in value.split(","):
            token = token.strip()
            if not token:
                continue
            normalized = normalize_brand(token)
            out.add(normalized)
            out.update(FAMILY_ALIASES.get(token.casefold(), set()))
            out.update(FAMILY_ALIASES.get(normalized.casefold(), set()))
    return out


def escape_c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def extract_array_block(text: str, array_name: str) -> str:
    marker = f"{array_name}[] = {{"
    start = text.find(marker)
    if start < 0:
        return ""
    start = text.find("{", start)
    if start < 0:
        return ""
    depth = 0
    for idx in range(start, len(text)):
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1 : idx]
    return ""


def load_existing_exact_oem_model(chip_db_path: Path) -> dict[tuple[str, str], str]:
    text = chip_db_path.read_text(encoding="utf-8", errors="replace")
    block = extract_array_block(text, "precise_brand_table")
    pat = re.compile(r"\{0x([0-9A-Fa-f]{1,4}),0x([0-9A-Fa-f]{1,4}),\"([^\"]+)\"\}")
    out: dict[tuple[str, str], str] = {}
    for match in pat.finditer(block):
        oem = match.group(1).upper().zfill(4)
        model = match.group(2).upper().zfill(4)
        out[(oem, model)] = match.group(3)
    return out


def load_existing_exact_pk(chip_db_path: Path) -> dict[str, str]:
    text = chip_db_path.read_text(encoding="utf-8", errors="replace")
    block = extract_array_block(text, "precise_display_pk_table")
    pat = re.compile(r"\{\"([0-9A-Fa-f]{8})\",\"([^\"]+)\"\}")
    out: dict[str, str] = {}
    for match in pat.finditer(block):
        out[match.group(1).upper()] = match.group(2)
    return out


@dataclass(frozen=True)
class OemModelCandidate:
    oem: str
    model: str
    brand: str
    count: int


@dataclass(frozen=True)
class PkCandidate:
    prefix: str
    brand: str
    count: int


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    with csv_path.open(encoding="utf-8-sig", errors="replace", newline="") as fp:
        for row in csv.DictReader(fp):
            row["brand"] = normalize_brand(row.get("brand", ""))
            rows.append(row)
    return rows


def stable_oem_model_candidates(rows: list[dict[str, str]]) -> tuple[list[OemModelCandidate], list[tuple[str, str, Counter[str]]]]:
    grouped: dict[tuple[str, str], Counter[str]] = defaultdict(Counter)
    for row in rows:
        oem = (row.get("oem1") or "").strip().upper()
        model = (row.get("oem2") or "").strip().upper()
        brand = row.get("brand", "")
        if not oem or not model or model == "0000" or brand in GENERIC_BRANDS:
            continue
        grouped[(oem, model)][brand] += 1

    stable: list[OemModelCandidate] = []
    conflicts: list[tuple[str, str, Counter[str]]] = []
    for (oem, model), counter in sorted(grouped.items()):
        if len(counter) == 1:
            brand, count = counter.most_common(1)[0]
            stable.append(OemModelCandidate(oem=oem, model=model, brand=brand, count=count))
        else:
            conflicts.append((oem, model, counter))
    return stable, conflicts


def stable_pk_candidates(rows: list[dict[str, str]]) -> tuple[list[PkCandidate], list[tuple[str, Counter[str]]]]:
    grouped: dict[str, Counter[str]] = defaultdict(Counter)
    for row in rows:
        pk_hash = (row.get("pk_hash") or "").strip().upper()
        brand = row.get("brand", "")
        if len(pk_hash) < 8 or brand in GENERIC_BRANDS:
            continue
        grouped[pk_hash[:8]][brand] += 1

    stable: list[PkCandidate] = []
    conflicts: list[tuple[str, Counter[str]]] = []
    for prefix, counter in sorted(grouped.items()):
        if len(counter) == 1:
            brand, count = counter.most_common(1)[0]
            stable.append(PkCandidate(prefix=prefix, brand=brand, count=count))
        else:
            conflicts.append((prefix, counter))
    return stable, conflicts


def emit_snippets(
    out_path: Path,
    exact_oem_model: list[OemModelCandidate],
    exact_pk: list[PkCandidate],
) -> None:
    lines: list[str] = []
    lines.append("/* Generated by tools/derive_precise_brand_tables.py */\n")
    lines.append("/* Conservative missing exact-brand candidates only. */\n\n")
    lines.append("/* precise_brand_table additions */\n")
    for item in sorted(exact_oem_model, key=lambda x: (x.brand, x.oem, x.model)):
        lines.append(
            f'{{0x{item.oem},0x{item.model},"{escape_c_string(item.brand)}"}}, /* count={item.count} */\n'
        )
    lines.append("\n/* precise_display_pk_table additions */\n")
    for item in sorted(exact_pk, key=lambda x: (x.brand, x.prefix)):
        lines.append(
            f'{{"{item.prefix.lower()}","{escape_c_string(item.brand)}"}}, /* count={item.count} */\n'
        )
    out_path.write_text("".join(lines), encoding="utf-8")


def emit_report(
    out_path: Path,
    rows: list[dict[str, str]],
    missing_oem_model: list[OemModelCandidate],
    conflict_oem_model: list[tuple[str, str, Counter[str]]],
    strong_pk: list[PkCandidate],
    weak_pk: list[PkCandidate],
    conflict_pk: list[tuple[str, Counter[str]]],
) -> None:
    lines: list[str] = []
    lines.append("=== Exact Brand Candidate Report ===\n\n")
    lines.append(f"rows: {len(rows)}\n")
    lines.append(f"missing exact OEM+Model candidates: {len(missing_oem_model)}\n")
    lines.append(f"strong exact PK candidates: {len(strong_pk)}\n")
    lines.append(f"weak exact PK candidates: {len(weak_pk)}\n")
    lines.append(f"conflicting OEM+Model pairs: {len(conflict_oem_model)}\n")
    lines.append(f"conflicting PK prefixes: {len(conflict_pk)}\n\n")

    lines.append("[A] Missing exact OEM+Model candidates\n")
    for item in sorted(missing_oem_model, key=lambda x: (x.brand, x.oem, x.model)):
        lines.append(f"  0x{item.oem}/0x{item.model} -> {item.brand}  (samples={item.count})\n")
    if not missing_oem_model:
        lines.append("  (none)\n")
    lines.append("\n")

    lines.append("[B] Strong exact PK candidates (stable, samples >= 2)\n")
    for item in sorted(strong_pk, key=lambda x: (x.brand, x.prefix)):
        lines.append(f"  {item.prefix} -> {item.brand}  (samples={item.count})\n")
    if not strong_pk:
        lines.append("  (none)\n")
    lines.append("\n")

    lines.append("[C] Weak exact PK candidates (stable, samples = 1)\n")
    for item in sorted(weak_pk, key=lambda x: (x.brand, x.prefix)):
        lines.append(f"  {item.prefix} -> {item.brand}  (samples=1)\n")
    if not weak_pk:
        lines.append("  (none)\n")
    lines.append("\n")

    lines.append("[D] Globally conflicting OEM+Model pairs touching this selection\n")
    for oem, model, counter in conflict_oem_model:
        joined = ", ".join(f"{brand}:{count}" for brand, count in counter.most_common())
        lines.append(f"  0x{oem}/0x{model} -> {joined}\n")
    if not conflict_oem_model:
        lines.append("  (none)\n")
    lines.append("\n")

    lines.append("[E] Globally conflicting PK prefixes touching this selection\n")
    for prefix, counter in conflict_pk:
        joined = ", ".join(f"{brand}:{count}" for brand, count in counter.most_common())
        lines.append(f"  {prefix} -> {joined}\n")
    if not conflict_pk:
        lines.append("  (none)\n")
    lines.append("\n")

    lines.append("[Policy]\n")
    lines.append("  - Prefer OEM+Model exact entries whenever brand is unique.\n")
    lines.append("  - Only auto-add PK prefixes when the prefix is stable.\n")
    lines.append("  - Treat weak PK prefixes as manual-review unless corroborated by another source.\n")
    lines.append("  - Use family fallback when evidence is mixed.\n")
    out_path.write_text("".join(lines), encoding="utf-8")


def filter_candidates_by_brand(items: list, allowed: set[str], brand_getter):
    if not allowed:
        return items
    return [item for item in items if brand_getter(item) in allowed]


def filter_conflict_oem_model(
    items: list[tuple[str, str, Counter[str]]],
    allowed: set[str],
) -> list[tuple[str, str, Counter[str]]]:
    if not allowed:
        return items
    out: list[tuple[str, str, Counter[str]]] = []
    for oem, model, counter in items:
        filtered = Counter({brand: count for brand, count in counter.items() if normalize_brand(brand) in allowed})
        if filtered:
            out.append((oem, model, filtered))
    return out


def filter_conflict_pk(
    items: list[tuple[str, Counter[str]]],
    allowed: set[str],
) -> list[tuple[str, Counter[str]]]:
    if not allowed:
        return items
    out: list[tuple[str, Counter[str]]] = []
    for prefix, counter in items:
        filtered = Counter({normalize_brand(brand): count for brand, count in counter.items() if normalize_brand(brand) in allowed})
        if filtered:
            out.append((prefix, filtered))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Derive conservative exact-brand tables from Firehose CSV")
    ap.add_argument("csv_path", type=Path, help="build/firehose_msm.csv")
    ap.add_argument("--chip-db", type=Path, required=True, help="core/src/chip_db.c")
    ap.add_argument("--report", type=Path, required=True, help="human-readable report path")
    ap.add_argument("--snippets", type=Path, help="optional C snippet output path")
    ap.add_argument("--brands", action="append", default=[],
                    help="filter report/snippets to one or more brands or families, comma-separated")
    args = ap.parse_args()

    if not args.csv_path.is_file():
        print(f"missing csv: {args.csv_path}", file=sys.stderr)
        return 2
    if not args.chip_db.is_file():
        print(f"missing chip_db: {args.chip_db}", file=sys.stderr)
        return 2

    rows = load_rows(args.csv_path)
    existing_oem_model = load_existing_exact_oem_model(args.chip_db)
    existing_pk = load_existing_exact_pk(args.chip_db)

    stable_oem_model, conflict_oem_model = stable_oem_model_candidates(rows)
    stable_pk, conflict_pk = stable_pk_candidates(rows)

    allowed_brands = expand_brand_filters(args.brands)

    missing_oem_model = [
        item
        for item in stable_oem_model
        if existing_oem_model.get((item.oem, item.model)) != item.brand
    ]

    missing_pk = [
        item
        for item in stable_pk
        if existing_pk.get(item.prefix) != item.brand
    ]
    strong_pk = [item for item in missing_pk if item.count >= 2]
    weak_pk = [item for item in missing_pk if item.count < 2]

    missing_oem_model = filter_candidates_by_brand(missing_oem_model, allowed_brands, lambda x: x.brand)
    strong_pk = filter_candidates_by_brand(strong_pk, allowed_brands, lambda x: x.brand)
    weak_pk = filter_candidates_by_brand(weak_pk, allowed_brands, lambda x: x.brand)
    conflict_oem_model = filter_conflict_oem_model(conflict_oem_model, allowed_brands)
    conflict_pk = filter_conflict_pk(conflict_pk, allowed_brands)

    emit_report(
        args.report,
        rows,
        missing_oem_model,
        conflict_oem_model,
        strong_pk,
        weak_pk,
        conflict_pk,
    )
    if args.snippets:
        emit_snippets(args.snippets, missing_oem_model, strong_pk)

    print(f"wrote {args.report}", file=sys.stderr)
    if args.snippets:
        print(f"wrote {args.snippets}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
