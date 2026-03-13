<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**오픈소스 Windows 데스크톱 도구. Qualcomm EDL, MediaTek(MTK), Fastboot 등 다양한 모드의 플래싱 및 기기 관리 지원.**

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## 소개

SakuraEDL은 Windows용 Android 플래싱·복구 도구입니다. USB로 연결하여 다음을 지원합니다.

- **Qualcomm EDL (9008)**: Sahara / Firehose 프로토콜, 파티션 읽기/쓰기, GPT 백업, 펌웨어 복호화 등
- **MediaTek (MTK)**: BROM / Preloader, XFlash 및 XML 듀얼 프로토콜, DA 로딩 및 익스플로잇
- **Fastboot**: 파티션 읽기/쓰기, OEM 잠금 해제, 기기 정보, Huawei/Honor 등 벤더 확장

개인 학습·연구·복구·플래싱에 사용할 수 있으며, 펌웨어와 드라이버는 직접 준비해야 합니다.

---

## 기능 개요

| 플랫폼 | 주요 기능 |
|--------|-----------|
| **Qualcomm EDL** | Sahara V2/V3, Firehose 플래싱, GPT 백업/복원, eMMC/UFS 감지, OFP/OZIP/OPS 복호화, Diag(IMEI/MEID/QCN), Loader 기능 감지, Motorola 펌웨어 |
| **MTK** | BROM/Preloader, XFlash + XML 듀얼 프로토콜, DA 로딩, Carbonara/AllinoneSignature 익스플로잇, CRC32 체크섬 |
| **Fastboot** | 파티션 읽기/쓰기, OEM 잠금 해제/잠금, 기기 정보, Huawei/Honor(FRP, Device ID, Bootloader 잠금 해제) |

### Qualcomm EDL 인증 방식

일부 기기는 EDL에서 파티션 읽기/쓰기 전에 벤더 인증이 필요합니다. 본 도구는 여러 인증 모드를 지원하며, 연결 시 선택할 수 있습니다.

| 모드 | 설명 |
|------|------|
| **없음 (none)** | 표준 Firehose. digest/signature 불필요. 대부분의 공개 Loader 기기에 적합. |
| **VIP / OPLUS** | 공통 VIP 파티션 인증. 먼저 digest와 signature 제출 후 configure 및 읽기/쓰기. OPPO/OnePlus 등 일부 Loader에서 사용. |
| **OnePlus / Demacia** | OnePlus 전용 흐름. Firehose configure 이후 인증 실행. |
| **Xiaomi** | Xiaomi EDL 인증. 일부 기기는 자동 감지되며 계정 Token 입력을 요청합니다. |
| **벤더 전용(예: Realme)** | 벤더 전용 Loader 인증. **Modern**(신형, Sahara 후 digest), **Legacy initdigest**(getstorageinfo + initdigest), **Legacy 간소**(nop → configure → getsigndata만) 등 하위 유형이 있으며, Loader banner로 자동 판별 후 해당 흐름을 따릅니다. |

연결 전 「인증 모드」에서 기기에 맞는 항목을 선택하세요. 잘 모르면 먼저 「없음」을 선택하고, 읽기/쓰기가 안 되면 해당 벤더 모드를 시도하세요.

### 공통 기능

- 다국어 UI(한국어·영어·중국어·일본어·러시아어·스페인어)
- 클라우드 Loader 매칭(Qualcomm, 칩 ID로 Loader 획득)
- Payload.bin 파싱, Super 파티션 병합, Sparse/Raw 변환, rawprogram 파싱

---

## 환경 및 실행

- **OS**: Windows 10/11(64비트)
- **런타임**: .NET 8(Windows Forms)
- **드라이버**: Qualcomm 9008, MTK PreLoader, ADB/Fastboot 등 필요 시 설치

### 빠른 시작

1. [Releases](https://github.com/xiriovo/SakuraEDL/releases)에서 최신 버전을 받아 압축 해제(경로는 영문 권장).
2. 기기 플랫폼에 맞는 USB 드라이버 설치.
3. `SakuraEDL.exe` 실행 후 포트와 모드를 선택해 사용.

### 소스에서 빌드

```bash
# .NET 8 SDK 필요
dotnet restore
dotnet build -c Release
# 출력: bin/Release/net8.0-windows/
```

자세한 내용은 [DEVELOPER.md](DEVELOPER.md).

---

## 프로젝트 구조

```
SakuraEDL/
├── Qualcomm/          # Qualcomm EDL: Sahara, Firehose, Diag, 기기 정보
├── MediaTek/          # MediaTek: BROM, XFlash, XML, DA, 익스플로잇
├── Fastboot/          # Fastboot 프로토콜 및 벤더 확장(Huawei/Honor 등)
├── Common/            # 공통: 언어, 워치독, 설정
├── Libs/              # 서드파티/로컬 라이브러리
├── Properties/        # 어셈블리 및 설정
├── assets/            # 아이콘, 스크린샷
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## 자주 묻는 질문

- **Qualcomm 9008 연결 안 됨**: 기기가 EDL 모드인지 확인하고 Qualcomm HS-USB 드라이버를 설치하세요. USB 포트나 케이블을 바꿔 보세요.
- **MTK 인식 안 됨**: MediaTek PreLoader 드라이버를 설치한 뒤 전원을 끄고 볼륨 키를 누른 상태로 USB를 연결해 BROM 모드로 들어가세요.
- **연결 해제 후 포트가 안 풀림**: 앱은 연결 해제 시 포트를 해제합니다. 여전히 사용 중이면 앱을 재시작하거나 기기를 뺐다 꽂아 보세요.

---

## 라이선스

본 프로젝트는 [MIT 라이선스](LICENSE)를 사용합니다. 사용·수정·재배포가 가능하며, 저작권 및 라이선스 표시를 유지해야 합니다. 자세한 내용은 [LICENSE](LICENSE).

---

## 감사 및 참고

- [edl](https://github.com/bkerler/edl) — Qualcomm EDL 참고
- [mtkclient](https://github.com/bkerler/mtkclient) — MTK 프로토콜 참고

---

## 연락처

- **QQ**: [SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**: [@xiriery](https://t.me/xiriery)
- **GitHub**: [@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
