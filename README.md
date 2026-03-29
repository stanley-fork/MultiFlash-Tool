# SakuraEDL

SakuraEDL is a Windows desktop utility for Qualcomm EDL workflows. It combines a Qt GUI with a native C/C++ backend for Sahara, Firehose, GPT, partition flashing, storage reporting, and Android property extraction from device partitions.

This repository is focused on source code. Build outputs, local caches, firmware packages, and other large binary artifacts should stay out of version control.

## Highlights

- Qualcomm EDL connection flow with Sahara and Firehose support
- GPT reading across multi-LUN UFS and eMMC devices
- Partition read, write, erase, zero-out, and rawprogram / patch XML handling
- Android system information parsing from ext4 / EROFS partition content
- Vendor-specific authentication and compatibility logic for devices such as Realme / OPLUS, Xiaomi, and OnePlus
- Qt Widgets desktop UI for flashing, inspection, and service operations
- Optional static Qt build path for producing a more self-contained Windows executable

## Repository Layout

- `core/`: native EDL backend, protocol handlers, storage logic, filesystem parsers, and service layer
- `mtk_core/`: experimental MTK low-level backend
- `docs/`: build notes and implementation documents
- `tools/`: helper tools used during development and analysis
- `bundled/`: optional runtime assets that can be embedded into the executable when present
- `scripts/`: local build and maintenance scripts

## Build Requirements

- Windows
- CMake 3.16+
- MSVC toolchain
- Qt 6 Widgets / Svg / Network
- Optional: `vcpkg` for dependency management and static Qt workflows

The project is built with CMake. Typical local builds use a separate build directory such as `build/` or `build-vcpkg-static/`.

## Quick Start

```powershell
cmake -B build -G Ninja
cmake --build build --config Release
```

For static Qt and vcpkg-based builds, see:

- `docs/BUILD_STATIC.md`

## Optional Bundled Assets

The application can embed optional helper files from `bundled/` during configuration time, including `dwebp.exe` and selected `misc*.img` files. These files are runtime assets, not source code, and should not be committed unless you intentionally want to distribute them from the repository.

## Realme Authentication

SakuraEDL includes a dedicated Realme authentication path for devices that require vendor-specific authorization after Firehose configuration.

- Modern Realme devices use a `getsigndata -> cloud signing -> verify` flow and require a valid `ProjectID`.
- Some legacy Realme devices use an additional `initdigest` step before `getsigndata`, which means a digest file must be supplied.
- Authentication is handled through a cloud-signing callback in the backend. Credentials, tokens, or private signing material are not intended to be stored in this public repository.
- Realme authentication is separate from the OPLUS VIP flow. OPLUS VIP is file-based and uses local digest / signature material, while Realme uses server-side signing.
- When the session is already in a direct Firehose state without the required Sahara / serial-side authentication context, the Realme serial authentication step is skipped.

This separation matters in practice: do not treat OPLUS VIP digest/signature files as Realme cloud credentials, and do not assume the same authentication path works across all OPLUS-family devices.

## GitHub Submission Rules

Only submit source code, UI files, build scripts, and documentation.

Do not submit:

- build directories such as `build/`, `build-vcpkg-static/`, and `out/`
- local dependency trees such as `vcpkg/` and `vcpkg_installed/`
- compiled outputs such as `.exe`, `.dll`, `.lib`, `.obj`, `.pdb`
- firmware packages, loaders, dumps, images, archives, and offline analysis data
- local IDE state and machine-specific cache files

The `.gitignore` in this repository is set up to keep those files out of Git by default.

## Notes

- Some bundled runtime assets are optional and may be placed beside the executable instead of being versioned.
- Some vendor authentication flows depend on external services or locally supplied material.
- The repository currently contains internal development documents in `docs/`; they are useful for contributors but are not required for ordinary end users.
