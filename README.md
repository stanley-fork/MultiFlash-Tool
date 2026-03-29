# SakuraEDL

<p align="center">
  <strong>Native Windows Service Tool for Qualcomm EDL Workflows</strong><br/>
  Sahara, Firehose, GPT, partition operations, Android property extraction, and vendor authentication support in a single Qt desktop application.
</p>

<p align="center">
  <a href="https://github.com/xiriovo/SakuraEDL/actions/workflows/windows-build.yml"><img src="https://img.shields.io/github/actions/workflow/status/xiriovo/SakuraEDL/windows-build.yml?branch=main&label=Windows%20Build" alt="Windows Build"></a>
  <a href="https://github.com/xiriovo/SakuraEDL/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License"></a>
  <a href="https://www.qt.io/"><img src="https://img.shields.io/badge/Qt-6-41CD52?logo=qt" alt="Qt 6"></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-3.16+-064F8C?logo=cmake" alt="CMake"></a>
  <a href="https://www.microsoft.com/windows"><img src="https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows" alt="Windows"></a>
</p>

<p align="center">
  <a href="#build-from-source"><img src="https://img.shields.io/badge/Build-Quick%20Start-0F766E?style=for-the-badge" alt="Build Quick Start"></a>
  <a href="#feature-overview"><img src="https://img.shields.io/badge/Guide-Feature%20Overview-1D4ED8?style=for-the-badge" alt="Feature Overview"></a>
  <a href="#realme-authentication"><img src="https://img.shields.io/badge/Auth-Realme%20Flow-7C3AED?style=for-the-badge" alt="Realme Authentication"></a>
  <a href="docs/BUILD_STATIC.md"><img src="https://img.shields.io/badge/Docs-Static%20Qt-EF4444?style=for-the-badge" alt="Static Qt Docs"></a>
  <a href="DISCLAIMER.md"><img src="https://img.shields.io/badge/Notice-Disclaimer-B91C1C?style=for-the-badge" alt="Disclaimer"></a>
  <a href="CHANGELOG.md"><img src="https://img.shields.io/badge/Project-Changelog-374151?style=for-the-badge" alt="Changelog"></a>
</p>

> This repository tracks the native Qt/CMake implementation of SakuraEDL. Public source code and documentation are included. Build outputs, firmware packages, and private signing material are intentionally excluded.

## Product Overview

| Protocol Core | Device Insight | Service Operations |
| --- | --- | --- |
| Native Sahara and Firehose handling for Qualcomm EDL sessions, loader configuration, and partition transport. | GPT parsing, storage reporting, Android property extraction, and multi-partition metadata overlay logic. | Partition read/write/erase flows, XML-driven flashing, reboot helpers, and vendor-specific service paths. |

| Vendor Compatibility | Desktop UI | Build Flexibility |
| --- | --- | --- |
| Realme / OPLUS, Xiaomi, and OnePlus authentication and device-specific compatibility paths. | Qt Widgets interface for flashing, inspection, storage queries, and service actions on Windows. | Standard dynamic Qt builds and optional static Qt workflows through local Qt or `vcpkg`. |

## Feature Overview

### Qualcomm EDL

- Sahara handshake and chip identification
- Firehose loader configuration and session management
- GPT parsing and partition discovery
- Partition flashing, readback, erase, and zero-out operations
- XML-driven `rawprogram` and `patch` workflows
- Storage reporting for UFS and eMMC devices

### Android System Information

- Property probing from partition content without booting Android
- ext4 and EROFS scanning paths
- Overlay-style property resolution across multiple partitions
- Extraction of branding, model, device codename, OTA data, build fields, locale, region, and vendor-specific metadata

### Vendor Authentication

- Realme cloud-signing flow support
- OPLUS VIP-style file-based support
- OnePlus-specific authentication path
- Xiaomi-specific authentication path

### Experimental MTK Backend

- Low-level MTK backend scaffolding in `mtk_core/`
- Intended for future integration and protocol expansion

## Quick Start

### Build From Source

```powershell
cmake --preset default
cmake --build --preset default
```

### Static Qt via vcpkg

```powershell
cmake --preset vcpkg-static-md
cmake --build --preset vcpkg-static-md
```

Additional build notes:

- [`docs/BUILD_STATIC.md`](docs/BUILD_STATIC.md)

## Realme Authentication

SakuraEDL includes a dedicated Realme authentication path for devices that require authorization after Firehose configuration.

- Modern Realme devices use a `getsigndata -> cloud signing -> verify` flow and require a valid `ProjectID`.
- Some legacy Realme devices require an additional `initdigest` step before `getsigndata`, so a digest file must be supplied.
- Realme authentication in this codebase is designed around a backend signing callback. Private credentials, tokens, and signing material are intentionally not stored in this public repository.
- Realme authentication is separate from the OPLUS VIP flow. OPLUS VIP is file-based and uses local digest / signature material, while Realme depends on server-side signing.
- When a session is already in a direct Firehose state without the required Sahara / serial-side authentication context, the Realme serial authentication step is skipped.

Related note:

- [`docs/REALME_VS_OPLUS_VIP.md`](docs/REALME_VS_OPLUS_VIP.md)

## Build Requirements

- Windows 10 or Windows 11
- CMake 3.16 or newer
- Ninja
- MSVC toolchain
- Qt 6 Widgets / Svg / Network
- Optional: `vcpkg` for static-Qt builds

## Repository Layout

- `core/`: native EDL backend, protocol handlers, storage logic, filesystem parsers, and service layer
- `mtk_core/`: experimental MTK low-level backend
- `docs/`: build notes and implementation documents
- `tools/`: helper tools used during development and analysis
- `bundled/`: optional runtime assets that can be embedded into the executable when present
- `scripts/`: local build and maintenance scripts
- `icons/`, `style/`: UI assets and styling

## Included Documentation

- [`docs/SAHARA.md`](docs/SAHARA.md)
- [`docs/WRITE_PARTITION.md`](docs/WRITE_PARTITION.md)
- [`docs/BOOT_SLOT.md`](docs/BOOT_SLOT.md)
- [`docs/CHIP_ID_SOURCE_SHORTLIST.md`](docs/CHIP_ID_SOURCE_SHORTLIST.md)
- [`DISCLAIMER.md`](DISCLAIMER.md)
- [`CHANGELOG.md`](CHANGELOG.md)

## Repository Policy

This repository is intended to track source code, UI resources, and documentation.

Do not commit:

- build directories such as `build/`, `build-vcpkg-static/`, and `out/`
- dependency trees such as `vcpkg/` and `vcpkg_installed/`
- compiled outputs such as `.exe`, `.dll`, `.lib`, `.obj`, and `.pdb`
- firmware packages, dumps, images, loaders, and offline analysis artifacts
- machine-specific IDE state and local cache files

The `.gitignore` is configured to keep those files out of version control by default.

## Safety Notice

SakuraEDL is a low-level service tool. Incorrect flashing, partition writes, or vendor-auth misuse can permanently brick a device or destroy user data.

- Use it only on devices you own or are explicitly authorized to service.
- Verify loader compatibility, storage type, and target partition before writing.
- Keep backups of critical partitions whenever possible.

Formal disclaimer:

- [`DISCLAIMER.md`](DISCLAIMER.md)

## Contributing

Issues and pull requests are welcome. For code contributions:

- keep changes focused and reviewable
- avoid committing build outputs or firmware data
- update documentation when behavior changes
- preserve existing device-safety checks and logging clarity

## License

This project is licensed under the MIT License. See [`LICENSE`](LICENSE).
