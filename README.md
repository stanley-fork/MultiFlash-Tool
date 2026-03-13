<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**开源 Windows 桌面工具，支持高通 EDL、联发科 MTK、Fastboot 等多种模式的刷机与设备管理。**

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

---

## 简介

SakuraEDL 是一款面向 Windows 的安卓刷机与救砖工具，通过 USB 连接设备，支持：

- **高通 EDL (9008)**：Sahara / Firehose 协议，分区读写、GPT 备份、固件解密等
- **联发科 (MTK)**：BROM / Preloader，XFlash 与 XML 双协议，DA 加载与漏洞利用
- **Fastboot**：分区读写、OEM 解锁、设备信息、华为/荣耀等厂商扩展

适用于个人学习、研究、救砖与刷机，需自行准备固件与驱动。

---

## 功能概览

| 平台 | 主要能力 |
|------|----------|
| **高通 EDL** | Sahara V2/V3、Firehose 刷写、GPT 备份/恢复、eMMC/UFS 检测、OFP/OZIP/OPS 解密、Diag（IMEI/MEID/QCN）、Loader 特性检测、Motorola 固件 |
| **MTK** | BROM/Preloader、XFlash + XML 双协议、DA 加载、Carbonara/AllinoneSignature 漏洞、CRC32 校验 |
| **Fastboot** | 分区读写、OEM 解锁/重锁、设备信息、华为/荣耀（FRP、Device ID、Bootloader 解锁） |

### 高通 EDL 认证方式

部分设备在 EDL 下需先通过厂商认证才能进行分区读写，本工具支持多种认证模式，连接时可按设备选择：

| 模式 | 说明 |
|------|------|
| **无认证 (none)** | 标准 Firehose，无需 digest/signature，适用于多数公开 Loader 设备。 |
| **VIP / OPLUS** | 通用 VIP 分区认证：先提交 digest 与 signature，通过后再进行 configure 与读写。常见于 OPPO/一加等部分 Loader。 |
| **一加 (OnePlus / Demacia)** | 一加专用流程，在 Firehose configure 之后执行认证。 |
| **小米 (Xiaomi)** | 小米 EDL 认证，部分机型可自动识别并提示输入账号 Token 完成认证。 |
| **厂商专用 (如 Realme)** | 厂商自有 Loader 的认证流程，可能包含多种子类型：**Modern**（新机型，Sahara 后发 digest）、**Legacy initdigest**（getstorageinfo + initdigest）、**Legacy 简化**（仅 nop → configure → getsigndata）。程序会根据 Loader 返回的 banner 自动识别子类型并走对应流程。 |

连接前在「认证模式」下拉框中选择与设备匹配的选项；若不确定，可先选「无认证」，无法读写时再尝试对应厂商模式。

### 通用能力

- 多语言界面（中文/英文/日韩俄西等）
- 云端 Loader 匹配（高通，按芯片 ID 获取 Loader）
- Payload.bin 解析、Super 分区合并、Sparse/Raw 转换、rawprogram 解析

---

## 环境与运行

- **系统**：Windows 10/11 (64 位)
- **运行时**：.NET 8（Windows Forms）
- **驱动**：高通 9008、MTK PreLoader、ADB/Fastboot 等按需安装

### 快速开始

1. 从 [Releases](https://github.com/xiriovo/SakuraEDL/releases) 下载最新版本并解压（路径建议使用英文）。
2. 按设备平台安装对应 USB 驱动。
3. 运行 `SakuraEDL.exe`，选择端口与模式后操作。

### 从源码构建

```bash
# 需安装 .NET 8 SDK
dotnet restore
dotnet build -c Release
# 输出在 bin/Release/net8.0-windows/
```

- **公开/GitHub 构建**：默认已排除 Realme 认证相关逻辑，直接 `dotnet build` 即可。
- **含 Realme 认证的完整构建**：在具备相应源文件时使用  
  `dotnet build -c Release -p:ExcludeRealmeAuth=false`。

详见 [DEVELOPER.md](DEVELOPER.md)。

---

## 项目结构

```
SakuraEDL/
├── Qualcomm/          # 高通 EDL：Sahara、Firehose、Diag、设备信息等
├── MediaTek/           # 联发科：BROM、XFlash、XML、DA、漏洞利用
├── Fastboot/           # Fastboot 协议与厂商扩展（如华为/荣耀）
├── Common/             # 公共：语言、看门狗、配置等
├── Libs/               # 第三方/本地库
├── Properties/         # 程序集与配置
├── assets/             # 图标、截图等资源
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## 常见问题

- **高通 9008 无法连接**：确认设备处于 EDL 模式、已安装 Qualcomm HS-USB 驱动，必要时更换 USB 口或线缆。
- **MTK 无法识别**：安装 MediaTek PreLoader 驱动，关机后按音量键再连 USB，确认为 BROM 模式。
- **端口断开后无法重连**：程序会在断开时释放端口；若仍占用，可重启程序或重新插拔设备。

---

## 许可证

本项目采用 [MIT 许可证](LICENSE)：允许使用、修改与再分发，须保留版权与许可声明。详见 [LICENSE](LICENSE)。

---

## 致谢与参考

- [edl](https://github.com/bkerler/edl) — Qualcomm EDL 参考
- [mtkclient](https://github.com/bkerler/mtkclient) — MTK 协议参考

---

## 联系方式

- **QQ 群**：[SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**：[@xiriery](https://t.me/xiriery)
- **GitHub**：[@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
