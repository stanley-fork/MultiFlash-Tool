<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**Open-source Windows desktop tool for flashing and device management: Qualcomm EDL, MediaTek (MTK), Fastboot, and more.**

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## Introduction

SakuraEDL is a Windows Android flashing and unbrick tool. Connect via USB and use:

- **Qualcomm EDL (9008)**: Sahara / Firehose protocol, partition read/write, GPT backup, firmware decryption, etc.
- **MediaTek (MTK)**: BROM / Preloader, XFlash and XML dual protocol, DA loading and exploits
- **Fastboot**: Partition read/write, OEM unlock, device info, Huawei/Honor and other vendor extensions

Suitable for personal learning, research, unbrick and flashing. You need to provide your own firmware and drivers.

---

## Features Overview

| Platform | Main capabilities |
|----------|--------------------|
| **Qualcomm EDL** | Sahara V2/V3, Firehose flashing, GPT backup/restore, eMMC/UFS detection, OFP/OZIP/OPS decryption, Diag (IMEI/MEID/QCN), Loader feature detection, Motorola firmware |
| **MTK** | BROM/Preloader, XFlash + XML dual protocol, DA loading, Carbonara/AllinoneSignature exploits, CRC32 checksum |
| **Fastboot** | Partition read/write, OEM unlock/relock, device info, Huawei/Honor (FRP, Device ID, Bootloader unlock) |

### Qualcomm EDL authentication modes

Some devices require vendor authentication in EDL before partition read/write. The tool supports multiple auth modes; choose one when connecting:

| Mode | Description |
|------|--------------|
| **None** | Standard Firehose, no digest/signature; for most public Loader devices. |
| **VIP / OPLUS** | Generic VIP partition auth: submit digest and signature first, then configure and read/write. Common on some OPPO/OnePlus Loaders. |
| **OnePlus / Demacia** | OnePlus-specific flow; authentication runs after Firehose configure. |
| **Xiaomi** | Xiaomi EDL auth; some models are auto-detected and will prompt for account token. |
| **Vendor-specific (e.g. Realme)** | Vendor Loader auth, with sub-types: **Modern** (new devices, digest after Sahara), **Legacy initdigest** (getstorageinfo + initdigest), **Legacy simplified** (nop → configure → getsigndata only). The app detects the sub-type from the Loader banner and follows the matching flow. |

Select the matching mode in the "Authentication mode" dropdown before connecting; if unsure, try "None" first, then the vendor mode if read/write fails.

### General

- Multi-language UI (Chinese, English, Japanese, Korean, Russian, Spanish)
- Cloud Loader matching (Qualcomm; fetch Loader by chip ID)
- Payload.bin parsing, Super partition merge, Sparse/Raw conversion, rawprogram parsing

---

## Environment and run

- **OS**: Windows 10/11 (64-bit)
- **Runtime**: .NET 8 (Windows Forms)
- **Drivers**: Qualcomm 9008, MTK PreLoader, ADB/Fastboot as needed

### Quick start

1. Download the latest build from [Releases](https://github.com/xiriovo/SakuraEDL/releases) and extract (English path recommended).
2. Install the USB drivers for your device platform.
3. Run `SakuraEDL.exe`, select port and mode, then use the tool.

### Build from source

```bash
# Requires .NET 8 SDK
dotnet restore
dotnet build -c Release
# Output: bin/Release/net8.0-windows/
```

- **Public / GitHub build**: Realme auth is excluded by default; run `dotnet build` as above.
- **Full build with Realme auth**: use `dotnet build -c Release -p:ExcludeRealmeAuth=false` when you have the corresponding source files.

See [DEVELOPER.md](DEVELOPER.md).

---

## Project structure

```
SakuraEDL/
├── Qualcomm/          # Qualcomm EDL: Sahara, Firehose, Diag, device info
├── MediaTek/          # MediaTek: BROM, XFlash, XML, DA, exploits
├── Fastboot/          # Fastboot protocol and vendor extensions (e.g. Huawei/Honor)
├── Common/            # Shared: language, watchdog, config
├── Libs/              # Third-party / local libs
├── Properties/        # Assembly and config
├── assets/            # Icons, screenshots
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## FAQ

- **Qualcomm 9008 won’t connect**: Ensure device is in EDL mode and Qualcomm HS-USB driver is installed; try another USB port or cable.
- **MTK not detected**: Install MediaTek PreLoader driver, power off then hold volume and connect USB so BROM mode is used.
- **Port stays in use after disconnect**: The app releases the port on disconnect; if it’s still busy, restart the app or replug the device.

---

## License

This project is under the [MIT License](LICENSE): use, modify and redistribute allowed with copyright and license notice. See [LICENSE](LICENSE).

---

## Thanks and references

- [edl](https://github.com/bkerler/edl) — Qualcomm EDL reference
- [mtkclient](https://github.com/bkerler/mtkclient) — MTK protocol reference

---

## Contact

- **QQ**: [SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**: [@xiriery](https://t.me/xiriery)
- **GitHub**: [@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
