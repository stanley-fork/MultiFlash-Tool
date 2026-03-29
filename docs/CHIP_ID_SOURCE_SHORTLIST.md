# Chip ID Data Source Shortlist

This note tracks open-source/public sources that are useful for improving:

- `MSM ID -> chip name`
- `OEM ID / Model ID -> brand`
- `PK Hash prefix -> brand`
- `codename <-> marketing model <-> SoC`
- loader directory / filename hints for vendor-family grouping

The current project already uses local tooling around `Firehose-Finder` samples.
This file documents the next sources worth mining and how to use them safely.

## Priority

### P0: already proven useful

1. `bkerler/edl`
   Link: <https://github.com/bkerler/edl>
   Best for:
   - baseline `msmids`
   - baseline `vendor{}` / OEM mapping
   - Sahara / Firehose protocol behavior notes
   Use as:
   - reference baseline
   - diff source for new `MSM ID` aliases
   Risk:
   - GPL-3.0; treat as reference input, avoid blind code copy into non-GPL parts

2. `hoplik/Firehose-Finder`
   Link: <https://github.com/hoplik/Firehose-Finder>
   Best for:
   - `ForFilter.xml`
   - `fh_collection/`
   - `PK Hash + HWID + marketing model` correlations
   Use as:
   - primary exact-evidence source for `OEM + Model -> Brand`
   - strongest source for new `PK Hash prefix -> Brand`
   - loader naming and real-world sample clustering
   Risk:
   - community dataset; conflicts exist, so do not auto-trust every row

3. `bkerler/Loaders`
   Link: <https://github.com/bkerler/Loaders>
   Best for:
   - vendor-family directory layout
   - loader filename patterns
   - fallback loader provenance for brand-family grouping
   Use as:
   - secondary evidence for vendor family
   - good for `Lenovo / Motorola`, `Huawei`, `Xiaomi`, `vivo`, `OPPO`, `OnePlus`
   Risk:
   - useful for clustering, but not a strong standalone source for exact brand labeling

### P1: best codename / model cross-check sources

4. `LineageOS` device trees
   Root: <https://github.com/LineageOS/android>
   Example device repo: <https://github.com/LineageOS/android_device_xiaomi_diting>
   Best for:
   - codename
   - marketing name
   - SoC string in `README.md`
   - common-device grouping like `sm8550-common`, `sm7435-common`
   Use as:
   - confirmation source for precise codename tables
   - SoC to marketing-family sanity check
   Risk:
   - coverage is strong for Xiaomi / Motorola / OnePlus / some OPPO, uneven for Huawei / vivo

5. `LineageOS` common repos
   Examples:
   - <https://github.com/LineageOS/android_hardware_xiaomi>
   - <https://github.com/LineageOS/android_device_oppo_common>
   Best for:
   - family-level grouping
   - common platform naming
   Use as:
   - family inference only
   - not enough by itself for exact single-model labeling

### P2: fallback device-tree sources when Lineage is missing

6. `TeamWin` device trees
   Org: <https://github.com/TeamWin>
   Example: <https://github.com/TeamWin/android_device_xiaomi_tapas>
   Best for:
   - codename
   - marketing name
   - recovery-maintained Qualcomm device coverage
   Use as:
   - fallback codename source when LineageOS has no tree

7. `OrangeFoxRecovery` device trees
   Org: <https://github.com/OrangeFoxRecovery>
   Examples:
   - <https://github.com/OrangeFoxRecovery/device_xiaomi_miatoll>
   - <https://github.com/OrangeFoxRecovery/device_xiaomi_garden>
   Best for:
   - Xiaomi / Redmi / POCO codename bundles
   - unified tree naming
   Use as:
   - fallback for Xiaomi ecosystem codename and model-family coverage
   Risk:
   - recovery trees often unify several models; good for family grouping, not always per-model exactness

## What to import from each source

### Safe to auto-import

- `bkerler/edl`
  - new `MSM ID -> chip name` aliases
  - new `OEM ID -> OEM family`

- `Firehose-Finder`
  - exact `PK Hash prefix -> Brand` only when sample brand is stable
  - exact `OEM + Model -> Brand` only when brand is unique
  - real-world `HWID -> marketing model` sample inventory

- `LineageOS` / `TeamWin` / `OrangeFox`
  - `codename -> marketing name`
  - `codename -> SoC`
  - common-tree family grouping

### Use only as secondary confirmation

- `bkerler/Loaders`
  - vendor-family by folder name
  - filename hints like `prog_ufs_firehose_sm8550_*`

- recovery / device trees without README SoC lines
  - use only when `BoardConfig.mk`, `AndroidProducts.mk`, or repo title clearly names codename and model family

## Import policy

### Exact brand display

Only display exact brand when at least one of these is true:

1. `PK Hash prefix` is stable and not conflicting across known samples.
2. `OEM + Model ID` maps to a single brand in known samples.
3. `OEM + Model ID` and loader-source family both agree on the same brand.

Otherwise fall back to family-level output:

- `Lenovo / Motorola`
- `Xiaomi / Redmi / POCO`
- `Vivo / iQOO`
- `Honor / Huawei`
- `OPLUS`

### Exact codename display

Only display codename when the mapping is explicit in:

- device tree README / repo name
- or a stable curated table built from two agreeing sources

Do not infer codename from marketing name alone.

## Recommended next batch

### Highest ROI

1. Expand `OEM + Model -> Brand` from `Firehose-Finder` for:
   - `Lenovo / Motorola`
   - `Xiaomi / Redmi / POCO`
   - `Vivo / iQOO`

2. Expand `codename_precise_table[]` from `LineageOS` device trees for:
   - Xiaomi
   - Motorola
   - OnePlus
   - OPPO

3. Use `bkerler/Loaders` only to confirm vendor-family when `PK Hash` is unavailable.

### Keep conservative

- `Honor / Huawei`: do not auto-split mixed evidence like shared OEM families unless `Model ID` is stable.
- `Xiaomi / Redmi / POCO`: prefer exact `PK Hash` or explicit model families, because OEM alone is too broad.
- `vivo / iQOO`: same rule; exact split only when `PK Hash` or `Model ID` is stable.

## Suggested tooling next

Add a small offline import pipeline that:

1. ingests `Firehose-Finder` CSV/XML
2. computes stable `OEM + Model -> Brand` pairs
3. computes conflicting `PK Hash prefix` entries
4. emits:
   - exact display table
   - family-only fallback report
   - manual-review queue

This is safer than directly appending rows by hand.

Current repo implementation:

- `tools/derive_precise_brand_tables.py`
  - emits a conservative exact-brand report
  - emits missing exact-table snippets
  - keeps weak PK-prefix candidates separate from strong candidates
  - supports `--brands` for family-focused review queues
