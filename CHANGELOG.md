# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and this project aims to follow Semantic Versioning for tagged releases.

## [Unreleased]

### Added

- MIT license for the public repository
- Windows GitHub Actions build workflow for the Qt/CMake codebase
- More polished GitHub-facing README layout with quick links, feature cards, and contributor-facing project guidance
- Formal repository disclaimer for safety, liability, and authorized-use boundaries

### Changed

- Refined repository presentation for open-source distribution
- Clarified repository policy around excluding build outputs, firmware packages, and private signing material
- Made the disclaimer and maintainer contact visible directly from the main GitHub README

## [4.0.0] - 2026-03-29

### Added

- Native Qt/CMake SakuraEDL desktop application
- Qualcomm Sahara and Firehose backend in `core/`
- GPT parsing, partition operations, rawprogram / patch XML handling, and storage reporting
- Android property extraction from ext4 and EROFS partition data
- Vendor-specific authentication paths for Realme / OPLUS, Xiaomi, and OnePlus
- Experimental MTK low-level backend in `mtk_core/`
- Static Qt build documentation and helper scripts

### Changed

- Repository mainline moved to the native C/C++ and Qt implementation
- Project layout reorganized around `core/`, `mtk_core/`, `docs/`, `scripts/`, and Qt UI sources

### Notes

- Public repository contents are source-only by design
- Build artifacts, firmware packages, loader binaries, and private signing material are intentionally excluded
