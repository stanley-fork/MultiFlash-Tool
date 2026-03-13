<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**Открытое Windows-приложение для прошивки и управления устройствами: Qualcomm EDL, MediaTek (MTK), Fastboot и другие режимы.**

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## О проекте

SakuraEDL — инструмент для прошивки и восстановления Android под Windows. Подключение по USB, поддержка:

- **Qualcomm EDL (9008)**: протоколы Sahara / Firehose, чтение/запись разделов, резервная копия GPT, расшифровка прошивки и т.д.
- **MediaTek (MTK)**: BROM / Preloader, двойной протокол XFlash и XML, загрузка DA и эксплойты
- **Fastboot**: чтение/запись разделов, разблокировка OEM, информация об устройстве, расширения Huawei/Honor и др.

Предназначено для личного обучения, исследований и прошивки. Прошивку и драйверы нужно подбирать самостоятельно.

---

## Возможности

| Платформа | Основные функции |
|-----------|------------------|
| **Qualcomm EDL** | Sahara V2/V3, прошивка Firehose, резервная копия/восстановление GPT, определение eMMC/UFS, расшифровка OFP/OZIP/OPS, Diag (IMEI/MEID/QCN), определение возможностей Loader, прошивка Motorola |
| **MTK** | BROM/Preloader, двойной протокол XFlash + XML, загрузка DA, эксплойты Carbonara/AllinoneSignature, контрольная сумма CRC32 |
| **Fastboot** | Чтение/запись разделов, разблокировка/блокировка OEM, информация об устройстве, Huawei/Honor (FRP, Device ID, разблокировка загрузчика) |

### Режимы аутентификации Qualcomm EDL

На части устройств в EDL перед чтением/записью разделов нужна аутентификация производителя. В программе доступны несколько режимов; выберите подходящий при подключении.

| Режим | Описание |
|-------|----------|
| **Без аутентификации (none)** | Стандартный Firehose, без digest/signature; для большинства устройств с публичным Loader. |
| **VIP / OPLUS** | Общая аутентификация раздела VIP: сначала отправка digest и signature, затем configure и чтение/запись. Часто используется в части Loader OPPO/OnePlus. |
| **OnePlus / Demacia** | Специальный сценарий OnePlus; аутентификация выполняется после Firehose configure. |
| **Xiaomi** | Аутентификация Xiaomi EDL; на части моделей определяется автоматически и запрашивается токен учётной записи. |
| **Специфичная для производителя (напр. Realme)** | Аутентификация Loader производителя с подтипами: **Modern** (новые устройства, digest после Sahara), **Legacy initdigest** (getstorageinfo + initdigest), **Legacy упрощённый** (только nop → configure → getsigndata). Подтип определяется по banner Loader, выполняется соответствующий сценарий. |

Перед подключением выберите нужный режим в списке «Режим аутентификации». Если не уверены, сначала выберите «Без аутентификации», при ошибке чтения/записи попробуйте режим производителя.

### Общее

- Многоязычный интерфейс (русский, английский, китайский, японский, корейский, испанский)
- Подбор Loader из облака (Qualcomm, по ID чипа)
- Разбор Payload.bin, объединение раздела Super, конвертация Sparse/Raw, разбор rawprogram

---

## Требования и запуск

- **ОС**: Windows 10/11 (64-bit)
- **Среда**: .NET 8 (Windows Forms)
- **Драйверы**: Qualcomm 9008, MTK PreLoader, ADB/Fastboot по необходимости

### Быстрый старт

1. Скачайте последнюю версию из [Releases](https://github.com/xiriovo/SakuraEDL/releases) и распакуйте (путь без кириллицы желателен).
2. Установите USB-драйверы для вашей платформы.
3. Запустите `SakuraEDL.exe`, выберите порт и режим.

### Сборка из исходников

```bash
# Требуется .NET 8 SDK
dotnet restore
dotnet build -c Release
# Результат: bin/Release/net8.0-windows/
```

Подробнее в [DEVELOPER.md](DEVELOPER.md).

---

## Структура проекта

```
SakuraEDL/
├── Qualcomm/          # Qualcomm EDL: Sahara, Firehose, Diag, информация об устройстве
├── MediaTek/         # MediaTek: BROM, XFlash, XML, DA, эксплойты
├── Fastboot/         # Протокол Fastboot и расширения (Huawei/Honor и др.)
├── Common/           # Общее: язык, watchdog, настройки
├── Libs/             # Сторонние и локальные библиотеки
├── Properties/       # Сборка и конфигурация
├── assets/           # Иконки, скриншоты
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## Частые вопросы

- **Qualcomm 9008 не подключается**: проверьте, что устройство в режиме EDL и установлен драйвер Qualcomm HS-USB; попробуйте другой порт или кабель.
- **MTK не определяется**: установите драйвер MediaTek PreLoader, выключите устройство и подключите с зажатой клавишей громкости для входа в BROM.
- **Порт занят после отключения**: приложение освобождает порт при отключении; если порт всё ещё занят, перезапустите программу или переподключите устройство.

---

## Лицензия

Проект распространяется под [лицензией MIT](LICENSE): разрешены использование, изменение и распространение с сохранением уведомления об авторских правах и лицензии. Подробнее в [LICENSE](LICENSE).

---

## Благодарности и ссылки

- [edl](https://github.com/bkerler/edl) — справочник по Qualcomm EDL
- [mtkclient](https://github.com/bkerler/mtkclient) — справочник по протоколу MTK

---

## Контакты

- **QQ**: [SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**: [@xiriery](https://t.me/xiriery)
- **GitHub**: [@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
