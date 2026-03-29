# SakuraEDL

[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![CMake](https://img.shields.io/badge/CMake-3.16+-064F8C?logo=cmake)](https://cmake.org/)
[![Qt](https://img.shields.io/badge/Qt-6-41CD52?logo=qt)](https://www.qt.io/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

SakuraEDL is a Windows desktop tool for Qualcomm EDL service workflows. It uses a Qt Widgets frontend and a native C/C++ backend for Sahara, Firehose, GPT handling, partition read/write operations, storage reporting, and Android property extraction from device partitions.

The current `main` branch tracks the native Qt/CMake implementation of SakuraEDL.

## What It Does

- Connects to Qualcomm devices in EDL mode and negotiates Sahara / Firehose sessions
- Reads GPT layouts across UFS and eMMC devices, including multi-LUN targets
- Reads, writes, erases, and zeroes partitions
- Parses `rawprogram` and `patch` XML workflows
- Extracts Android system information from ext4 and EROFS partition content
- Includes vendor-specific authentication and compatibility logic for Realme / OPLUS, Xiaomi, and OnePlus device families
- Provides a Windows GUI for device inspection, flashing, reboot actions, storage information, and service tasks

## Project Status

| Area | Status |
|------|--------|
| Qualcomm EDL backend | Active |
| Qt desktop UI | Active |
| Android property parsing | Active |
| Vendor auth integrations | Active |
| MTK backend | Experimental |

## Feature Overview

### Qualcomm EDL

- Sahara handshake and chip identification
- Firehose loader configuration and session management
- GPT parsing and partition discovery
- Partition flashing and partition readback
- Storage reporting for UFS and eMMC
- XML-driven flashing workflows

### Android System Info Extraction

- Property probing from partition content without booting Android
- ext4 and EROFS scanning paths
- Overlay-style property resolution across multiple partitions
- Device branding, model, build, OTA, region, locale, and vendor-specific metadata extraction

### Vendor Authentication

- Realme cloud-signing flow support
- OPLUS VIP-style file-based flow support
- OnePlus-specific authentication path
- Xiaomi-specific authentication path

## Realme Authentication

SakuraEDL includes a dedicated Realme authentication path for devices that require authorization after Firehose configuration.

- Modern Realme devices use a `getsigndata -> cloud signing -> verify` flow and require a valid `ProjectID`.
- Some legacy Realme devices require an additional `initdigest` step before `getsigndata`, so a digest file must be supplied.
- Realme authentication in this codebase is designed around a backend signing callback. Private credentials, tokens, and signing material are intentionally not stored in this public repository.
- Realme authentication is separate from the OPLUS VIP flow. OPLUS VIP is file-based and uses local digest / signature material, while Realme depends on server-side signing.
- When a session is already in a direct Firehose state without the required Sahara / serial-side authentication context, the Realme serial authentication step is skipped.

## Build Requirements

- Windows 10 or Windows 11
- CMake 3.16 or newer
- Ninja
- MSVC toolchain
- Qt 6 Widgets / Svg / Network
- Optional: `vcpkg` for static-Qt builds

## Build From Source

### Standard Build

```powershell
cmake --preset default
cmake --build --preset default
```

### Static Qt via vcpkg

```powershell
cmake --preset vcpkg-static-md
cmake --build --preset vcpkg-static-md
```

If you use a custom static Qt installation instead of `vcpkg`, review:

- [`docs/BUILD_STATIC.md`](docs/BUILD_STATIC.md)

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
- [`docs/REALME_VS_OPLUS_VIP.md`](docs/REALME_VS_OPLUS_VIP.md)
- [`docs/CHIP_ID_SOURCE_SHORTLIST.md`](docs/CHIP_ID_SOURCE_SHORTLIST.md)

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

## Contributing

Issues and pull requests are welcome. For code contributions:

- keep changes focused and reviewable
- avoid committing build outputs or firmware data
- update documentation when behavior changes
- preserve existing device-safety checks and logging clarity

## License

This project is licensed under the MIT License. See [`LICENSE`](LICENSE).
