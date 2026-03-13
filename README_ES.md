<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**Herramienta de escritorio Windows de código abierto para flashear y gestionar dispositivos: Qualcomm EDL, MediaTek (MTK), Fastboot y más.**

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## Introducción

SakuraEDL es una herramienta Windows para flashear y recuperar dispositivos Android. Conexión por USB, soporta:

- **Qualcomm EDL (9008)**: protocolos Sahara / Firehose, lectura/escritura de particiones, copia de seguridad GPT, descifrado de firmware, etc.
- **MediaTek (MTK)**: BROM / Preloader, doble protocolo XFlash y XML, carga de DA y exploits
- **Fastboot**: lectura/escritura de particiones, desbloqueo OEM, información del dispositivo, extensiones Huawei/Honor y otras

Para aprendizaje personal, investigación y flasheo. Debes proporcionar tu propio firmware y controladores.

---

## Resumen de funciones

| Plataforma | Capacidades principales |
|------------|--------------------------|
| **Qualcomm EDL** | Sahara V2/V3, flasheo Firehose, copia/restauración GPT, detección eMMC/UFS, descifrado OFP/OZIP/OPS, Diag (IMEI/MEID/QCN), detección de capacidades Loader, firmware Motorola |
| **MTK** | BROM/Preloader, doble protocolo XFlash + XML, carga DA, exploits Carbonara/AllinoneSignature, checksum CRC32 |
| **Fastboot** | Lectura/escritura de particiones, desbloqueo/bloqueo OEM, información del dispositivo, Huawei/Honor (FRP, Device ID, desbloqueo bootloader) |

### Modos de autenticación Qualcomm EDL

Algunos dispositivos requieren autenticación del fabricante en EDL antes de leer/escribir particiones. La herramienta soporta varios modos; elige uno al conectar.

| Modo | Descripción |
|------|-------------|
| **Ninguno (none)** | Firehose estándar, sin digest/signature; para la mayoría de dispositivos con Loader público. |
| **VIP / OPLUS** | Autenticación genérica de partición VIP: primero enviar digest y signature, luego configure y lectura/escritura. Común en algunos Loader OPPO/OnePlus. |
| **OnePlus / Demacia** | Flujo específico OnePlus; la autenticación se ejecuta después de Firehose configure. |
| **Xiaomi** | Autenticación EDL Xiaomi; en algunos modelos se detecta automáticamente y se pide el token de cuenta. |
| **Fabricante específico (ej. Realme)** | Autenticación Loader del fabricante, con subtipos: **Modern** (dispositivos nuevos, digest tras Sahara), **Legacy initdigest** (getstorageinfo + initdigest), **Legacy simplificado** (solo nop → configure → getsigndata). La aplicación detecta el subtipo por el banner del Loader y sigue el flujo correspondiente. |

Antes de conectar, selecciona el modo adecuado en el desplegable «Modo de autenticación». Si no estás seguro, prueba primero «Ninguno»; si no puede leer/escribir, prueba el modo del fabricante.

### General

- Interfaz multidioma (español, inglés, chino, japonés, coreano, ruso)
- Coincidencia de Loader en la nube (Qualcomm; obtener Loader por ID de chip)
- Análisis Payload.bin, fusión de partición Super, conversión Sparse/Raw, análisis rawprogram

---

## Entorno y ejecución

- **SO**: Windows 10/11 (64 bits)
- **Runtime**: .NET 8 (Windows Forms)
- **Controladores**: Qualcomm 9008, MTK PreLoader, ADB/Fastboot según necesidad

### Inicio rápido

1. Descarga la última versión en [Releases](https://github.com/xiriovo/SakuraEDL/releases) y extrae (ruta en inglés recomendada).
2. Instala los controladores USB según tu plataforma.
3. Ejecuta `SakuraEDL.exe`, elige puerto y modo.

### Compilar desde código

```bash
# Requiere .NET 8 SDK
dotnet restore
dotnet build -c Release
# Salida: bin/Release/net8.0-windows/
```

Ver [DEVELOPER.md](DEVELOPER.md).

---

## Estructura del proyecto

```
SakuraEDL/
├── Qualcomm/          # Qualcomm EDL: Sahara, Firehose, Diag, información de dispositivo
├── MediaTek/          # MediaTek: BROM, XFlash, XML, DA, exploits
├── Fastboot/          # Protocolo Fastboot y extensiones (Huawei/Honor, etc.)
├── Common/            # Común: idioma, watchdog, configuración
├── Libs/              # Bibliotecas de terceros y locales
├── Properties/       # Ensamblado y configuración
├── assets/           # Iconos, capturas
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## Preguntas frecuentes

- **Qualcomm 9008 no conecta**: comprueba que el dispositivo esté en modo EDL y que tengas el controlador Qualcomm HS-USB; prueba otro puerto o cable.
- **MTK no se detecta**: instala el controlador MediaTek PreLoader, apaga el dispositivo y conéctalo con el botón de volumen pulsado para entrar en BROM.
- **El puerto no se libera al desconectar**: la aplicación libera el puerto al desconectar; si sigue en uso, reinicia la aplicación o vuelve a conectar el dispositivo.

---

## Licencia

Este proyecto está bajo la [licencia MIT](LICENSE): se permite uso, modificación y redistribución manteniendo el aviso de copyright y licencia. Ver [LICENSE](LICENSE).

---

## Agradecimientos y referencias

- [edl](https://github.com/bkerler/edl) — referencia Qualcomm EDL
- [mtkclient](https://github.com/bkerler/mtkclient) — referencia protocolo MTK

---

## Contacto

- **QQ**: [SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**: [@xiriery](https://t.me/xiriery)
- **GitHub**: [@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
