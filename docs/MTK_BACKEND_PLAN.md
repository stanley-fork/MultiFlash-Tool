# MTK Backend Plan

## Goal

Add an MTK low-level backend without touching UI integration and without affecting the default application build.

## Current state

- New optional static library: `mtk_core`
- Default build behavior unchanged (`EDL_ENABLE_MTK_CORE=OFF`)
- No UI linkage, no Qt dependency
- Service-first low-level API established under `mtk_core/include/mtk`
- Windows transport implemented for COM access
- Windows MTK port detection implemented for common MediaTek VID/PID pairs
- Boot-stage detection now combines port enumeration + conservative BROM sync probe
- Security/capability model now derives from detected boot stage
- DA selection now requires `da_path` and chooses a provisional mode (`legacy` / `xflash`) based on stage/security hints

## Layering

```text
mtk_transport  -> raw transport / port ownership
mtk_boot       -> BROM / Preloader stage detection
mtk_security   -> SLA / DAA / capability probing
mtk_da         -> DA upload / DA session abstraction
mtk_storage    -> block-level storage access
mtk_partition  -> partition-table abstraction
mtk_service    -> high-level orchestration facade
```

## Why this shape

This mirrors the flow observed in mtkclient:

1. Detect and open transport
2. Detect boot stage
3. Probe security restrictions
4. Upload/init DA
5. Probe storage
6. Read partition table
7. Read/write/erase partitions

## Intentional limitations in current scaffold

Current implementation is only a compile-safe scaffold:

- transport is placeholder only
- stage detection is placeholder only
- security probing is placeholder only
- DA upload is placeholder only
- block read/write/erase return `MTK_E_NOT_IMPLEMENTED`
- partition table returns a placeholder item

This is intentional: it keeps current app build unaffected while establishing stable interfaces for incremental migration of MTK logic.

## Next implementation order

1. `mtk_transport`:
   - real Windows USB/serial transport
   - timeout handling
   - framing helpers
2. `mtk_boot`:
   - BROM / Preloader handshake
   - hardware code detection
3. `mtk_security`:
   - SLA / DAA state detection
   - capability derivation
4. `mtk_da`:
   - DA selection/config
   - legacy / xflash / xmlflash strategy split
5. `mtk_storage`:
   - block read/write/erase
6. `mtk_partition`:
   - GPT / PMT / region mapping as required

## Build

Enable with:

```cmake
-DEDL_ENABLE_MTK_CORE=ON
```

Default remains OFF.
